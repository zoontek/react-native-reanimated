'use strict';

import { IS_JEST, logger, SHOULD_BE_USE_WEB } from '../common';

type SetGestureState = (handlerTag: number, newState: number) => void;

export let setGestureState: SetGestureState;

function setGestureStateNative(handlerTag: number, newState: number) {
  'worklet';
  if (!globalThis._WORKLET) {
    logger.warn('You can not use setGestureState in non-worklet function.');
    return;
  }
  global._setGestureState(handlerTag, newState);
}

function setGestureStateJest() {
  logger.warn('setGestureState() cannot be used with Jest.');
}

function setGestureStateDefault() {
  logger.warn('setGestureState() is not supported on this configuration.');
}

if (!SHOULD_BE_USE_WEB) {
  setGestureState = setGestureStateNative;
} else if (IS_JEST) {
  setGestureState = setGestureStateJest;
} else {
  setGestureState = setGestureStateDefault;
}
