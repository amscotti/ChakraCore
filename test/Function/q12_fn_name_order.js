//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertEqual(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + " (expected: " + String(expected) + ", actual: " + String(actual) + ")");
    }
}

function assertArrayEqual(actual, expected, msg) {
    assertEqual(actual.length, expected.length, msg + " [length]");
    for (let i = 0; i < expected.length; i++) {
        assertEqual(actual[i], expected[i], msg + " [index " + i + "]");
    }
}

function assertDataProp(obj, prop, value, writable, enumerable, configurable, label) {
    const desc = Object.getOwnPropertyDescriptor(obj, prop);
    assert(desc !== undefined, label + " missing own property '" + String(prop) + "'");
    assertEqual(desc.value, value, label + "." + String(prop) + ".value");
    assertEqual(desc.writable, writable, label + "." + String(prop) + ".writable");
    assertEqual(desc.enumerable, enumerable, label + "." + String(prop) + ".enumerable");
    assertEqual(desc.configurable, configurable, label + "." + String(prop) + ".configurable");
}

function assertLengthThenNameOrder(fn, label) {
    const names = Object.getOwnPropertyNames(fn);
    const lenIdx = names.indexOf("length");
    const nameIdx = names.indexOf("name");
    assert(lenIdx >= 0, label + " must own length");
    assertEqual(nameIdx, lenIdx + 1, label + " must have name immediately after length; got " + JSON.stringify(names));
}

// 1. Built-in constructors: Object and Promise own-property order
(function testBuiltinConstructors() {
    const objKeys = Object.getOwnPropertyNames(Object);
    const objLenIdx = objKeys.indexOf("length");
    assertEqual(objKeys[objLenIdx + 1], "name", "Object: name immediately follows length");
    assertEqual(objKeys[objLenIdx + 2], "prototype", "Object: prototype immediately follows name");
    assertDataProp(Object, "length", 1, false, false, true, "Object");
    assertDataProp(Object, "name", "Object", false, false, true, "Object");

    const promKeys = Reflect.ownKeys(Promise);
    const promLenIdx = promKeys.indexOf("length");
    const promSpeciesIdx = promKeys.indexOf(Symbol.species);
    assert(promLenIdx >= 0, "Promise must have length");
    assertEqual(promKeys[promLenIdx + 1], "name", "Promise: name immediately follows length");
    assertEqual(promKeys[promLenIdx + 2], "prototype", "Promise: prototype immediately follows name");
    assert(promSpeciesIdx > promLenIdx + 2, "Promise: Symbol.species present after string keys");
    assertDataProp(Promise, "length", 1, false, false, true, "Promise");
    assertDataProp(Promise, "name", "Promise", false, false, true, "Promise");
})();

// 2. %ThrowTypeError%
(function testThrowTypeError() {
    const throwTypeError = Object.getOwnPropertyDescriptor(function () {
        "use strict";
        return arguments;
    }(), "callee").get;

    assertArrayEqual(Object.getOwnPropertyNames(throwTypeError), ["length", "name"], "ThrowTypeError keys");
    assertDataProp(throwTypeError, "length", 0, false, false, false, "ThrowTypeError");
    assertDataProp(throwTypeError, "name", "", false, false, false, "ThrowTypeError");
    assertEqual(Object.isExtensible(throwTypeError), false, "ThrowTypeError extensible");
    assertEqual(Object.isFrozen(throwTypeError), true, "ThrowTypeError frozen");

    const fnProtoCaller = Object.getOwnPropertyDescriptor(Function.prototype, "caller");
    assertEqual(fnProtoCaller.get, throwTypeError, "Function.prototype.caller getter is %ThrowTypeError%");
})();

// 3. Proxy.revocable revocation function
(function testProxyRevoke() {
    const revocable = Proxy.revocable({}, {});
    const revoke = revocable.revoke;
    assertArrayEqual(Object.getOwnPropertyNames(revoke), ["length", "name"], "Proxy revoke keys");
    assertDataProp(revoke, "length", 0, false, false, true, "Proxy revoke");
    assertDataProp(revoke, "name", "", false, false, true, "Proxy revoke");
    assertEqual(Object.prototype.hasOwnProperty.call(revoke, "prototype"), false, "Proxy revoke has no prototype");
    revoke();
    let threw = false;
    try {
        revocable.proxy.a;
    } catch (e) {
        threw = e instanceof TypeError;
    }
    assert(threw, "Revoked proxy still throws TypeError");
})();

