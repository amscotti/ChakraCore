//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// S11 remainder: Object.groupBy, Map.groupBy, RegExp.escape, Promise.try,
// Promise.withResolvers, String.prototype.isWellFormed, and
// String.prototype.toWellFormed. Synchronous checks run immediately; the
// promise checks collect into `promises`, and the file prints one final
// "pass" only when both phases are clean (the for-await-of.js shape).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const promises = [];
let syncFailures = 0;
const subclassRejection = new Error("subclass boom");

function runTest(name, body) {
    try {
        body();
    } catch (ex) {
        syncFailures++;
        helpers.writeln(`*** FAILED sync test: ${name}`);
        helpers.writeln(ex.stack || ex.message || ex);
    }
}

// The promise resolves with `expected` (strictEqual).
function checkResolves(promise, expected, name) {
    promises.push(promise.then(
        (value) => { assert.strictEqual(expected, value, name); },
        (err) => { assert.fail(name + ": unexpected rejection " + err); }
    ));
}

// The promise rejects with an Error whose message is `message`.
function checkRejectsWith(promise, message, name) {
    promises.push(promise.then(
        () => { assert.fail(name + ": expected a rejection"); },
        (err) => { assert.areEqual(message, err.message, name); }
    ));
}

// The promise rejects with the exact value `value` (identity).
function checkRejectsWithValue(promise, value, name) {
    promises.push(promise.then(
        () => { assert.fail(name + ": expected a rejection"); },
        (err) => { assert.strictEqual(value, err, name); }
    ));
}

// The promise rejects with a TypeError.
function checkRejectsWithTypeError(promise, name) {
    promises.push(promise.then(
        () => { assert.fail(name + ": expected a rejection"); },
        (err) => { assert.isTrue(err instanceof TypeError, name + ": got " + err); }
    ));
}

// An instrumented iterable whose iterator records `return` calls;
// nextResult(n) supplies the n-th next() result.
function spiedIterable(nextResult) {
    const log = { nextCalls: 0, returnCalls: 0 };
    const iterator = {
        next() { log.nextCalls++; return nextResult(log.nextCalls); },
        return(v) { log.returnCalls++; return { done: true, value: v }; },
    };
    const items = {};
    items[Symbol.iterator] = function () { return iterator; };
    return { items: items, log: log };
}

// Runs groupBy over a spied iterable and captures the outcome (the groupBy
// iterator-close probes; behavior verified against Node 26.5.0).
function groupByOutcome(groupBy, nextResult, callback) {
    const spy = spiedIterable(nextResult);
    let error = null;
    let result = null;
    try {
        result = groupBy(spy.items, callback);
    } catch (e) {
        error = e;
    }
    return { error: error, result: result, log: spy.log };
}

// The three abrupt next() shapes that must propagate WITHOUT closing the
// iterator (IteratorStepValue has no IfAbruptCloseIterator): a throwing
// next call, a throwing done getter, and a throwing value getter.
function noCloseThrowers() {
    const nextErr = new TypeError("next-boom");
    const doneErr = new TypeError("done-boom");
    const valueErr = new TypeError("value-boom");
    return [
        ["next() throw", nextErr, function () { throw nextErr; }],
        ["done getter throw", doneErr, function () { return { get done() { throw doneErr; }, value: 1 }; }],
        ["value getter throw", valueErr, function () { return { done: false, get value() { throw valueErr; } }; }],
    ];
}

// Asserts one no-close shape: TypeError identity, next() reached, `return`
// untouched.
function checkNoClose(label, groupBy, nextResult, expected) {
    const outcome = groupByOutcome(groupBy, nextResult, function () { return "k"; });
    assert.isTrue(outcome.error instanceof TypeError, label + " throws TypeError");
    assert.strictEqual(expected, outcome.error, label + " surfaces the original error");
    assert.areEqual(1, outcome.log.nextCalls, label + " reached next()");
    assert.areEqual(0, outcome.log.returnCalls, label + " leaves the iterator open");
}

