// Minimal `test/common` shim for the vendored js-native-api runners.
//
// Upstream Node's `test/common/index.js` (at the vendored pin) pulls in
// `fs`, `net`, `buffer`, `child_process`, `worker_threads`, `process`, ...
// and is deliberately NOT vendored (plan N0.5: nothing requiring Node
// builtins enters the runnable subset; such tests are recorded as
// `needs-runtime-global`). This shim covers exactly the API surface used by
// the vendored `test/js-native-api` runners at Node v22.22.3:
//
//   - `buildType`  ('Release': the runner builds into `build/Release/`)
//   - `mustCall`, `mustCallAtLeast`, `mustNotCall`
//   - `nodeProcessAborted` (used only by the excluded spawn tests)
//
// `mustCall` semantics mirror Node's `test/common`: the wrapped function
// counts calls and, when a Node-like `process` with an `exit` event is
// available, a mismatch fails the process at exit. On the Chakra host there
// is no `process` yet, so exit-time verification is best-effort: call counts
// are still tracked (see `mustCall` records below) and the wrapped functions
// behave identically; only the missing-call failure cannot fire. Tests
// driven under the host therefore rely on the native assertions for
// correctness, not on mustCall exit checks.
//
// This file is OUR shim (not vendored upstream). Under Node it is only used
// as `<work>/common.js` for the copied test tree; Node's own `assert`
// builtin still wins for `require('assert')`.

'use strict';

// Build directory name the runner builds addons into. Node's real common
// derives this from `process.config.v8_enable...`/build type flags; the
// matrix runner always uses the Release layout.
const buildType = 'Release';

// --- mustCall machinery ----------------------------------------------------

// Records: { count, exact (number|null), minimum (number|null), label }
const mustCallRecords = [];

// Called via `process.on('exit')` when a Node-like process is present.
function checkMustCallAtExit() {
  let failed = false;
  for (const rec of mustCallRecords) {
    if (rec.exact != null && rec.count !== rec.exact) {
      console.error(
        `mustCall check failed: expected exactly ${rec.exact} call(s), got ${rec.count} (${rec.label})`);
      failed = true;
    } else if (rec.minimum != null && rec.count < rec.minimum) {
      console.error(
        `mustCall check failed: expected at least ${rec.minimum} call(s), got ${rec.count} (${rec.label})`);
      failed = true;
    }
  }
  if (failed) {
    // Match Node's behavior of failing the process on unfulfilled calls.
    if (typeof process !== 'undefined' && typeof process.exit === 'function') {
      process.exitCode = 1;
    } else {
      throw new Error('mustCall check failed at exit');
    }
  }
}

// Hook the exit check once, if a Node-like process with events exists.
if (typeof process !== 'undefined' && typeof process.on === 'function') {
  process.on('exit', checkMustCallAtExit);
}

// Node's `mustCall(fn, exact)` also accepts `mustCall()` and `mustCall(n)`:
// a numeric first argument means `exact`, an omitted fn means a no-op.
function mustCall(fn, exact) {
  if (typeof fn === 'number') {
    exact = fn;
    fn = null;
  }
  if (typeof exact !== 'number') exact = 1;
  const label = fn && fn.name ? fn.name : 'anonymous';
  const rec = { count: 0, exact, minimum: null, label };
  const wrapped = function mustCallWrapper() {
    rec.count++;
    if (typeof fn === 'function') return fn.apply(this, arguments);
    return undefined;
  };
  wrapped._mustCallRecord = rec;
  mustCallRecords.push(rec);
  return wrapped;
}
mustCall._records = mustCallRecords;

// Ensures the wrapped function is called at least `minimum` times.
function mustCallAtLeast(fn, minimum) {
  if (typeof fn === 'number' && typeof minimum !== 'number') {
    minimum = fn;
    fn = null;
  }
  if (typeof minimum !== 'number') minimum = 1;
  const label = fn && fn.name ? fn.name : 'anonymous';
  const rec = { count: 0, exact: null, minimum, label };
  const wrapped = function mustCallAtLeastWrapper() {
    rec.count++;
    if (typeof fn === 'function') return fn.apply(this, arguments);
    return undefined;
  };
  wrapped._mustCallRecord = rec;
  mustCallRecords.push(rec);
  return wrapped;
}
mustCallAtLeast._records = mustCallRecords;

// Returns a function that throws if it is ever called (Node semantics).
function mustNotCall(msg) {
  return function mustNotCallWrapper() {
    throw new Error(msg || 'mustNotCall function should not be called');
  };
}

// Best-effort mirror of Node's `common.nodeProcessAborted(exitCode, signal)`:
// a process that was killed by a signal (or exited without a code) counts as
// aborted. Only used by excluded `spawnSync` tests; provided so the shim is
// a complete `common` for the vendored directory.
function nodeProcessAborted(exitCode, signal) {
  return signal != null && signal !== 0 || exitCode == null;
}

module.exports = {
  buildType,
  mustCall,
  mustCallAtLeast,
  mustNotCall,
  nodeProcessAborted,
  // Exposed for hosts that want to verify mustCall records themselves.
  _mustCallRecords: mustCallRecords,
  _checkMustCall: checkMustCallAtExit,
};
