#include <jsi/jsi.h>
#include <reanimated/NativeModules/ReanimatedModuleProxy.h>
#include <reanimated/RuntimeDecorators/UIRuntimeDecorator.h>
#include <reanimated/Tools/FeatureFlags.h>
#include <reanimated/Tools/ReanimatedSystraceSection.h>

#include <worklets/Registries/EventHandlerRegistry.h>
#include <worklets/SharedItems/Shareables.h>
#include <worklets/Tools/AsyncQueue.h>
#include <worklets/Tools/WorkletEventHandler.h>

#ifdef __ANDROID__
#include <fbjni/fbjni.h>
#endif // __ANDROID__

#include <react/renderer/scheduler/Scheduler.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/primitives.h>

#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace reanimated {

ReanimatedModuleProxy::ReanimatedModuleProxy(
    const std::shared_ptr<WorkletsModuleProxy> &workletsModuleProxy,
    jsi::Runtime &rnRuntime,
    const std::shared_ptr<CallInvoker> &jsCallInvoker,
    const PlatformDepMethodsHolder &platformDepMethodsHolder,
    const bool isReducedMotion)
    : ReanimatedModuleProxySpec(jsCallInvoker),
      isReducedMotion_(isReducedMotion),
      workletsModuleProxy_(workletsModuleProxy),
      eventHandlerRegistry_(std::make_unique<EventHandlerRegistry>()),
      requestRender_(platformDepMethodsHolder.requestRender),
      animatedSensorModule_(platformDepMethodsHolder),
      jsLogger_(
          std::make_shared<JSLogger>(workletsModuleProxy->getJSScheduler())),
      layoutAnimationsManager_(
          std::make_shared<LayoutAnimationsManager>(jsLogger_)),
      getAnimationTimestamp_(platformDepMethodsHolder.getAnimationTimestamp),
      animatedPropsRegistry_(std::make_shared<AnimatedPropsRegistry>()),
      staticPropsRegistry_(std::make_shared<StaticPropsRegistry>()),
      updatesRegistryManager_(
          std::make_shared<UpdatesRegistryManager>(staticPropsRegistry_)),
      cssAnimationKeyframesRegistry_(std::make_shared<CSSKeyframesRegistry>()),
      cssAnimationsRegistry_(std::make_shared<CSSAnimationsRegistry>()),
      cssTransitionsRegistry_(std::make_shared<CSSTransitionsRegistry>(
          staticPropsRegistry_,
          getAnimationTimestamp_)),
      viewStylesRepository_(std::make_shared<ViewStylesRepository>(
          staticPropsRegistry_,
          animatedPropsRegistry_)),
      subscribeForKeyboardEventsFunction_(
          platformDepMethodsHolder.subscribeForKeyboardEvents),
      unsubscribeFromKeyboardEventsFunction_(
          platformDepMethodsHolder.unsubscribeFromKeyboardEvents) {
  auto lock = updatesRegistryManager_->lock();
  // Add registries in order of their priority (from the lowest to the
  // highest)
  // CSS transitions should be overriden by animated style animations;
  // animated style animations should be overriden by CSS animations
  updatesRegistryManager_->addRegistry(cssTransitionsRegistry_);
  updatesRegistryManager_->addRegistry(animatedPropsRegistry_);
  updatesRegistryManager_->addRegistry(cssAnimationsRegistry_);
}