const tests = [
    {
        name: "all seven are installed with the right length, name, and no constructor behavior",
        body() {
            const specs = [
                [Object, "groupBy", 2], [Map, "groupBy", 2], [RegExp, "escape", 1],
                [Promise, "try", 1], [Promise, "withResolvers", 0],
                [String.prototype, "isWellFormed", 0], [String.prototype, "toWellFormed", 0],
            ];
            for (const [owner, name, length] of specs) {
                const fn = owner[name];
                assert.areEqual("function", typeof fn, name + " is a function");
                assert.areEqual(length, fn.length, name + " length");
                assert.areEqual(name, fn.name, name + " name");
                assert.isFalse(Object.prototype.hasOwnProperty.call(fn, "prototype"), name + " has no prototype property");
                assert.throws(() => { new fn(); }, TypeError, name + " is not a constructor");
            }
        }
    },
    {
        name: "the new properties are writable, non-enumerable, configurable",
        body() {
            for (const [owner, name] of [[Object, "groupBy"], [Map, "groupBy"], [RegExp, "escape"], [Promise, "try"], [Promise, "withResolvers"]]) {
                const d = Object.getOwnPropertyDescriptor(owner, name);
                assert.isTrue(d.writable, name + " writable");
                assert.isFalse(d.enumerable, name + " non-enumerable");
                assert.isTrue(d.configurable, name + " configurable");
            }
            for (const name of ["isWellFormed", "toWellFormed"]) {
                const d = Object.getOwnPropertyDescriptor(String.prototype, name);
                assert.isTrue(d.writable, name + " writable");
                assert.isFalse(d.enumerable, name + " non-enumerable");
                assert.isTrue(d.configurable, name + " configurable");
            }
        }
    },

    // ---- Object.groupBy ----
    {
        name: "Object.groupBy groups by coerced key in first-encounter order",
        body() {
            const array = [1, 2, 3];
            const obj = Object.groupBy(array, function (i) { return i % 2 === 0 ? "even" : "odd"; });
            assert.areEqual("odd,even", Object.keys(obj).join(","), "key order");
            assert.areEqual("2", obj.even.join(","), "even group");
            assert.areEqual("1,3", obj.odd.join(","), "odd group");
            assert.areEqual(0, Object.keys(Object.groupBy([], function () { return "k"; })).length, "empty iterable gives no keys");
        }
    },
    {
        name: "Object.groupBy returns a null-prototype object with array values",
        body() {
            const obj = Object.groupBy([1, 2], function (i) { return i; });
            assert.areEqual(null, Object.getPrototypeOf(obj), "null prototype");
            assert.areEqual(undefined, obj.hasOwnProperty, "no inherited members");
            assert.isFalse(obj instanceof Object, "instanceof Object is false");
            assert.isTrue(Array.isArray(obj[1]), "groups are arrays");
            const marker = {};
            const o2 = Object.groupBy([marker], function () { return "m"; });
            assert.strictEqual(marker, o2.m[0], "values are not copied");
        }
    },
    {
        name: "Object.groupBy coerces keys with ToPropertyKey",
        body() {
            const byLen = Object.groupBy(["test", "hello", "world"], function (s) { return s.length; });
            assert.areEqual("4,5", Object.keys(byLen).join(","), "number keys become strings");
            const bySign = Object.groupBy([-0, 0], function (x) { return x; });
            assert.areEqual("0", Object.keys(bySign).join(","), "-0 key is the string 0");
            const nully = Object.groupBy([1, 2, 3], function (x) { return x === 1 ? null : undefined; });
            assert.areEqual("null,undefined", Object.keys(nully).join(","), "null and undefined keys");
            assert.areEqual("1", nully.null.join(","), "null group content");
            assert.areEqual("2,3", nully.undefined.join(","), "undefined group content");
            assert.areEqual("true", Object.keys(Object.groupBy([1], function () { return true; })).join(","), "boolean key");
            const stringable = Object.groupBy([1, "1"], function (x) { return { toString() { return "one"; } }; });
            assert.areEqual("one", Object.keys(stringable).join(","), "object key uses toString");
            assert.areEqual("1,1", stringable.one.join(","), "both values grouped");
            const sym = Symbol("s");
            const bySym = Object.groupBy([7], function () { return sym; });
            assert.areEqual(1, Object.getOwnPropertySymbols(bySym).length, "symbol key recorded");
            assert.strictEqual(sym, Object.getOwnPropertySymbols(bySym)[0], "symbol key identity");
            assert.areEqual(7, bySym[sym][0], "symbol group value");
        }
    },
    {
        name: "Object.groupBy calls the callback with (value, index) and this undefined",
        body() {
            const arr = [5, 6, 7];
            let calls = 0;
            Object.groupBy(arr, function (n, i) {
                calls++;
                assert.areEqual(arr[i], n, "value aligns with index");
                assert.areEqual(calls - 1, i, "index counts from zero");
                assert.areEqual(2, arguments.length, "exactly two arguments");
                return "k";
            });
            assert.areEqual(3, calls, "called for every element");
            let seenThis = "unset";
            Object.groupBy([1], function () { "use strict"; seenThis = this; return "k"; });
            assert.areEqual(undefined, seenThis, "strict callback sees undefined this");
        }
    },
    {
        name: "Object.groupBy consumes iterators and strings",
        body() {
            const s = "\uD83E\uDD70\uD83D\uDCA9\uD83D\uDE4F\uD83D\uDE08";
            const byHalf = Object.groupBy(s, function (ch) { return ch < "\uD83D\uDE4F" ? "before" : "after"; });
            assert.areEqual("after,before", Object.keys(byHalf).join(","), "string iterates by code point");
            assert.areEqual("\uD83E\uDD70,\uD83D\uDE4F", byHalf.after.join(","), "after group keeps code points");
            assert.areEqual("\uD83D\uDCA9,\uD83D\uDE08", byHalf.before.join(","), "before group");
            let steps = 0;
            const iterable = {};
            iterable[Symbol.iterator] = function () {
                let i = 0;
                return { next() { steps++; return i < 2 ? { done: false, value: i++ } : { done: true }; } };
            };
            const o = Object.groupBy(iterable, function (v) { return v === 0 ? "a" : "b"; });
            assert.areEqual(3, steps, "iterator stepped to exhaustion");
            assert.areEqual("a,b", Object.keys(o).join(","), "iterator values grouped");
        }
    },
    {
        name: "Object.groupBy error paths: non-callable first, then iterator failures",
        body() {
            const boom = function () { throw new Error("should not run"); };
            for (const bad of [undefined, null, {}, 42]) {
                assert.throws(() => Object.groupBy([1], bad), TypeError, "non-callable " + bad);
            }
            // The IsCallable check happens before GetIterator.
            let iterated = false;
            const recording = {};
            Object.defineProperty(recording, Symbol.iterator, { get() { iterated = true; return function () { return [][Symbol.iterator](); }; } });
            assert.throws(() => Object.groupBy(recording, undefined), TypeError, "check order");
            assert.isFalse(iterated, "iterator not read when callback is non-callable");
            const nullish1 = {}; nullish1[Symbol.iterator] = undefined;
            const nullish2 = {}; nullish2[Symbol.iterator] = null;
            assert.throws(() => Object.groupBy(nullish1, boom), TypeError, "undefined Symbol.iterator");
            assert.throws(() => Object.groupBy(nullish2, boom), TypeError, "null Symbol.iterator");
            const throwingNext = {}; throwingNext[Symbol.iterator] = function () { return { next() { throw new Error("next"); } }; };
            assert.throws(() => Object.groupBy(throwingNext, boom), Error, "next throw propagates");
            assert.throws(() => Object.groupBy([1], function () { throw new Error("cb"); }), Error, "callback throw propagates");
            assert.throws(() => Object.groupBy([1], function () { return { toString() { throw new Error("key"); } }; }), Error, "ToPropertyKey throw propagates");
        }
    },
    {
        name: "Object.groupBy: abrupt next(), done, or value propagates without closing the iterator",
        body() {
            for (const [label, expected, nextResult] of noCloseThrowers()) {
                checkNoClose("Object.groupBy " + label, Object.groupBy, nextResult, expected);
            }
            // A non-object next() result is also an IteratorStepValue abrupt.
            const outcome = groupByOutcome(Object.groupBy, function () { return 42; }, function () { return "k"; });
            assert.isTrue(outcome.error instanceof TypeError, "non-object next result throws TypeError");
            assert.areEqual(1, outcome.log.nextCalls, "non-object result reached next()");
            assert.areEqual(0, outcome.log.returnCalls, "non-object result leaves the iterator open");
        }
    },
    {
        name: "Object.groupBy closes the iterator once when the callback or ToPropertyKey throws",
        body() {
            const ok = function () { return { done: false, value: 1 }; };
            const cbErr = new TypeError("cb-boom");
            let outcome = groupByOutcome(Object.groupBy, ok, function () { throw cbErr; });
            assert.strictEqual(cbErr, outcome.error, "callback error surfaces");
            assert.areEqual(1, outcome.log.returnCalls, "callback throw closes the iterator");

            const keyErr = new TypeError("key-boom");
            const badKey = {};
            badKey[Symbol.toPrimitive] = function () { throw keyErr; };
            outcome = groupByOutcome(Object.groupBy, ok, function () { return badKey; });
            assert.strictEqual(keyErr, outcome.error, "ToPropertyKey error surfaces");
            assert.areEqual(1, outcome.log.returnCalls, "ToPropertyKey throw closes the iterator");
        }
    },

    // ---- Map.groupBy ----
    {
        name: "Map.groupBy returns a real Map with uncoerced keys and -0 normalized to +0",
        body() {
            const array = [1, 2, 3];
            const map = Map.groupBy(array, function (i) { return i % 2 === 0 ? "even" : "odd"; });
            assert.isTrue(map instanceof Map, "result is a Map");
            assert.areEqual(Map.prototype, Object.getPrototypeOf(map), "plain Map prototype");
            assert.areEqual("odd,even", Array.from(map.keys()).join(","), "key order");
            assert.areEqual("2", map.get("even").join(","), "even group");
            assert.areEqual("1,3", map.get("odd").join(","), "odd group");

            const byValue = Map.groupBy([1, "1"], function (v) { return v; });
            assert.areEqual(2, byValue.size, "1 and '1' are distinct keys");
            assert.areEqual(1, byValue.get(1)[0], "numeric key");
            assert.areEqual("1", byValue.get("1")[0], "string key");

            const bySign = Map.groupBy([-0, 0], function (x) { return x; });
            assert.areEqual(1, bySign.size, "-0 and +0 share one group");
            const signValues = bySign.get(0);
            assert.areEqual(2, signValues.length, "both values kept");
            assert.isTrue(Object.is(-0, signValues[0]), "first value is -0");
            assert.isTrue(Object.is(0, signValues[1]), "second value is +0");
            assert.areEqual(Infinity, 1 / bySign.keys().next().value, "stored key is +0");

            const objKey = {};
            const byObj = Map.groupBy([objKey], function () { return objKey; });
            assert.strictEqual(objKey, Array.from(byObj.keys())[0], "object keys allowed");
        }
    },
    {
        name: "Map.groupBy error paths",
        body() {
            const boom = function () { throw new Error("should not run"); };
            for (const bad of [undefined, null, {}, 42]) {
                assert.throws(() => Map.groupBy([1], bad), TypeError, "non-callable " + bad);
            }
            const nullish = {}; nullish[Symbol.iterator] = undefined;
            assert.throws(() => Map.groupBy(nullish, boom), TypeError, "undefined Symbol.iterator");
            assert.throws(() => Map.groupBy([1], function () { throw new Error("cb"); }), Error, "callback throw propagates");
        }
    },
    {
        name: "Map.groupBy: abrupt next(), done, or value propagates unclosed; callback throw closes once; keys are not coerced",
        body() {
            for (const [label, expected, nextResult] of noCloseThrowers()) {
                checkNoClose("Map.groupBy " + label, Map.groupBy, nextResult, expected);
            }
            const nonObject = groupByOutcome(Map.groupBy, function () { return 42; }, function () { return "k"; });
            assert.isTrue(nonObject.error instanceof TypeError, "non-object next result throws TypeError");
            assert.areEqual(0, nonObject.log.returnCalls, "non-object result leaves the iterator open");

            const ok = function () { return { done: false, value: 1 }; };
            const cbErr = new TypeError("cb-boom");
            const outcome = groupByOutcome(Map.groupBy, ok, function () { throw cbErr; });
            assert.strictEqual(cbErr, outcome.error, "callback error surfaces");
            assert.areEqual(1, outcome.log.returnCalls, "callback throw closes the iterator");

            // No key coercion site exists, so a coercible key never runs user
            // code and the iterator runs to exhaustion unclosed.
            const finite = function (n) { return n <= 1 ? { done: false, value: n } : { done: true }; };
            const touched = [];
            const coercible = {};
            coercible[Symbol.toPrimitive] = function (hint) { touched.push(hint); return "s"; };
            const keyMap = groupByOutcome(Map.groupBy, finite, function () { return coercible; });
            assert.strictEqual(null, keyMap.error, "coercible key does not throw");
            assert.areEqual(0, touched.length, "Map.groupBy never coerces keys");
            assert.areEqual(1, keyMap.result.size, "one group under the raw key");
            assert.strictEqual(coercible, Array.from(keyMap.result.keys())[0], "key stored by identity");
            assert.areEqual(0, keyMap.log.returnCalls, "no close on the non-coercion path");
        }
    },

    // ---- RegExp.escape ----
    {
        name: "RegExp.escape escapes syntax characters and solidus with a backslash",
        body() {
            const syntax = ".*+?^$|()[]{}\\/";
            let expected = "";
            for (const c of syntax) { expected += "\\" + c; }
            assert.areEqual(expected, RegExp.escape(syntax), "every syntax character");
            assert.areEqual("\\.", RegExp.escape("."), "dot");
            assert.areEqual("\\/", RegExp.escape("/"), "solidus");
            assert.areEqual("\\.\\/a\\\\\\/\\*b\\+c\\?d\\^e\\$f\\|g\\{2\\}h\\[i\\]j\\\\k",
                RegExp.escape("./a\\/*b+c?d^e$f|g{2}h[i]j\\k"), "mixed");
        }
    },
    {
        name: "RegExp.escape uses control escapes for U+0009 to U+000D",
        body() { assert.areEqual("\\t\\n\\v\\f\\r", RegExp.escape("\t\n\v\f\r"), "control set"); }
    },
    {
        name: "RegExp.escape hex-escapes the other punctuators",
        body() {
            const other = ",-=<>#&!%:;@~'`\"";
            const expected = "\\x2c\\x2d\\x3d\\x3c\\x3e\\x23\\x26\\x21\\x25\\x3a\\x3b\\x40\\x7e\\x27\\x60\\x22";
            assert.areEqual(expected, RegExp.escape(other), "full list");
            assert.areEqual("\\x2c", RegExp.escape(","), "comma");
            assert.areEqual("\\x22", RegExp.escape('"'), "quotation mark");
        }
    },
    {
        name: "RegExp.escape escapes the WhiteSpace and LineTerminator sets",
        body() {
            assert.areEqual("\\x20", RegExp.escape(" "), "space");
            assert.areEqual("\\xa0", RegExp.escape("\u00A0"), "nbsp");
            assert.areEqual("\\u1680", RegExp.escape("\u1680"), "ogham space");
            let zs = "";
            let zsExpected = "";
            for (let u = 0x2000; u <= 0x200a; u++) { zs += String.fromCharCode(u); zsExpected += "\\u" + u.toString(16); }
            assert.areEqual(zsExpected, RegExp.escape(zs), "U+2000..U+200A");
            assert.areEqual("\\u202f", RegExp.escape("\u202F"), "narrow nbsp");
            assert.areEqual("\\u205f", RegExp.escape("\u205F"), "medium math space");
            assert.areEqual("\\u3000", RegExp.escape("\u3000"), "ideographic space");
            assert.areEqual("\\ufeff", RegExp.escape("\uFEFF"), "zwnbsp");
            assert.areEqual("\\u2028\\u2029", RegExp.escape("\u2028\u2029"), "line separators");
        }
    },
    {
        name: "RegExp.escape escapes lone surrogates and passes pairs through",
        body() {
            assert.areEqual("\\ud800", RegExp.escape("\uD800"), "lone high");
            assert.areEqual("\\udbff", RegExp.escape("\uDBFF"), "last high");
            assert.areEqual("\\udc00", RegExp.escape("\uDC00"), "lone low");
            assert.areEqual("\\udfff", RegExp.escape("\uDFFF"), "last low");
            assert.areEqual("\\ud800x", RegExp.escape("\uD800x"), "high followed by text is lone");
            assert.areEqual("\uD800\uDC00", RegExp.escape("\uD800\uDC00"), "pair untouched");
            assert.areEqual("\uD800\uDC00", RegExp.escape("\uD800\uDC00x".slice(0, 2)), "pair via slice untouched");
            assert.areEqual("\\x61\uD83D\uDCA9c", RegExp.escape("a\uD83D\uDCA9c"), "pair inside text untouched after the initial letter");
            assert.areEqual("\\udc00x\\ud800", RegExp.escape("\uDC00x\uD800"), "wrong order escaped");
        }
    },
    {
        name: "RegExp.escape escapes an initial digit or ASCII letter as \\xNN",
        body() {
            assert.areEqual("\\x61aa", RegExp.escape("aaa"), "letter");
            assert.areEqual("\\x30000", RegExp.escape("0000"), "digit");
            assert.areEqual("\\x31\\.5", RegExp.escape("1.5"), "digit then dot");
            assert.areEqual("\\x61\\*a", RegExp.escape("a*a"), "letter then star");
            assert.areEqual("\\x5aZZ", RegExp.escape("ZZZ"), "uppercase");
            assert.areEqual("\\x68ello_world", RegExp.escape("hello_world"), "letters later are untouched");
            assert.areEqual("", RegExp.escape(""), "empty string");
        }
    },
    {
        name: "RegExp.escape leaves everything else verbatim",
        body() {
            assert.areEqual("\\x61bz09", RegExp.escape("abz09"), "only the initial letter is hex-escaped");
            assert.areEqual("_b_z09", RegExp.escape("_b_z09"), "mid-string letters and digits");
            assert.areEqual("_", RegExp.escape("_"), "underscore");
            assert.areEqual("\u4F60\u597D", RegExp.escape("\u4F60\u597D"), "BMP text");
            assert.areEqual("\uD7FF", RegExp.escape("\uD7FF"), "just below surrogates");
            assert.areEqual("\uE000", RegExp.escape("\uE000"), "just above surrogates");
            assert.areEqual("\uFFFD", RegExp.escape("\uFFFD"), "replacement char");
            assert.areEqual("\uD83D\uDE00", RegExp.escape("\uD83D\uDE00"), "emoji pair");
        }
    },
    {
        name: "RegExp.escape output matches the input when used as a pattern",
        body() {
            const nasty = ".a*b+c?(d)^e$f|g{2}h[i]j\\k/l,-=<>#&!%:;@~'`\"m n\u00A0\u2028 o";
            assert.isTrue(new RegExp("^" + RegExp.escape(nasty) + "$").test(nasty), "round trip");
            assert.isTrue(new RegExp(RegExp.escape("aaa")).test("xaaay"), "escaped initial letter matches as literal");
            assert.areEqual("1.5,1.5", "1.5 2.5 1.5".match(new RegExp(RegExp.escape("1.5"), "g")).join(","), "dot is literal");
        }
    },
    {
        name: "RegExp.escape throws TypeError for non-string inputs",
        body() {
            for (const bad of [123, {}, [], null, undefined, true, Symbol("s")]) {
                assert.throws(() => RegExp.escape(bad), TypeError, "non-string input");
            }
            assert.throws(() => RegExp.escape(new String(".")), TypeError, "String object is not a string");
        }
    },

    // ---- Promise.try ----
    {
        name: "Promise.try calls the function synchronously with forwarded arguments",
        body() {
            const sentinel = { sentinel: true };
            let seen = null;
            const p = Promise.try(function (a, b, c) {
                seen = { args: [a, b, c], count: arguments.length };
                return 1;
            }, 1, "two", Error, sentinel);
            assert.areEqual(1, seen.args[0], "first arg");
            assert.areEqual("two", seen.args[1], "second arg");
            assert.areEqual(Error, seen.args[2], "third arg");
            assert.areEqual(4, seen.count, "argument count includes the sentinel");
            assert.isTrue(p instanceof Promise, "returns a promise");
            checkResolves(Promise.try(function () { return 42; }), 42, "resolves with return value");
            checkResolves(Promise.try(function () { }), undefined, "undefined return resolves undefined");
            checkRejectsWith(Promise.try(function () { return Promise.reject(new Error("rej")); }), "rej", "returned rejected promise assimilates");
        }
    },
    {
        name: "Promise.try passes undefined this and converts a throw into a rejection",
        body() {
            let seenThis = "unset";
            Promise.try(function () { "use strict"; seenThis = this; });
            assert.areEqual(undefined, seenThis, "strict callback this");
            const marker = new Error("boom");
            checkRejectsWithValue(Promise.try(function () { throw marker; }), marker, "sync throw rejects with the error");
        }
    },
    {
        name: "Promise.try with a non-callable rejects with a TypeError",
        body() { checkRejectsWithTypeError(Promise.try(undefined), "undefined callback"); }
    },
    {
        name: "Promise.try resolves through PromiseResolve: promises and thenables are assimilated",
        body() {
            const sentinel = Promise.resolve();
            assert.strictEqual(sentinel, Promise.try(function () { return sentinel; }), "native promise returned unwrapped");
            class SubPromise extends Promise { }
            const sub = SubPromise.resolve();
            assert.strictEqual(sub, SubPromise.try(function () { return sub; }), "subclass promise unwrapped for its own constructor");
            const subInstance = Promise.try.call(SubPromise, function () { });
            assert.isTrue(subInstance instanceof SubPromise, "instance of the receiver");
            checkResolves(Promise.try(function () { return { then(res) { res("thenable"); } }; }), "thenable", "thenable assimilation");
        }
    },
    {
        name: "Promise.try receiver must be a constructor; constructor errors propagate",
        body() {
            for (const bad of [undefined, null, 86, "string", true, Symbol(), eval]) {
                assert.throws(() => Promise.try.call(bad, function () { }), TypeError, "non-constructor receiver");
            }
            function ThrowingCtor() { throw new Error("ctor"); }
            assert.throws(() => Promise.try.call(ThrowingCtor, function () { }), Error, "throwing constructor propagates synchronously");
            let executor = null;
            let count = 0;
            class Counting extends Promise {
                constructor(a) { super(a); executor = a; count++; }
            }
            const inst = Promise.try.call(Counting, function () { return 9; });
            assert.areEqual(1, count, "constructed once");
            assert.areEqual("function", typeof executor, "executor is a function");
            assert.isTrue(inst instanceof Counting, "instance of subclass");
            checkResolves(inst, 9, "subclass promise resolves with value");
            checkRejectsWithValue(Counting.try(function () { throw subclassRejection; }), subclassRejection, "subclass rejection");
        }
    },

    // ---- Promise.withResolvers ----
    {
        name: "Promise.withResolvers returns the promise/resolve/reject triple on a plain object",
        body() {
            const w = Promise.withResolvers();
            assert.areEqual("object", typeof w, "plain object");
            assert.areEqual(Object.prototype, Object.getPrototypeOf(w), "Object.prototype");
            assert.areEqual("promise,resolve,reject", Object.keys(w).join(","), "own keys in order");
            assert.isTrue(w.promise instanceof Promise, "promise member");
            assert.areEqual("function", typeof w.resolve, "resolve member");
            assert.areEqual("function", typeof w.reject, "reject member");
            assert.areEqual("", w.resolve.name, "resolve is anonymous");
            assert.areEqual("", w.reject.name, "reject is anonymous");
            assert.areEqual(1, w.resolve.length, "resolve length");
            assert.areEqual(1, w.reject.length, "reject length");
            for (const key of ["promise", "resolve", "reject"]) {
                const d = Object.getOwnPropertyDescriptor(w, key);
                assert.isTrue(d.writable, key + " writable");
                assert.isTrue(d.enumerable, key + " enumerable");
                assert.isTrue(d.configurable, key + " configurable");
            }
            w.resolve(42);
            checkResolves(w.promise, 42, "resolve settles the promise");
            const w2 = Promise.withResolvers();
            const reason = { reason: true };
            w2.reject(reason);
            checkRejectsWithValue(w2.promise, reason, "reject settles the promise");
            const w3 = Promise.withResolvers();
            w3.resolve(1);
            w3.resolve(2); // already resolved; the second call is a no-op
            checkResolves(w3.promise, 1, "resolve is one-shot");
        }
    },
    {
        name: "Promise.withResolvers receiver must be a constructor and builds subclass promises",
        body() {
            for (const bad of [undefined, null, 86, "string", true, Symbol(), eval]) {
                assert.throws(() => Promise.withResolvers.call(bad), TypeError, "non-constructor receiver");
            }
            class SubPromise extends Promise { }
            const w = Promise.withResolvers.call(SubPromise);
            assert.isTrue(w.promise instanceof SubPromise, "promise is a subclass instance");
            assert.areEqual(SubPromise, w.promise.constructor, "constructor is the receiver");
            w.resolve("sub");
            checkResolves(w.promise, "sub", "subclass resolve works");
            function ThrowingCtor() { throw new Error("ctor"); }
            assert.throws(() => Promise.withResolvers.call(ThrowingCtor), Error, "throwing constructor propagates");
        }
    },

    // ---- String.prototype.isWellFormed / toWellFormed ----
    {
        name: "isWellFormed detects every kind of lone surrogate",
        body() {
            const hi = "\uD83D", lo = "\uDCA9";
            assert.isFalse(("a" + hi + "c").isWellFormed(), "lone leading");
            assert.isFalse(("a" + lo + "e").isWellFormed(), "lone trailing");
            assert.isFalse((lo + hi + "d").isWellFormed(), "wrong order");
            assert.isFalse(hi.isWellFormed(), "just a leading half");
            assert.isFalse(lo.isWellFormed(), "just a trailing half");
            assert.isTrue((hi + lo).isWellFormed(), "concatenated pair");
            assert.isTrue("a\uD83D\uDCA9c".isWellFormed(), "escaped pair");
            assert.isFalse((hi + lo).slice(0, 1).isWellFormed(), "sliced half is lone");
            assert.isTrue("abc".isWellFormed(), "latin-1 text");
            assert.isTrue("a\u25A8c".isWellFormed(), "non-ASCII BMP");
            assert.isTrue("\uFFFD".isWellFormed(), "replacement character");
            assert.isFalse(("\uD800\uD800").isWellFormed(), "two leadings");
            assert.isFalse(("\uDC00\uD800").isWellFormed(), "trailing then leading is not a pair");
        }
    },
    {
        name: "toWellFormed replaces each lone surrogate with U+FFFD and keeps pairs",
        body() {
            assert.areEqual("\uFFFD", "\uD83D".toWellFormed(), "lone leading replaced");
            assert.areEqual("\uFFFD", "\uDCA9".toWellFormed(), "lone trailing replaced");
            assert.areEqual("\uFFFD\uFFFD", "\uD800\uD800".toWellFormed(), "two lone leadings");
            assert.areEqual("\uFFFDx\uFFFD", "\uDC00x\uD800".toWellFormed(), "each lone surrogate separately");
            assert.areEqual("\uD800\uDC00", "\uD800\uDC00".toWellFormed(), "pair untouched");
            assert.areEqual("a\uD83D\uDCA9c", "a\uD83D\uDCA9c".toWellFormed(), "pair inside text untouched");
            assert.areEqual("\uFFFD\uD800\uDC00", "\uD800\uD800\uDC00".toWellFormed(), "lone half before a real pair");
            assert.areEqual("abc", "abc".toWellFormed(), "well-formed text unchanged");
            assert.areEqual(4, "a\uD800b\uDC00".toWellFormed().length, "length preserved");
            assert.areEqual("\uFFFD", ("\uD800\uDC00").slice(0, 1).toWellFormed(), "sliced half replaced");
        }
    },
    {
        name: "isWellFormed and toWellFormed coerce the receiver with ToString",
        body() {
            assert.isTrue(String.prototype.isWellFormed.call(1), "number receiver");
            assert.areEqual("true", String.prototype.toWellFormed.call(true), "boolean receiver");
            assert.areEqual("42", String.prototype.toWellFormed.call({ toString() { return "42"; } }), "object receiver via toString");
            assert.throws(() => String.prototype.isWellFormed.call(undefined), TypeError, "undefined receiver");
            assert.throws(() => String.prototype.isWellFormed.call(null), TypeError, "null receiver");
            assert.throws(() => String.prototype.toWellFormed.call(undefined), TypeError, "undefined receiver toWellFormed");
            assert.throws(() => String.prototype.toWellFormed.call(null), TypeError, "null receiver toWellFormed");
            assert.throws(() => String.prototype.isWellFormed.call(Symbol("s")), TypeError, "symbol receiver");
            assert.throws(() => String.prototype.isWellFormed.call({ toString() { throw new Error("ts"); } }), Error, "toString throw propagates");
            assert.throws(() => String.prototype.toWellFormed.call({ toString() { throw new Error("ts"); } }), Error, "toString throw propagates in toWellFormed");
        }
    },
];

for (const test of tests) {
    runTest(test.name, test.body);
}

Promise.all(promises).then(
    () => {
        if (syncFailures === 0) {
            print("pass");
        } else {
            print(`${syncFailures} sync test(s) failed`);
        }
    },
    (err) => print("async failure: " + ((err && (err.stack || err.message)) || err))
);
