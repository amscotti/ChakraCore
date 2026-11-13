// Minimal `bindings` shim for the vendored node-addon-examples drivers.
//
// The examples' JS does `require('bindings')('addon')` (npm dependency,
// never vendored: no network/npm at run time). The matrix runner builds
// each addon with `zig cc`/`zig c++` into `<testdir>/build/Release/
// <target>.node` and sets `NODE_PATH=<shims-dir>` (plus
// `NODEAPI_BUILD_DIR=<testdir>/build/Release`) so this shim can resolve
// the binary without the `bindings` package.
//
// Resolution order for `bindings(name)` (matching the real package's
// build/Release layout for our builder):
//   1. $NODEAPI_BUILD_DIR/<clean>.node   (runner-injected, exact)
//   2. <directory of the requiring file>/build/Release/<clean>.node
//   3. <directory of the requiring file>/build/Debug/<clean>.node
//   4. <cwd>/build/Release/<clean>.node
// where <clean> is `name` with a trailing `.node` stripped. The caller
// directory is recovered from the Error stack (Node-direct runs); on the
// Chakra host the first option is the intended path (the host reads
// NODEAPI_BUILD_DIR or injects its own mapping).
//
// Under real Node the builtin `assert` still wins; `bindings` has no
// builtin, so this file is what Node loads via NODE_PATH.
//
// OUR shim (not vendored upstream).

'use strict';

// Intentionally dependency-free (no require('path')) so the shim also
// loads on hosts without Node builtins.
function joinPath(a, b) {
  if (a.endsWith('/')) return a + b;
  return a + '/' + b;
}

function cleanName(name) {
  return name.endsWith('.node') ? name.slice(0, -'.node'.length) : name;
}

function callerDir() {
  try {
    const stack = new Error().stack || '';
    const lines = String(stack).split('\n');
    // Skip the `Error` header line and this shim's own frames; the first
    // frame referencing a `.js` file outside this shim is the caller.
    for (const line of lines) {
      const match = line.match(/\(([^()]+:\d+:\d+)\)/);
      if (!match) continue;
      const file = match[1].replace(/:\d+:\d+$/, '');
      if (file.endsWith('bindings.js')) continue;
      const slash = file.lastIndexOf('/');
      if (slash > 0) return file.slice(0, slash);
    }
  } catch {
    // Fall through to env/cwd candidates.
  }
  return null;
}

function envVar(name) {
  try {
    if (typeof process !== 'undefined' && process.env &&
        typeof process.env[name] === 'string') {
      return process.env[name];
    }
  } catch {
    // Host without process: no env available.
  }
  return null;
}

function cwdDir() {
  try {
    if (typeof process !== 'undefined' && typeof process.cwd === 'function') {
      return process.cwd();
    }
  } catch {
    // Ignore.
  }
  return null;
}

function bindings(name) {
  const base = cleanName(String(name));
  const candidates = [];
  const buildDir = envVar('NODEAPI_BUILD_DIR');
  if (buildDir) candidates.push(joinPath(buildDir, base + '.node'));
  const dir = callerDir();
  if (dir) {
    candidates.push(joinPath(dir, 'build/Release/' + base + '.node'));
    candidates.push(joinPath(dir, 'build/Debug/' + base + '.node'));
  }
  const cwd = cwdDir();
  if (cwd && (!dir || cwd !== dir)) {
    candidates.push(joinPath(cwd, 'build/Release/' + base + '.node'));
  }
  let lastError = null;
  for (const candidate of candidates) {
    try {
      // eslint-disable-next-line no-undef
      return require(candidate);
    } catch (e) {
      lastError = e;
    }
  }
  throw new Error(
    `bindings: cannot locate addon "${name}" (tried: ${candidates.join(', ')})` +
    (lastError ? `; last error: ${lastError.message}` : ''));
}

module.exports = bindings;