void ReanimatedModuleProxy::init(
    const PlatformDepMethodsHolder &platformDepMethodsHolder) {
  auto onRenderCallback = [weakThis =
                               weak_from_this()](const double timestampMs) {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return;
    }

    strongThis->renderRequested_ = false;
    strongThis->onRender(timestampMs);
  };
  onRenderCallback_ = std::move(onRenderCallback);

  auto updateProps = [weakThis = weak_from_this()](
                         jsi::Runtime &rt, const jsi::Value &operations) {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return;
    }

    strongThis->animatedPropsRegistry_->update(rt, operations);
  };

  auto measure = [weakThis = weak_from_this()](
                     jsi::Runtime &rt,
                     const jsi::Value &shadowNodeValue) -> jsi::Value {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return jsi::Value::undefined();
    }
    return strongThis->measure(rt, shadowNodeValue);
  };

  auto dispatchCommand = [weakThis = weak_from_this()](
                             jsi::Runtime &rt,
                             const jsi::Value &shadowNodeValue,
                             const jsi::Value &commandNameValue,
                             const jsi::Value &argsValue) {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return;
    }

    strongThis->dispatchCommand(
        rt, shadowNodeValue, commandNameValue, argsValue);
  };
  ProgressLayoutAnimationFunction progressLayoutAnimation =
      [weakThis = weak_from_this()](
          jsi::Runtime &rt, int tag, const jsi::Object &newStyle) {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }

        auto surfaceId =
            strongThis->layoutAnimationsProxy_->progressLayoutAnimation(
                tag, newStyle);
        if (!surfaceId) {
          return;
        }
        strongThis->uiManager_->getShadowTreeRegistry().visit(
            *surfaceId, [](const ShadowTree &shadowTree) {
              shadowTree.notifyDelegatesOfUpdates();
            });
      };

  EndLayoutAnimationFunction endLayoutAnimation =
      [weakThis = weak_from_this()](int tag, bool shouldRemove) {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }

        auto surfaceId = strongThis->layoutAnimationsProxy_->endLayoutAnimation(
            tag, shouldRemove);
        if (!surfaceId) {
          return;
        }

        strongThis->uiManager_->getShadowTreeRegistry().visit(
            *surfaceId, [](const ShadowTree &shadowTree) {
              shadowTree.notifyDelegatesOfUpdates();
            });
      };

  auto obtainProp = [weakThis = weak_from_this()](
                        jsi::Runtime &rt,
                        const jsi::Value &shadowNodeWrapper,
                        const jsi::Value &propName) {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return jsi::String::createFromUtf8(rt, "");
    }

    return strongThis->obtainProp(rt, shadowNodeWrapper, propName);
  };

  jsi::Runtime &uiRuntime =
      workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime();
  UIRuntimeDecorator::decorate(
      uiRuntime,
      obtainProp,
      updateProps,
      measure,
      dispatchCommand,
      platformDepMethodsHolder.getAnimationTimestamp,
      platformDepMethodsHolder.setGestureStateFunction,
      progressLayoutAnimation,
      endLayoutAnimation,
      platformDepMethodsHolder.maybeFlushUIUpdatesQueueFunction);
}

ReanimatedModuleProxy::~ReanimatedModuleProxy() {
  // event handler registry and frame callbacks store some JSI values from UI
  // runtime, so they have to go away before we tear down the runtime
  eventHandlerRegistry_.reset();
  frameCallbacks_.clear();
}

jsi::Value ReanimatedModuleProxy::registerEventHandler(
    jsi::Runtime &rt,
    const jsi::Value &worklet,
    const jsi::Value &eventName,
    const jsi::Value &emitterReactTag) {
  static uint64_t NEXT_EVENT_HANDLER_ID = 1;

  uint64_t newRegistrationId = NEXT_EVENT_HANDLER_ID++;
  auto eventNameStr = eventName.asString(rt).utf8(rt);
  auto handlerShareable = extractShareableOrThrow<ShareableWorklet>(
      rt, worklet, "[Reanimated] Event handler must be a worklet.");
  int emitterReactTagInt = emitterReactTag.asNumber();

  workletsModuleProxy_->getUIScheduler()->scheduleOnUI(
      [=, weakThis = weak_from_this()]() {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }
        auto handler = std::make_shared<WorkletEventHandler>(
            newRegistrationId,
            eventNameStr,
            emitterReactTagInt,
            handlerShareable);
        strongThis->eventHandlerRegistry_->registerEventHandler(
            std::move(handler));
      });

  return jsi::Value(static_cast<double>(newRegistrationId));
}

void ReanimatedModuleProxy::unregisterEventHandler(
    jsi::Runtime &,
    const jsi::Value &registrationId) {
  uint64_t id = registrationId.asNumber();
  workletsModuleProxy_->getUIScheduler()->scheduleOnUI(
      [=, weakThis = weak_from_this()]() {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }
        strongThis->eventHandlerRegistry_->unregisterEventHandler(id);
      });
}

static inline std::string intColorToHex(const int val) {
  std::stringstream
      invertedHexColorStream; // By default transparency is first, color second
  invertedHexColorStream << std::setfill('0') << std::setw(8) << std::hex
                         << val;

  auto invertedHexColor = invertedHexColorStream.str();
  auto hexColor =
      "#" + invertedHexColor.substr(2, 6) + invertedHexColor.substr(0, 2);

  return hexColor;
}

