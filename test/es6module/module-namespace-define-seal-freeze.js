//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(msg + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertThrowsType(fn, ErrCtor, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = (e instanceof ErrCtor);
        if (!threw) {
            throw new Error(msg + ": expected " + ErrCtor.name + ", got " + e);
        }
    }
    if (!threw) {
        throw new Error(msg + ": expected " + ErrCtor.name + ", did not throw");
    }
}

WScript.RegisterModuleSource("rv09_dep.js", `
    export let reexp = 7;
`);

WScript.RegisterModuleSource("rv09_live.js", `
    export let live = 1;
    export const fixed = 2;
    export let undef;
    export let nanVal = NaN;
    export let posZero = +0;
    export { reexp } from "rv09_dep.js";
    export function bump() { live = 42; }
`);

WScript.RegisterModuleSource("rv09_empty.js", `
    const hidden = 1;
`);

var p1 = Promise.all([
    import("rv09_live.js"),
    import("rv09_empty.js")
]).then(function (mods) {
    var ns = mods[0];
    var emptyNs = mods[1];

    // 1. [[DefineOwnProperty]] on existing exports.
    assertThrowsType(function () {
        Object.defineProperty(ns, "live", { value: 99 });
    }, TypeError, "defineProperty with different value throws TypeError");
    assertEq(ns.live, 1, "live export unchanged after rejected defineProperty");

    assertEq(Reflect.defineProperty(ns, "live", { value: 99 }), false, "Reflect.defineProperty diff value returns false");
    assertEq(Reflect.defineProperty(ns, "live", { value: 1 }), true, "Reflect.defineProperty same value returns true");
    assertEq(Object.defineProperty(ns, "live", { value: 1 }), ns, "Object.defineProperty same value returns ns");
    assertEq(Object.defineProperty(ns, "live", {}), ns, "empty descriptor succeeds");
    assertEq(Object.defineProperty(ns, "live", { writable: true, enumerable: true, configurable: false }), ns, "matching attributes succeed");

    assertThrowsType(function () {
        Object.defineProperty(ns, "live", { writable: false });
    }, TypeError, "writable:false throws TypeError");
    assertThrowsType(function () {
        Object.defineProperty(ns, "live", { configurable: true });
    }, TypeError, "configurable:true throws TypeError");
    assertThrowsType(function () {
        Object.defineProperty(ns, "live", { enumerable: false });
    }, TypeError, "enumerable:false throws TypeError");
    assertThrowsType(function () {
        Object.defineProperty(ns, "live", { get: function () { return 1; } });
    }, TypeError, "accessor descriptor throws TypeError");

    // Non-local re-export behaves identically.
    assertEq(Reflect.defineProperty(ns, "reexp", { value: 7 }), true, "re-export same value returns true");
    assertEq(Reflect.defineProperty(ns, "reexp", { value: 8 }), false, "re-export diff value returns false");

    // Live export mutation is reflected by [[DefineOwnProperty]].
    ns.bump();
    assertEq(ns.live, 42, "live export updated after bump()");
    assertEq(Reflect.defineProperty(ns, "live", { value: 1 }), false, "stale value rejected after live update");
    assertEq(Reflect.defineProperty(ns, "live", { value: 42 }), true, "updated live value accepted");

    // SameValue edge cases: undefined, NaN, +0 vs -0.
    assertEq(Reflect.defineProperty(ns, "undef", { value: undefined }), true, "SameValue(undefined, undefined)");
    assertEq(Reflect.defineProperty(ns, "undef", { value: 1 }), false, "SameValue(1, undefined)");
    assertEq(Reflect.defineProperty(ns, "nanVal", { value: NaN }), true, "SameValue(NaN, NaN)");
    assertEq(Reflect.defineProperty(ns, "posZero", { value: +0 }), true, "SameValue(+0, +0)");
    assertEq(Reflect.defineProperty(ns, "posZero", { value: -0 }), false, "SameValue(-0, +0) is false");

    // Non-exported string property and Symbols.
    assertThrowsType(function () {
        Object.defineProperty(ns, "missing", { value: 1 });
    }, TypeError, "new string property throws TypeError");
    assertEq(Reflect.defineProperty(ns, Symbol.toStringTag, { value: "Module" }), true, "Symbol.toStringTag same value succeeds");
    assertEq(Reflect.defineProperty(ns, Symbol.toStringTag, { value: "Other" }), false, "Symbol.toStringTag diff value fails");
    assertEq(Reflect.defineProperty(ns, Symbol("s"), { value: 1 }), false, "new symbol fails");

    // 2. Seal / Freeze / IsSealed / IsFrozen on non-empty namespace.
    assertEq(Object.isSealed(ns), true, "non-empty namespace isSealed === true");
    assertEq(Object.isFrozen(ns), false, "non-empty namespace isFrozen === false");
    assertEq(Object.seal(ns), ns, "Object.seal(ns) succeeds and returns ns");
    assertThrowsType(function () {
        Object.freeze(ns);
    }, TypeError, "Object.freeze(ns) throws TypeError when mutable exports exist");

    // 3. Seal / Freeze / IsSealed / IsFrozen on 0-export namespace.
    assertEq(Object.isSealed(emptyNs), true, "empty namespace isSealed === true");
    assertEq(Object.isFrozen(emptyNs), true, "empty namespace isFrozen === true");
    assertEq(Object.seal(emptyNs), emptyNs, "Object.seal(emptyNs) returns emptyNs");
    assertEq(Object.freeze(emptyNs), emptyNs, "Object.freeze(emptyNs) returns emptyNs");

    // Prototype immutability sanity.
    assertEq(Object.setPrototypeOf(ns, null), ns, "setPrototypeOf(ns, null) succeeds");
    assertThrowsType(function () {
        Object.setPrototypeOf(ns, {});
    }, TypeError, "setPrototypeOf(ns, {}) throws TypeError");
});

// 4. TDZ ReferenceError checks on seal/freeze/isSealed/isFrozen/defineProperty.
WScript.RegisterModuleSource("rv09_tdz_entry.js", `
    import * as mid from "rv09_tdz_mid.js";
    export let uninit = 123;
`);
WScript.RegisterModuleSource("rv09_tdz_mid.js", `
    import * as topNs from "rv09_tdz_entry.js";
    export const results = [];
    for (const op of [
        function () { return Object.isSealed(topNs); },
        function () { return Object.isFrozen(topNs); },
        function () { return Object.seal(topNs); },
        function () { return Object.freeze(topNs); },
        function () { return Object.defineProperty(topNs, "uninit", { value: 123 }); }
    ]) {
        let errName = "none";
        try { op(); } catch (e) { errName = e.name; }
        results.push(errName);
    }
`);

var p2 = import("rv09_tdz_entry.js").then(function () {
    return import("rv09_tdz_mid.js");
}).then(function (midNs) {
    assertEq(midNs.results.length, 5, "TDZ results count");
    for (var i = 0; i < midNs.results.length; i++) {
        assertEq(midNs.results[i], "ReferenceError", "TDZ check #" + i);
    }
});

Promise.all([p1, p2]).then(function () {
    print("pass");
}).catch(function (e) {
    print("fail: " + (e && e.stack ? e.stack : e));
    throw e;
});
