// `common/gc` shim: `gcUntil` for the vendored js-native-api runners.
//
// Upstream `test/common/gc.js` drives real garbage collection through
// `global.gc()` (requiring `--expose-gc`) plus `async_hooks`/`timers`
// builtins. The test host (tools/nodeapi/ch-nodeapi.cpp) IS the gc hook:
// N5.4 added test-host-only `globalThis.gc` (JsCollectGarbage) -- it is
// never part of the provider. Vendored GC tests remain `needs-gc` skips
// in run_matrix.py pending N5.1 adoption (they also need async_hooks/
// timers/setImmediate semantics the host does not provide); the hook
// exists for N5.4 stress drivers and future adoption.
//
// This shim works with the hook: it calls `global.gc()`, then polls the
// condition with `setImmediate` (falling back to a promise microtask)
// up to `maxCount` attempts. `gcOptions` (Node gc flags) are accepted
// but ignored (no JsRT mapping). If the hook is ever removed, the
// `typeof` check throws needs-gc as the backstop.

'use strict';

async function gcUntil(name, condition, maxCount = 10, gcOptions) {
  if (typeof globalThis.gc !== 'function') {
    throw new Error(
      'needs-gc: global.gc() is unavailable (test host has no gc hook; ' +
      'recorded as a needs-gc skip by run_matrix.py)');
  }
  for (let count = 0; count < maxCount; ++count) {
    if (typeof setImmediate === 'function') {
      await new Promise((resolve) => setImmediate(resolve));
    } else {
      await Promise.resolve();
    }
    if (gcOptions !== undefined) {
      await globalThis.gc(gcOptions);
    } else {
      await globalThis.gc();
    }
    if (condition()) return;
  }
  throw new Error(
    `gcUntil: condition not satisfied after ${maxCount} GC attempts` +
    (name ? ` (${name})` : ''));
}

module.exports = { gcUntil };