std::string ReanimatedModuleProxy::obtainPropFromShadowNode(
    jsi::Runtime &rt,
    const std::string &propName,
    const ShadowNode::Shared &shadowNode) {
  auto newestCloneOfShadowNode =
      uiManager_->getNewestCloneOfShadowNode(*shadowNode);

  if (propName == "width" || propName == "height" || propName == "top" ||
      propName == "left") {
    // These props are calculated from frame
    auto layoutableShadowNode = dynamic_cast<LayoutableShadowNode const *>(
        newestCloneOfShadowNode.get());
    const auto &frame = layoutableShadowNode->layoutMetrics_.frame;

    if (propName == "width") {
      return std::to_string(frame.size.width);
    } else if (propName == "height") {
      return std::to_string(frame.size.height);
    } else if (propName == "top") {
      return std::to_string(frame.origin.y);
    } else if (propName == "left") {
      return std::to_string(frame.origin.x);
    }
  } else {
    // These props are calculated from viewProps
    auto props = newestCloneOfShadowNode->getProps();
    auto viewProps = std::static_pointer_cast<const ViewProps>(props);
    if (propName == "opacity") {
      return std::to_string(viewProps->opacity);
    } else if (propName == "zIndex") {
      if (viewProps->zIndex.has_value()) {
        return std::to_string(*viewProps->zIndex);
      }
    } else if (propName == "backgroundColor") {
      return intColorToHex(*viewProps->backgroundColor);
    }
  }

  throw std::runtime_error(std::string(
      "Getting property `" + propName +
      "` with function `getViewProp` is not supported"));
}

jsi::Value ReanimatedModuleProxy::getViewProp(
    jsi::Runtime &rnRuntime,
    const jsi::Value &shadowNodeWrapper,
    const jsi::Value &propName,
    const jsi::Value &callback) {
  const auto propNameStr = propName.asString(rnRuntime).utf8(rnRuntime);
  const auto funPtr = std::make_shared<jsi::Function>(
      callback.getObject(rnRuntime).asFunction(rnRuntime));
  const auto shadowNode = shadowNodeFromValue(rnRuntime, shadowNodeWrapper);
  workletsModuleProxy_->getUIScheduler()->scheduleOnUI(
      [=, weakThis = weak_from_this()]() {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }
        jsi::Runtime &uiRuntime =
            strongThis->workletsModuleProxy_->getUIWorkletRuntime()
                ->getJSIRuntime();
        const auto resultStr = strongThis->obtainPropFromShadowNode(
            uiRuntime, propNameStr, shadowNode);

        strongThis->workletsModuleProxy_->getJSScheduler()->scheduleOnJS(
            [=](jsi::Runtime &rnRuntime) {
              const auto resultValue =
                  jsi::String::createFromUtf8(rnRuntime, resultStr);
              funPtr->call(rnRuntime, resultValue);
            });
      });
  return jsi::Value::undefined();
}

jsi::Value ReanimatedModuleProxy::setDynamicFeatureFlag(
    jsi::Runtime &rt,
    const jsi::Value &name,
    const jsi::Value &value) {
  DynamicFeatureFlags::setFlag(name.asString(rt).utf8(rt), value.asBool());
  return jsi::Value::undefined();
}

jsi::Value ReanimatedModuleProxy::configureLayoutAnimationBatch(
    jsi::Runtime &rt,
    const jsi::Value &layoutAnimationsBatch) {
  auto array = layoutAnimationsBatch.asObject(rt).asArray(rt);
  size_t length = array.size(rt);
  std::vector<LayoutAnimationConfig> batch(length);
  for (int i = 0; i < length; i++) {
    auto item = array.getValueAtIndex(rt, i).asObject(rt);
    auto &batchItem = batch[i];
    batchItem.tag = item.getProperty(rt, "viewTag").asNumber();
    batchItem.type = static_cast<LayoutAnimationType>(
        item.getProperty(rt, "type").asNumber());
    auto config = item.getProperty(rt, "config");
    if (config.isUndefined()) {
      batchItem.config = nullptr;
    } else {
      batchItem.config = extractShareableOrThrow<ShareableObject>(
          rt,
          config,
          "[Reanimated] Layout animation config must be an object.");
    }
  }
  layoutAnimationsManager_->configureAnimationBatch(batch);
  return jsi::Value::undefined();
}

