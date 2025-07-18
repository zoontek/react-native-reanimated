/* eslint-disable camelcase */
'use strict';

import { ReanimatedError } from '../common';
import type { IAnimatedComponentInternal } from '../createAnimatedComponent/commonTypes';

export type HostInstance = {
  __internalInstanceHandle?: Record<string, unknown>;
  __nativeTag?: number;
  _viewConfig?: Record<string, unknown>;
};

function findHostInstanceFastPath(maybeNativeRef: HostInstance | undefined) {
  if (!maybeNativeRef) {
    return undefined;
  }
  if (
    maybeNativeRef.__internalInstanceHandle &&
    maybeNativeRef.__nativeTag &&
    maybeNativeRef._viewConfig
  ) {
    return maybeNativeRef;
  }
  // That means it’s a ref to a non-native component, and it’s necessary
  // to call `findHostInstance_DEPRECATED` on them.
  return undefined;
}

function resolveFindHostInstance_DEPRECATED() {
  if (findHostInstance_DEPRECATED !== undefined) {
    return;
  }
  try {
    // eslint-disable-next-line @typescript-eslint/no-require-imports, @typescript-eslint/no-var-requires
    const ReactFabric = require('react-native/Libraries/Renderer/shims/ReactFabric');
    // Since RN 0.77 ReactFabric exports findHostInstance_DEPRECATED in default object so we're trying to
    // access it first, then fallback on named export
    findHostInstance_DEPRECATED =
      ReactFabric?.default?.findHostInstance_DEPRECATED ??
      ReactFabric?.findHostInstance_DEPRECATED;
  } catch (_e) {
    throw new ReanimatedError('Failed to resolve findHostInstance_DEPRECATED');
  }
}

let findHostInstance_DEPRECATED: (ref: unknown) => HostInstance;
export function findHostInstance(
  component: IAnimatedComponentInternal | React.Component
): HostInstance {
  // Fast path for native refs
  const hostInstance = findHostInstanceFastPath(
    (component as IAnimatedComponentInternal)._componentRef as HostInstance
  );
  if (hostInstance !== undefined) {
    return hostInstance;
  }

  resolveFindHostInstance_DEPRECATED();
  /*
    The Fabric implementation of `findHostInstance_DEPRECATED` requires a React ref as an argument
    rather than a native ref. If a component implements the `getAnimatableRef` method, it must use 
    the ref provided by this method. It is the component's responsibility to ensure that this is 
    a valid React ref.
  */
  return findHostInstance_DEPRECATED(
    (component as IAnimatedComponentInternal)._hasAnimatedRef
      ? (component as IAnimatedComponentInternal)._componentRef
      : component
  );
}