// 4. Promise built-in anonymous functions
(function testPromiseAnonymousFunctions() {
    let executorFn;
    let resolveFn;
    let rejectFn;

    function CustomPromise(executor) {
        executorFn = executor;
        executor(function () {}, function () {});
    }
    Promise.resolve.call(CustomPromise);
    assertArrayEqual(Object.getOwnPropertyNames(executorFn), ["length", "name"], "Promise capability executor keys");
    assertDataProp(executorFn, "length", 2, false, false, true, "Promise capability executor");
    assertDataProp(executorFn, "name", "", false, false, true, "Promise capability executor");

    new Promise(function (resolve, reject) {
        resolveFn = resolve;
        rejectFn = reject;
    });
    for (const [label, fn] of [["resolve", resolveFn], ["reject", rejectFn]]) {
        assertArrayEqual(Object.getOwnPropertyNames(fn), ["length", "name"], "Promise " + label + " keys");
        assertDataProp(fn, "length", 1, false, false, true, "Promise " + label);
        assertDataProp(fn, "name", "", false, false, true, "Promise " + label);
    }

    function captureElement(combinator) {
        let captured = null;
        const thenable = {
            then(onFulfilled, onRejected) {
                captured = [onFulfilled, onRejected];
            }
        };
        function Ctor(executor) {
            executor(function () {}, function () {});
        }
        Ctor.resolve = function (x) { return x; };
        combinator.call(Ctor, [thenable]);
        return captured;
    }

    const [allResolve] = captureElement(Promise.all);
    assertArrayEqual(Object.getOwnPropertyNames(allResolve), ["length", "name"], "Promise.all resolveElement keys");
    assertDataProp(allResolve, "length", 1, false, false, true, "Promise.all resolveElement");
    assertDataProp(allResolve, "name", "", false, false, true, "Promise.all resolveElement");

    const [, anyReject] = captureElement(Promise.any);
    assertArrayEqual(Object.getOwnPropertyNames(anyReject), ["length", "name"], "Promise.any rejectElement keys");
    assertDataProp(anyReject, "length", 1, false, false, true, "Promise.any rejectElement");
    assertDataProp(anyReject, "name", "", false, false, true, "Promise.any rejectElement");

    const [settledResolve, settledReject] = captureElement(Promise.allSettled);
    for (const [label, fn] of [["allSettled resolve", settledResolve], ["allSettled reject", settledReject]]) {
        assertArrayEqual(Object.getOwnPropertyNames(fn), ["length", "name"], label + " keys");
        assertDataProp(fn, "length", 1, false, false, true, label);
        assertDataProp(fn, "name", "", false, false, true, label);
    }

    // Promise.prototype.finally: thenFinally (len 1), catchFinally (len 1), and valueThunk/thrower (len 0)
    let thenFinallyFn;
    let catchFinallyFn;
    let thunkFn;
    let throwerFn;
    function CustomFinallyCtor(executor) {
        executor(function () {}, function () {});
        return {
            then(onFulfilled) {
                if (thunkFn === undefined) {
                    thunkFn = onFulfilled;
                } else {
                    throwerFn = onFulfilled;
                }
            }
        };
    }
    CustomFinallyCtor[Symbol.species] = CustomFinallyCtor;
    const fakePromise = {
        constructor: CustomFinallyCtor,
        then(onFulfilled, onRejected) {
            thenFinallyFn = onFulfilled;
            catchFinallyFn = onRejected;
            return this;
        }
    };
    Promise.prototype.finally.call(fakePromise, function () { return 123; });
    for (const [label, fn] of [["thenFinally", thenFinallyFn], ["catchFinally", catchFinallyFn]]) {
        assertArrayEqual(Object.getOwnPropertyNames(fn), ["length", "name"], label + " keys");
        assertDataProp(fn, "length", 1, false, false, true, label);
        assertDataProp(fn, "name", "", false, false, true, label);
    }

    thenFinallyFn(42);
    catchFinallyFn(new Error("boom"));
    for (const [label, fn] of [["valueThunk", thunkFn], ["thrower", throwerFn]]) {
        assertArrayEqual(Object.getOwnPropertyNames(fn), ["length", "name"], label + " keys");
        assertDataProp(fn, "length", 0, false, false, true, label);
        assertDataProp(fn, "name", "", false, false, true, label);
    }
})();