void ReanimatedModuleProxy::setShouldAnimateExiting(
    jsi::Runtime &rt,
    const jsi::Value &viewTag,
    const jsi::Value &shouldAnimate) {
  layoutAnimationsManager_->setShouldAnimateExiting(
      viewTag.asNumber(), shouldAnimate.getBool());
}

bool ReanimatedModuleProxy::isAnyHandlerWaitingForEvent(
    const std::string &eventName,
    const int emitterReactTag) {
  return eventHandlerRegistry_->isAnyHandlerWaitingForEvent(
      eventName, emitterReactTag);
}

void ReanimatedModuleProxy::maybeRequestRender() {
  if (!renderRequested_) {
    renderRequested_ = true;
    requestRender_(onRenderCallback_);
  }
}

void ReanimatedModuleProxy::onRender(double timestampMs) {
  ReanimatedSystraceSection s("ReanimatedModuleProxy::onRender");
  auto callbacks = std::move(frameCallbacks_);
  frameCallbacks_.clear();
  jsi::Runtime &uiRuntime =
      workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime();
  jsi::Value timestamp{timestampMs};
  for (const auto &callback : callbacks) {
    runOnRuntimeGuarded(uiRuntime, *callback, timestamp);
  }
}

jsi::Value ReanimatedModuleProxy::registerSensor(
    jsi::Runtime &rt,
    const jsi::Value &sensorType,
    const jsi::Value &interval,
    const jsi::Value &iosReferenceFrame,
    const jsi::Value &sensorDataHandler) {
  return animatedSensorModule_.registerSensor(
      rt,
      workletsModuleProxy_->getUIWorkletRuntime(),
      sensorType,
      interval,
      iosReferenceFrame,
      sensorDataHandler);
}

void ReanimatedModuleProxy::unregisterSensor(
    jsi::Runtime &,
    const jsi::Value &sensorId) {
  animatedSensorModule_.unregisterSensor(sensorId);
}

void ReanimatedModuleProxy::cleanupSensors() {
  animatedSensorModule_.unregisterAllSensors();
}

void ReanimatedModuleProxy::setViewStyle(
    jsi::Runtime &rt,
    const jsi::Value &viewTag,
    const jsi::Value &viewStyle) {
  const auto tag = viewTag.asNumber();
  staticPropsRegistry_->set(rt, tag, viewStyle);
  if (staticPropsRegistry_->hasObservers(tag)) {
    maybeRunCSSLoop();
  }
}

void ReanimatedModuleProxy::markNodeAsRemovable(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeWrapper) {
  auto shadowNode = shadowNodeFromValue(rt, shadowNodeWrapper);
  updatesRegistryManager_->markNodeAsRemovable(shadowNode);
}

void ReanimatedModuleProxy::unmarkNodeAsRemovable(
    jsi::Runtime &rt,
    const jsi::Value &viewTag) {
  updatesRegistryManager_->unmarkNodeAsRemovable(viewTag.asNumber());
}

void ReanimatedModuleProxy::registerCSSKeyframes(
    jsi::Runtime &rt,
    const jsi::Value &animationName,
    const jsi::Value &keyframesConfig) {
  cssAnimationKeyframesRegistry_->add(
      animationName.asString(rt).utf8(rt),
      parseCSSAnimationKeyframesConfig(
          rt, keyframesConfig, viewStylesRepository_));
}

void ReanimatedModuleProxy::unregisterCSSKeyframes(
    jsi::Runtime &rt,
    const jsi::Value &animationName) {
  cssAnimationKeyframesRegistry_->remove(animationName.asString(rt).utf8(rt));
}

