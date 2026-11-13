// Minimal `assert` shim for vendored-test drivers on hosts without Node
// builtins (the Chakra host exposes no require()-able `assert` module).
//
// Under real Node, `require('assert')` resolves to the builtin and this shim
// is never loaded; the matrix runner sets NODE_PATH to the shims dir so
// builtins still win there. On the host, the host's require() resolves
// 'assert' to this file. Implemented subset (exactly the methods the
// vendored `test/js-native-api` runners use at the pinned Node release):
//
//   assert(value, message), assert.ok, assert.strictEqual,
//   assert.notStrictEqual, assert.deepStrictEqual, assert.throws,
//   assert.match
//
// plus `assert.fail`. Error semantics mirror Node: failures throw an
// `AssertionError` (name 'AssertionError', code 'ERR_ASSERTION'), and
// `assert.throws(fn, matcher)` accepts an Error class, a RegExp (matched
// against the error message), or a plain object whose properties the error
// must match (name/message).

'use strict';

class AssertionError extends Error {
  constructor(options) {
    const message = typeof options === 'string'
      ? options
      : (options && options.message) || 'Assertion failed';
    super(message);
    this.name = 'AssertionError';
    this.code = 'ERR_ASSERTION';
    if (options && typeof options === 'object') {
      this.actual = options.actual;
      this.expected = options.expected;
      this.operator = options.operator;
    }
  }
}

function fmt(value) {
  if (value === null) return 'null';
  if (value === undefined) return 'undefined';
  if (typeof value === 'string') return JSON.stringify(value);
  try {
    const s = JSON.stringify(value);
    if (s !== undefined) return s;
  } catch { /* fall through */ }
  try {
    return String(value);
  } catch {
    return Object.prototype.toString.call(value);
  }
}

function assert(value, message) {
  if (!value) {
    throw new AssertionError(
      message || `Expected truthy, got ${fmt(value)}`);
  }
}

assert.ok = function ok(value, message) {
  assert(value, message || `Expected truthy, got ${fmt(value)}`);
};

assert.strictEqual = function strictEqual(actual, expected, message) {
  if (!Object.is(actual, expected)) {
    throw new AssertionError({
      actual,
      expected,
      operator: 'strictEqual',
      message: message ||
        `Expected strict equality: ${fmt(actual)} !== ${fmt(expected)}`,
    });
  }
};

assert.notStrictEqual = function notStrictEqual(actual, expected, message) {
  if (Object.is(actual, expected)) {
    throw new AssertionError({
      actual,
      expected,
      operator: 'notStrictEqual',
      message: message ||
        `Expected non-strict-inequality: ${fmt(actual)} === ${fmt(expected)}`,
    });
  }
};

// Structural equality for the plain data the runners compare (primitives,
// arrays, objects, Errors). Mirrors deepStrictEqual closely enough for the
// vendored drivers: strict primitive identity plus prototype-aware object
// comparison for the tested shapes.
function deepEqual(a, b) {
  if (Object.is(a, b)) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return false;
  if (typeof a === 'object') {
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (a.constructor !== b.constructor) return false;
    if (a instanceof Error && b instanceof Error) {
      return a.name === b.name && a.message === b.message;
    }
    const keysA = Object.keys(a);
    const keysB = Object.keys(b);
    if (keysA.length !== keysB.length) return false;
    for (const key of keysA) {
      if (!Object.prototype.hasOwnProperty.call(b, key)) return false;
      if (!deepEqual(a[key], b[key])) return false;
    }
    return true;
  }
  return false;
}

assert.deepStrictEqual = function deepStrictEqual(actual, expected, message) {
  if (!deepEqual(actual, expected)) {
    throw new AssertionError({
      actual,
      expected,
      operator: 'deepStrictEqual',
      message: message ||
        `Expected deep strict equality: ${fmt(actual)} vs ${fmt(expected)}`,
    });
  }
};

assert.match = function match(string, regexp, message) {
  if (typeof string !== 'string' || !(regexp instanceof RegExp)) {
    throw new TypeError('assert.match needs (string, RegExp)');
  }
  if (!regexp.test(string)) {
    throw new AssertionError(
      message || `${fmt(string)} does not match ${regexp}`);
  }
};

assert.throws = function throws(fn, matcher, message) {
  let threw = false;
  let err;
  try {
    fn();
  } catch (e) {
    threw = true;
    err = e;
  }
  if (!threw) {
    throw new AssertionError('Expected function to throw');
  }
  if (matcher == null) return err;
  if (typeof matcher === 'string') {
    // Node's assert.throws(fn, error?, message?): a string in the
    // second position is the assertion *message*, not a matcher —
    // any thrown error satisfies it (verified under real Node 22:
    // throws(fn, 'Error: xyz') passes for any message). Vendored
    // globalObject drivers pass 'Error: A string/number was expected'
    // as strings, so they are vacuous under Node; mirror Node here
    // instead of inventing a substring check.
    return err;
  }
  if (typeof matcher === 'function') {
    // Node: an Error subclass checks instanceof; any other function is a
    // validator that must return true for the caught error.
    let isErrorClass = false;
    try {
      isErrorClass = (matcher === Error ||
        (matcher.prototype && matcher.prototype instanceof Error));
    } catch { isErrorClass = false; }
    if (isErrorClass) {
      if (!(err instanceof matcher)) {
        throw new AssertionError(
          `Expected ${matcher.name}, got ${err && err.name}`);
      }
      return err;
    }
    // Validator function form (e.g. (err) => err === theError).
    let ok = false;
    try {
      ok = matcher(err) === true;
    } catch (e) {
      throw new AssertionError(`throws validator threw: ${e && e.message}`);
    }
    if (!ok) {
      throw new AssertionError('throws validator returned false');
    }
    return err;
  }
  if (matcher instanceof RegExp) {
    // Node tests the RegExp against String(err) ("Error: <msg>"),
    // which is why vendored tests use /^Error: ...$/ and /^TypeError: ...$/.
    const text = String(err);
    if (!matcher.test(text)) {
      throw new AssertionError(
        `Expected error message matching ${matcher}, got ${fmt(text)}`);
    }
    return err;
  }
  if (typeof matcher === 'object') {
    for (const [key, expected] of Object.entries(matcher)) {
      const actual = err && err[key];
      if (expected instanceof RegExp) {
        // Node: RegExp expectations test against String(err[key]).
        if (!expected.test(String(actual))) {
          throw new AssertionError(
            `Expected ${key} matching ${expected}, got ${fmt(String(actual))}`);
        }
      } else if (actual !== expected) {
        throw new AssertionError(
          `Expected error.${key} === ${fmt(expected)}, got ${fmt(actual)}`);
      }
    }
    return err;
  }
  throw new TypeError('assert.throws: unsupported matcher');
};

assert.fail = function fail(message) {
  throw new AssertionError(message || 'Assertion failed');
};

assert.AssertionError = AssertionError;

module.exports = assert;