// 5. Anonymous & named JS function expressions, async, generator, arrow, class
(function testJSFunctions() {
    const anonFn = (0, function () {});
    assertArrayEqual(Object.getOwnPropertyNames(anonFn), ["prototype", "length", "name"], "anon function keys");
    assertDataProp(anonFn, "length", 0, false, false, true, "anon function");
    assertDataProp(anonFn, "name", "", false, false, true, "anon function");

    const anonArrow = (0, () => {});
    assertArrayEqual(Object.getOwnPropertyNames(anonArrow), ["length", "name"], "anon arrow keys");
    assertDataProp(anonArrow, "length", 0, false, false, true, "anon arrow");
    assertDataProp(anonArrow, "name", "", false, false, true, "anon arrow");

    const anonGen = (0, function* () {});
    assertArrayEqual(Object.getOwnPropertyNames(anonGen), ["prototype", "length", "name"], "anon generator keys");
    assertDataProp(anonGen, "length", 0, false, false, true, "anon generator");
    assertDataProp(anonGen, "name", "", false, false, true, "anon generator");

    const anonAsync = (0, async function () {});
    assertArrayEqual(Object.getOwnPropertyNames(anonAsync), ["length", "name"], "anon async keys");
    assertDataProp(anonAsync, "length", 0, false, false, true, "anon async");
    assertDataProp(anonAsync, "name", "", false, false, true, "anon async");

    const namedAsync = (0, async function myAsync(a, b) {});
    assertArrayEqual(Object.getOwnPropertyNames(namedAsync), ["length", "name"], "named async keys");
    assertDataProp(namedAsync, "length", 2, false, false, true, "named async");
    assertDataProp(namedAsync, "name", "myAsync", false, false, true, "named async");

    const anonAsyncArrow = (0, async () => {});
    assertArrayEqual(Object.getOwnPropertyNames(anonAsyncArrow), ["length", "name"], "anon async arrow keys");
    assertDataProp(anonAsyncArrow, "length", 0, false, false, true, "anon async arrow");
    assertDataProp(anonAsyncArrow, "name", "", false, false, true, "anon async arrow");

    const anonAsyncGen = (0, async function* () {});
    assertArrayEqual(Object.getOwnPropertyNames(anonAsyncGen), ["prototype", "length", "name"], "anon async generator keys");
    assertDataProp(anonAsyncGen, "length", 0, false, false, true, "anon async generator");
    assertDataProp(anonAsyncGen, "name", "", false, false, true, "anon async generator");

    const anonClass = (0, class {});
    assertArrayEqual(Object.getOwnPropertyNames(anonClass), ["length", "name", "prototype"], "anon class keys");
    assertDataProp(anonClass, "length", 0, false, false, true, "anon class");
    assertDataProp(anonClass, "name", "", false, false, true, "anon class");

    // Destructuring declaration NamedEvaluation
    const {
        fn = function () {},
        arr = () => {},
        gen = function* () {},
        afn = async function () {},
        agen = async function* () {}
    } = {};
    for (const [expectedName, f] of [["fn", fn], ["arr", arr], ["gen", gen], ["afn", afn], ["agen", agen]]) {
        assertLengthThenNameOrder(f, "destr " + expectedName);
        assertDataProp(f, "name", expectedName, false, false, true, "destr " + expectedName);
    }

    // Computed names evaluated multiple times in a loop (undeferred on 2nd+ iteration)
    const s1 = Symbol("s1");
    const s2 = Symbol("s2");
    const s3 = Symbol("s3");
    const s4 = Symbol("s4");
    const s5 = Symbol("s5");
    const s6 = Symbol("s6");
    const s7 = Symbol("s7");
    for (let i = 0; i < 3; i++) {
        const key = "dyn_" + i;
        const obj = {
            [s1]() {},
            [key]() {},
            [s2]: function () {},
            [s3]: () => {},
            *[s4]() {},
            async [s5]() {},
            async *[s6]() {},
            [s7]: async function () {}
        };
        assertDataProp(obj[s1], "name", "[s1]", false, false, true, "loop method sym " + i);
        assertDataProp(obj[key], "name", key, false, false, true, "loop method str " + i);
        assertDataProp(obj[s2], "name", "[s2]", false, false, true, "loop fn sym " + i);
        assertDataProp(obj[s3], "name", "[s3]", false, false, true, "loop arrow sym " + i);
        assertDataProp(obj[s4], "name", "[s4]", false, false, true, "loop gen sym " + i);
        assertDataProp(obj[s5], "name", "[s5]", false, false, true, "loop async sym " + i);
        assertDataProp(obj[s6], "name", "[s6]", false, false, true, "loop async gen sym " + i);
        assertDataProp(obj[s7], "name", "[s7]", false, false, true, "loop async fn sym " + i);
        assertLengthThenNameOrder(obj[s1], "loop method sym " + i);
        assertLengthThenNameOrder(obj[s2], "loop fn sym " + i);
        assertLengthThenNameOrder(obj[s5], "loop async sym " + i);
    }
})();

print("pass");