void ReanimatedModuleProxy::applyCSSAnimations(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeWrapper,
    const jsi::Value &animationUpdates) {
  auto shadowNode = shadowNodeFromValue(rt, shadowNodeWrapper);
  const auto timestamp = getCssTimestamp();
  const auto updates = parseCSSAnimationUpdates(rt, animationUpdates);

  CSSAnimationsMap newAnimations;

  if (!updates.newAnimationSettings.empty()) {
    // animationNames always exists when newAnimationSettings is not empty
    const auto animationNames = updates.animationNames.value();
    const auto animationNamesCount = animationNames.size();

    for (const auto &[index, settings] : updates.newAnimationSettings) {
      if (index >= animationNamesCount) {
        throw std::invalid_argument(
            "[Reanimated] index is out of bounds of animationNames");
      }

      const auto &name = animationNames[index];
      const auto animation = std::make_shared<CSSAnimation>(
          rt,
          shadowNode,
          name,
          cssAnimationKeyframesRegistry_->get(name),
          settings,
          timestamp);

      newAnimations.emplace(index, animation);
    }
  }

  {
    auto lock = cssAnimationsRegistry_->lock();
    cssAnimationsRegistry_->apply(
        rt,
        shadowNode,
        updates.animationNames,
        std::move(newAnimations),
        updates.settingsUpdates,
        timestamp);
  }

  maybeRunCSSLoop();
}

void ReanimatedModuleProxy::unregisterCSSAnimations(const jsi::Value &viewTag) {
  auto lock = cssAnimationsRegistry_->lock();
  cssAnimationsRegistry_->remove(viewTag.asNumber());
}

void ReanimatedModuleProxy::registerCSSTransition(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeWrapper,
    const jsi::Value &transitionConfig) {
  auto shadowNode = shadowNodeFromValue(rt, shadowNodeWrapper);

  auto transition = std::make_shared<CSSTransition>(
      std::move(shadowNode),
      parseCSSTransitionConfig(rt, transitionConfig),
      viewStylesRepository_);

  {
    auto lock = cssTransitionsRegistry_->lock();
    cssTransitionsRegistry_->add(transition);
  }
  maybeRunCSSLoop();
}

void ReanimatedModuleProxy::updateCSSTransition(
    jsi::Runtime &rt,
    const jsi::Value &viewTag,
    const jsi::Value &configUpdates) {
  auto lock = cssTransitionsRegistry_->lock();
  cssTransitionsRegistry_->updateSettings(
      viewTag.asNumber(), parsePartialCSSTransitionConfig(rt, configUpdates));
  maybeRunCSSLoop();
}

void ReanimatedModuleProxy::unregisterCSSTransition(
    jsi::Runtime &rt,
    const jsi::Value &viewTag) {
  auto lock = cssTransitionsRegistry_->lock();
  cssTransitionsRegistry_->remove(viewTag.asNumber());
}

bool ReanimatedModuleProxy::handleEvent(
    const std::string &eventName,
    const int emitterReactTag,
    const jsi::Value &payload,
    double currentTime) {
  ReanimatedSystraceSection s("ReanimatedModuleProxy::handleEvent");

  eventHandlerRegistry_->processEvent(
      workletsModuleProxy_->getUIWorkletRuntime(),
      currentTime,
      eventName,
      emitterReactTag,
      payload);

  // TODO: return true if Reanimated successfully handled the event
  // to avoid sending it to JavaScript
  return false;
}

bool ReanimatedModuleProxy::handleRawEvent(
    const RawEvent &rawEvent,
    double currentTime) {
  ReanimatedSystraceSection s("ReanimatedModuleProxy::handleRawEvent");

  const EventTarget *eventTarget = rawEvent.eventTarget.get();
  if (eventTarget == nullptr) {
    // after app reload scrollView is unmounted and its content offset is set
    // to 0 and view is thrown into recycle pool setting content offset
    // triggers scroll event eventTarget is null though, because it's
    // unmounting we can just ignore this event, because it's an event on
    // unmounted component
    return false;
  }

  int tag = eventTarget->getTag();
  auto eventType = rawEvent.type;
  if (eventType.rfind("top", 0) == 0) {
    eventType = "on" + eventType.substr(3);
  }

  if (!isAnyHandlerWaitingForEvent(eventType, tag)) {
    return false;
  }

  jsi::Runtime &rt =
      workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime();
  const auto &eventPayload = rawEvent.eventPayload;
  jsi::Value payload = eventPayload->asJSIValue(rt);

  auto res = handleEvent(eventType, tag, std::move(payload), currentTime);
  // TODO: we should call performOperations conditionally if event is handled
  // (res == true), but for now handleEvent always returns false. Thankfully,
  // performOperations does not trigger a lot of code if there is nothing to
  // be done so this is fine for now.
  performOperations();
  return res;
}

