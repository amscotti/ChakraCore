// Minimal `common` shim for node-addon-api test drivers under the Chakra
// host (plan card N5.3). OUR shim (not vendored upstream); the runner
// materializes it as `<test>/common.js` in the copied work tree, where it
// shadows the vendored upstream `test/common/index.js` for both hosts
// (a sibling `common.js` file wins over the `common/` directory in both
// Node's resolution and the host's require).
//
// Upstream `test/common/index.js` needs Node builtins (fs/child_process/
// readline) and loads four binding variants async — unusable under
// ch-nodeapi. This shim covers exactly the surface the runnable N5.3
// drivers use, verified by grep at the pin (v8.9.2):
//
//   - `runTest(test)` — calls `test(binding)` once. `binding` is
//     `globalThis.__exports` on the host (the host binds the addon
//     exports there) or `require('./build/Release/binding.node')` under
//     real Node (where the runner places the built addon — same trick
//     as upstream `runTest`, minus the four build variants: noexcept /
//     noexcept-maybe / custom-namespace builds are out of the N5.3
//     subset — provider-gap, recorded in the receipt).
//   - `mustCall` / `mustNotCall` — passthrough (no call-count checking;
//     no driver in the runnable subset uses them).
//   - `mustCallChecks()` — no-op.
//   - `gcUntil()` — throws a `needs-gc` error (must never fire for a
//     correctly classified runnable driver; failing loudly beats a
//     silent skip).
//
// `runTest` is synchronous. Drivers whose test body is `async` are
// pre-classified `provider-gap` (never run): the Chakra promise pump
// swallows unhandled rejections (verified with N5.3 rej1-3 probes), so
// an async failure could never be attributed to an exit code.

'use strict';

function runTest(test) {
  let binding;
  if (typeof globalThis !== 'undefined' && globalThis.__exports) {
    binding = globalThis.__exports;
  } else {
    binding = require('./build/Release/binding.node');
  }
  return test(binding);
}

function runTestWithBindingPath(test) {
  return test('./build/Release/binding.node');
}

function mustCall(fn) {
  return fn;
}

function mustNotCall(name) {
  return function () {
    throw new Error('mustNotCall violated: ' + name);
  };
}

function mustCallChecks() {}

function gcUntil() {
  throw new Error('needs-gc: gcUntil is not provided by this shim');
}

module.exports = { runTest, runTestWithBindingPath, mustCall, mustNotCall, mustCallChecks, gcUntil };