void ReanimatedModuleProxy::cssLoopCallback(const double /*timestampMs*/) {
  shouldUpdateCssAnimations_ = true;
  if (cssAnimationsRegistry_->hasUpdates() ||
      cssTransitionsRegistry_->hasUpdates()
#ifdef ANDROID
      || updatesRegistryManager_->hasPropsToRevert()
#endif // ANDROID
  ) {
    requestRender_([weakThis = weak_from_this()](const double newTimestampMs) {
      if (auto strongThis = weakThis.lock()) {
        strongThis->cssLoopCallback(newTimestampMs);
      }
    });
  } else {
    cssLoopRunning_ = false;
  }
}

void ReanimatedModuleProxy::maybeRunCSSLoop() {
  if (cssLoopRunning_) {
    return;
  }

  cssLoopRunning_ = true;

  workletsModuleProxy_->getUIScheduler()->scheduleOnUI(
      [weakThis = weak_from_this()]() {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }
        strongThis->requestRender_([weakThis](const double timestampMs) {
          if (auto strongThis = weakThis.lock()) {
            strongThis->cssLoopCallback(timestampMs);
          }
        });
      });
}

double ReanimatedModuleProxy::getCssTimestamp() {
  if (cssLoopRunning_) {
    return currentCssTimestamp_;
  }
  currentCssTimestamp_ = getAnimationTimestamp_();
  return currentCssTimestamp_;
}

void ReanimatedModuleProxy::performOperations() {
  ReanimatedSystraceSection s("ReanimatedModuleProxy::performOperations");

  jsi::Runtime &rt =
      workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime();

  UpdatesBatch updatesBatch;
  {
    ReanimatedSystraceSection s2("ReanimatedModuleProxy::flushUpdates");

    auto lock = updatesRegistryManager_->lock();

    if (shouldUpdateCssAnimations_) {
      currentCssTimestamp_ = getAnimationTimestamp_();
      auto lock = cssTransitionsRegistry_->lock();
      // Update CSS transitions and flush updates
      cssTransitionsRegistry_->update(currentCssTimestamp_);
      cssTransitionsRegistry_->flushUpdates(updatesBatch);
    }

    {
      auto lock = animatedPropsRegistry_->lock();
      // Flush all animated props updates
      animatedPropsRegistry_->flushUpdates(updatesBatch);
    }

    if (shouldUpdateCssAnimations_) {
      auto lock = cssAnimationsRegistry_->lock();
      // Update CSS animations and flush updates
      cssAnimationsRegistry_->update(currentCssTimestamp_);
      cssAnimationsRegistry_->flushUpdates(updatesBatch);
    }

    shouldUpdateCssAnimations_ = false;

    if ((updatesBatch.size() > 0) &&
        updatesRegistryManager_->shouldReanimatedSkipCommit()) {
      updatesRegistryManager_->pleaseCommitAfterPause();
    }
  }

  if (updatesRegistryManager_->shouldReanimatedSkipCommit()) {
    // It may happen that `performOperations` is called on the UI thread
    // while React Native tries to commit a new tree on the JS thread.
    // In this case, we should skip the commit here and let React Native do
    // it. The commit will include the current values from the updates manager
    // which will be applied in ReanimatedCommitHook.
    return;
  }

  commitUpdates(rt, updatesBatch);

  // Clear the entire cache after the commit
  // (we don't know if the view is updated from outside of Reanimated
  // so we have to clear the entire cache)
  viewStylesRepository_->clearNodesCache();
}

void ReanimatedModuleProxy::requestFlushRegistry() {
  requestRender_([weakThis = weak_from_this()](const double timestamp) {
    if (auto strongThis = weakThis.lock()) {
      strongThis->shouldFlushRegistry_ = true;
    }
  });
}

void ReanimatedModuleProxy::commitUpdates(
    jsi::Runtime &rt,
    const UpdatesBatch &updatesBatch) {
  ReanimatedSystraceSection s("ReanimatedModuleProxy::commitUpdates");
  react_native_assert(uiManager_ != nullptr);
  const auto &shadowTreeRegistry = uiManager_->getShadowTreeRegistry();

  std::unordered_map<SurfaceId, PropsMap> propsMapBySurface;

#ifdef ANDROID
  updatesRegistryManager_->collectPropsToRevertBySurface(propsMapBySurface);
#endif

  if (shouldFlushRegistry_) {
    shouldFlushRegistry_ = false;
    const auto propsMap = updatesRegistryManager_->collectProps();
    for (auto const &[family, props] : propsMap) {
      const auto surfaceId = family->getSurfaceId();
      auto &propsVector = propsMapBySurface[surfaceId][family];
      for (const auto &prop : props) {
        propsVector.emplace_back(prop);
      }
    }
  } else {
    for (auto const &[shadowNode, props] : updatesBatch) {
      SurfaceId surfaceId = shadowNode->getSurfaceId();
      auto family = &shadowNode->getFamily();
      react_native_assert(family->getSurfaceId() == surfaceId);
      propsMapBySurface[surfaceId][family].emplace_back(std::move(props));
    }
  }

  for (auto const &[surfaceId, propsMap] : propsMapBySurface) {
    shadowTreeRegistry.visit(surfaceId, [&](ShadowTree const &shadowTree) {
      const auto status = shadowTree.commit(
          [&](RootShadowNode const &oldRootShadowNode)
              -> RootShadowNode::Unshared {
            if (updatesRegistryManager_->shouldReanimatedSkipCommit()) {
              return nullptr;
            }

            auto rootNode =
                cloneShadowTreeWithNewProps(oldRootShadowNode, propsMap);

            // Mark the commit as Reanimated commit so that we can distinguish
            // it in ReanimatedCommitHook.

            auto reaShadowNode =
                std::reinterpret_pointer_cast<ReanimatedCommitShadowNode>(
                    rootNode);
            reaShadowNode->setReanimatedCommitTrait();

            return rootNode;
          },
          {/* .enableStateReconciliation = */
           false,
           /* .mountSynchronously = */ true});

#ifdef ANDROID
      if (status == ShadowTree::CommitStatus::Succeeded) {
        updatesRegistryManager_->clearPropsToRevert(surfaceId);
      }
#endif
    });
  }
}

void ReanimatedModuleProxy::dispatchCommand(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue,
    const jsi::Value &commandNameValue,
    const jsi::Value &argsValue) {
  ShadowNode::Shared shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  std::string commandName = stringFromValue(rt, commandNameValue);
  folly::dynamic args = commandArgsFromValue(rt, argsValue);
  const auto &scheduler = static_cast<Scheduler *>(uiManager_->getDelegate());

  if (!scheduler) {
    return;
  }

  const auto &schedulerDelegate = scheduler->getDelegate();

  if (schedulerDelegate) {
    const auto shadowView = ShadowView(*shadowNode);
    schedulerDelegate->schedulerDidDispatchCommand(
        shadowView, commandName, args);
  }
}

jsi::String ReanimatedModuleProxy::obtainProp(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeWrapper,
    const jsi::Value &propName) {
  jsi::Runtime &uiRuntime =
      workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime();
  const auto propNameStr = propName.asString(rt).utf8(rt);
  const auto shadowNode = shadowNodeFromValue(rt, shadowNodeWrapper);
  const auto resultStr =
      obtainPropFromShadowNode(uiRuntime, propNameStr, shadowNode);
  return jsi::String::createFromUtf8(rt, resultStr);
}

jsi::Value ReanimatedModuleProxy::measure(
    jsi::Runtime &rt,
    const jsi::Value &shadowNodeValue) {
  // based on implementation from UIManagerBinding.cpp

  auto shadowNode = shadowNodeFromValue(rt, shadowNodeValue);
  auto layoutMetrics = uiManager_->getRelativeLayoutMetrics(
      *shadowNode, nullptr, {/* .includeTransform = */ true});

  if (layoutMetrics == EmptyLayoutMetrics) {
    // Originally, in this case React Native returns `{0, 0, 0, 0, 0, 0}`,
    // most likely due to the type of measure callback function which accepts
    // just an array of numbers (not null). In Reanimated, `measure` returns
    // `MeasuredDimensions | null`.
    return jsi::Value::null();
  }
  auto newestCloneOfShadowNode =
      uiManager_->getNewestCloneOfShadowNode(*shadowNode);

  auto layoutableShadowNode =
      dynamic_cast<LayoutableShadowNode const *>(newestCloneOfShadowNode.get());
  facebook::react::Point originRelativeToParent =
      layoutableShadowNode != nullptr
      ? layoutableShadowNode->getLayoutMetrics().frame.origin
      : facebook::react::Point();

  auto frame = layoutMetrics.frame;

  jsi::Object result(rt);
  result.setProperty(
      rt, "x", jsi::Value(static_cast<double>(originRelativeToParent.x)));
  result.setProperty(
      rt, "y", jsi::Value(static_cast<double>(originRelativeToParent.y)));
  result.setProperty(
      rt, "width", jsi::Value(static_cast<double>(frame.size.width)));
  result.setProperty(
      rt, "height", jsi::Value(static_cast<double>(frame.size.height)));
  result.setProperty(
      rt, "pageX", jsi::Value(static_cast<double>(frame.origin.x)));
  result.setProperty(
      rt, "pageY", jsi::Value(static_cast<double>(frame.origin.y)));
  return result;
}

void ReanimatedModuleProxy::initializeFabric(
    const std::shared_ptr<UIManager> &uiManager) {
  uiManager_ = uiManager;
  viewStylesRepository_->setUIManager(uiManager_);

  initializeLayoutAnimationsProxy();

  const std::function<void()> request = [weakThis = weak_from_this()]() {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return;
    }

    strongThis->requestFlushRegistry();
  };
  mountHook_ = std::make_shared<ReanimatedMountHook>(
      uiManager_, updatesRegistryManager_, request);
  commitHook_ = std::make_shared<ReanimatedCommitHook>(
      uiManager_, updatesRegistryManager_, layoutAnimationsProxy_);
}

void ReanimatedModuleProxy::initializeLayoutAnimationsProxy() {
  uiManager_->setAnimationDelegate(nullptr);
  auto scheduler = reinterpret_cast<Scheduler *>(uiManager_->getDelegate());
  auto componentDescriptorRegistry =
      scheduler->getContextContainer()
          ->at<std::weak_ptr<const ComponentDescriptorRegistry>>(
              "ComponentDescriptorRegistry_DO_NOT_USE_PRETTY_PLEASE")
          .lock();

  if (componentDescriptorRegistry) {
    layoutAnimationsProxy_ = std::make_shared<LayoutAnimationsProxy>(
        layoutAnimationsManager_,
        componentDescriptorRegistry,
        scheduler->getContextContainer(),
        workletsModuleProxy_->getUIWorkletRuntime()->getJSIRuntime(),
        workletsModuleProxy_->getUIScheduler());
  }
}

#ifdef IS_REANIMATED_EXAMPLE_APP

std::string format(bool b) {
  return b ? "✅" : "❌";
}

std::function<std::string()>
ReanimatedModuleProxy::createRegistriesLeakCheck() {
  return [weakThis = weak_from_this()]() {
    auto strongThis = weakThis.lock();
    if (!strongThis) {
      return std::string("");
    }

    std::string result = "";

    result += "AnimatedPropsRegistry: " +
        format(strongThis->animatedPropsRegistry_->isEmpty());
    result += "\nCSSAnimationsRegistry: " +
        format(strongThis->cssAnimationsRegistry_->isEmpty());
    result += "\nCSSTransitionsRegistry: " +
        format(strongThis->cssTransitionsRegistry_->isEmpty());
    result += "\nStaticPropsRegistry: " +
        format(strongThis->staticPropsRegistry_->isEmpty()) + "\n";

    return result;
  };
}

#endif // IS_REANIMATED_EXAMPLE_APP

jsi::Value ReanimatedModuleProxy::subscribeForKeyboardEvents(
    jsi::Runtime &rt,
    const jsi::Value &handlerWorklet,
    const jsi::Value &isStatusBarTranslucent,
    const jsi::Value &isNavigationBarTranslucent) {
  auto shareableHandler = extractShareableOrThrow<ShareableWorklet>(
      rt,
      handlerWorklet,
      "[Reanimated] Keyboard event handler must be a worklet.");
  return subscribeForKeyboardEventsFunction_(
      [=, weakThis = weak_from_this()](int keyboardState, int height) {
        auto strongThis = weakThis.lock();
        if (!strongThis) {
          return;
        }
        strongThis->workletsModuleProxy_->getUIWorkletRuntime()->runGuarded(
            shareableHandler, jsi::Value(keyboardState), jsi::Value(height));
      },
      isStatusBarTranslucent.getBool(),
      isNavigationBarTranslucent.getBool());
}

void ReanimatedModuleProxy::unsubscribeFromKeyboardEvents(
    jsi::Runtime &,
    const jsi::Value &listenerId) {
  unsubscribeFromKeyboardEventsFunction_(listenerId.asNumber());
}

} // namespace reanimated
