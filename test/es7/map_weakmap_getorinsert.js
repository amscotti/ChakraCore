//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

"use strict";

function fail(msg) {
    print("FAIL: " + msg);
    if (typeof WScript !== "undefined" && typeof WScript.Quit === "function") {
        WScript.Quit(1);
    }
    throw new Error(msg);
}

function assert(cond, msg) {
    if (!cond) {
        fail(msg);
    }
}

function assertSameValue(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        fail(msg + " (expected " + String(expected) + ", got " + String(actual) + ")");
    }
}

function assertThrowsTypeError(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        if (e instanceof TypeError) {
            threw = true;
        } else {
            fail(msg + " (threw non-TypeError: " + e + ")");
        }
    }
    assert(threw, msg + " (did not throw TypeError)");
}

function verifyBuiltInMethod(proto, name, expectedLength) {
    var desc = Object.getOwnPropertyDescriptor(proto, name);
    assert(desc !== undefined, name + " descriptor missing");
    assert(typeof desc.value === "function", name + " is a function");
    assertSameValue(desc.writable, true, name + " writable");
    assertSameValue(desc.enumerable, false, name + " enumerable");
    assertSameValue(desc.configurable, true, name + " configurable");

    var fn = desc.value;
    var nameDesc = Object.getOwnPropertyDescriptor(fn, "name");
    assertSameValue(nameDesc.value, name, name + ".name");
    assertSameValue(nameDesc.writable, false, name + ".name writable");
    assertSameValue(nameDesc.enumerable, false, name + ".name enumerable");
    assertSameValue(nameDesc.configurable, true, name + ".name configurable");

    var lenDesc = Object.getOwnPropertyDescriptor(fn, "length");
    assertSameValue(lenDesc.value, expectedLength, name + ".length");
    assertSameValue(lenDesc.writable, false, name + ".length writable");
    assertSameValue(lenDesc.enumerable, false, name + ".length enumerable");
    assertSameValue(lenDesc.configurable, true, name + ".length configurable");

    assert(!Object.prototype.hasOwnProperty.call(fn, "prototype"), name + " has no prototype property");
    assertThrowsTypeError(function () { new fn(); }, name + " is not a constructor");
}

// 1. Property descriptors, length, name, non-constructor
verifyBuiltInMethod(Map.prototype, "getOrInsert", 2);
verifyBuiltInMethod(Map.prototype, "getOrInsertComputed", 2);
verifyBuiltInMethod(WeakMap.prototype, "getOrInsert", 2);
verifyBuiltInMethod(WeakMap.prototype, "getOrInsertComputed", 2);

// 2. Brand checks (non-object and wrong internal slot)
(function testBrandChecks() {
    var nonObjects = [undefined, null, false, true, 0, 1, "str", Symbol("s")];
    var wrongObjects = [
        {},
        [],
        new Set(),
        Map.prototype,
        WeakMap.prototype,
        new Proxy(new Map(), {}),
        new Proxy(new WeakMap(), {})
    ];
    var cb = function () { return 42; };

    for (var i = 0; i < nonObjects.length; i++) {
        var r = nonObjects[i];
        assertThrowsTypeError(function () { Map.prototype.getOrInsert.call(r, 1, 2); }, "Map.getOrInsert non-object " + i);
        assertThrowsTypeError(function () { Map.prototype.getOrInsertComputed.call(r, 1, cb); }, "Map.getOrInsertComputed non-object " + i);
        assertThrowsTypeError(function () { WeakMap.prototype.getOrInsert.call(r, {}, 2); }, "WeakMap.getOrInsert non-object " + i);
        assertThrowsTypeError(function () { WeakMap.prototype.getOrInsertComputed.call(r, {}, cb); }, "WeakMap.getOrInsertComputed non-object " + i);
    }

    for (var j = 0; j < wrongObjects.length; j++) {
        var o = wrongObjects[j];
        assertThrowsTypeError(function () { Map.prototype.getOrInsert.call(o, 1, 2); }, "Map.getOrInsert wrong-obj " + j);
        assertThrowsTypeError(function () { Map.prototype.getOrInsertComputed.call(o, 1, cb); }, "Map.getOrInsertComputed wrong-obj " + j);
        assertThrowsTypeError(function () { WeakMap.prototype.getOrInsert.call(o, {}, 2); }, "WeakMap.getOrInsert wrong-obj " + j);
        assertThrowsTypeError(function () { WeakMap.prototype.getOrInsertComputed.call(o, {}, cb); }, "WeakMap.getOrInsertComputed wrong-obj " + j);
    }

    assertThrowsTypeError(function () { Map.prototype.getOrInsert.call(new WeakMap(), 1, 2); }, "Map.getOrInsert on WeakMap");
    assertThrowsTypeError(function () { Map.prototype.getOrInsertComputed.call(new WeakMap(), 1, cb); }, "Map.getOrInsertComputed on WeakMap");
    assertThrowsTypeError(function () { WeakMap.prototype.getOrInsert.call(new Map(), {}, 2); }, "WeakMap.getOrInsert on Map");
    assertThrowsTypeError(function () { WeakMap.prototype.getOrInsertComputed.call(new Map(), {}, cb); }, "WeakMap.getOrInsertComputed on Map");
})();

// 3. Map.prototype.getOrInsert semantics & -0/+0 canonicalization & key types
(function testMapGetOrInsert() {
    var m = new Map();
    assertSameValue(m.getOrInsert("a", 1), 1, "Map.getOrInsert inserts absent");
    assertSameValue(m.get("a"), 1, "Map.getOrInsert stored value");
    assertSameValue(m.getOrInsert("a", 2), 1, "Map.getOrInsert returns existing without overwrite");
    assertSameValue(m.get("a"), 1, "Map.getOrInsert kept old value");

    // Existing undefined/falsy values must not be overwritten
    m.set("undef", undefined);
    m.set("zero", 0);
    m.set("nan", NaN);
    assertSameValue(m.getOrInsert("undef", 99), undefined, "existing undefined preserved");
    assert(m.has("undef") && m.get("undef") === undefined, "existing undefined still undefined");
    assertSameValue(m.getOrInsert("zero", 99), 0, "existing 0 preserved");
    assertSameValue(m.getOrInsert("nan", 99), NaN, "existing NaN preserved");

    // Default arguments
    var mDef = new Map();
    assertSameValue(mDef.getOrInsert(), undefined, "no args inserts undefined->undefined");
    assert(mDef.has(undefined) && mDef.get(undefined) === undefined, "no args key/value present");
    assertSameValue(mDef.getOrInsert("onlyKey"), undefined, "missing value inserts undefined");
    assert(mDef.has("onlyKey") && mDef.get("onlyKey") === undefined, "onlyKey present");

    // -0 / +0 canonicalization on SimpleVarMap
    var mZeroSimple = new Map();
    assertSameValue(mZeroSimple.getOrInsert(-0, "neg0"), "neg0", "-0 insert");
    assertSameValue(mZeroSimple.size, 1, "-0 size is 1");
    assertSameValue(Array.from(mZeroSimple.keys())[0], +0, "SimpleVarMap stored +0 key");
    assertSameValue(mZeroSimple.getOrInsert(0, "int0"), "neg0", "0 matches -0");
    assertSameValue(mZeroSimple.getOrInsert(+0, "pos0"), "neg0", "+0 matches -0");
    assertSameValue(mZeroSimple.getOrInsert(-0, "neg0-2"), "neg0", "-0 matches -0");
    assertSameValue(mZeroSimple.size, 1, "size stays 1");

    // -0 / +0 canonicalization on ComplexVarMap (string key forces ComplexVarMap)
    var mZeroComplex = new Map([["strKey", 10]]);
    assertSameValue(mZeroComplex.getOrInsert(-0, "complexNeg0"), "complexNeg0", "complex -0 insert");
    assertSameValue(Array.from(mZeroComplex.keys())[1], +0, "ComplexVarMap stored +0 key");
    assertSameValue(mZeroComplex.getOrInsert(0, "other"), "complexNeg0", "complex 0 lookup");
    assertSameValue(mZeroComplex.getOrInsert(-0, "other2"), "complexNeg0", "complex -0 lookup");

    // Diverse key types
    var mTypes = new Map();
    var objKey = { id: 1 };
    var arrKey = [1, 2];
    var fnKey = function () {};
    var symKey = Symbol("desc");
    var regSym = Symbol.for("map-reg-sym");
    var proxyKey = new Proxy({}, {});
    var revocable = Proxy.revocable({}, {});
    revocable.revoke();

    var pairs = [
        [undefined, "u"],
        [null, "n"],
        [true, "bt"],
        [false, "bf"],
        [42, "num42"],
        [1.5, "num1.5"],
        [NaN, "nan"],
        [Infinity, "inf"],
        [-Infinity, "ninf"],
        ["", "empty"],
        ["ab", "str_ab"],
        [1n, "bi1"],
        [-1n, "bi_neg1"],
        [1n << 100n, "bi_big"],
        [symKey, "sym"],
        [regSym, "regsym"],
        [Symbol.iterator, "wk_sym"],
        [objKey, "obj"],
        [arrKey, "arr"],
        [fnKey, "fn"],
        [proxyKey, "proxy"],
        [revocable.proxy, "revoked"]
    ];
    for (var k = 0; k < pairs.length; k++) {
        assertSameValue(mTypes.getOrInsert(pairs[k][0], pairs[k][1]), pairs[k][1], "insert key " + k);
    }
    assertSameValue(mTypes.getOrInsert(0 / 0, "otherNan"), "nan", "0/0 matches NaN");
    assertSameValue(mTypes.getOrInsert(-NaN, "otherNegNan"), "nan", "-NaN matches NaN");
    assertSameValue(mTypes.getOrInsert("a" + "b", "otherStr"), "str_ab", "concat string matches");
    assertSameValue(mTypes.getOrInsert(BigInt(1), "otherBigInt"), "bi1", "BigInt(1) matches 1n");
    assertSameValue(mTypes.getOrInsert(Symbol.for("map-reg-sym"), "otherReg"), "regsym", "Symbol.for matches");
    assert(mTypes.getOrInsert(Symbol("desc"), "distinctSym") === "distinctSym", "distinct Symbol inserts");
    assertSameValue(mTypes.getOrInsert(revocable.proxy, "otherRev"), "revoked", "revoked Proxy matches");
})();

// 4. Map.prototype.getOrInsertComputed
(function testMapGetOrInsertComputed() {
    var m = new Map([["present", "orig"], ["presentUndef", undefined]]);

    // Non-callable callbackfn throws even when key is present or omitted
    var nonCallables = [undefined, null, 1, "", true, {}, [], Symbol("nc")];
    for (var i = 0; i < nonCallables.length; i++) {
        var nc = nonCallables[i];
        assertThrowsTypeError(function () { m.getOrInsertComputed("absent", nc); }, "non-callable absent " + i);
        assertThrowsTypeError(function () { m.getOrInsertComputed("present", nc); }, "non-callable present " + i);
    }
    assertThrowsTypeError(function () { m.getOrInsertComputed("present"); }, "missing callbackfn present");
    assertThrowsTypeError(function () { m.getOrInsertComputed(); }, "missing all args");
    assert(!m.has("absent"), "absent not inserted when callbackfn invalid");

    // Different callable types
    var bound = (function (k) { return "bound:" + k; }).bind(m);
    var fnProxy = new Proxy(function (k) { return "proxy:" + k; }, {});
    function namedFn(k) { return "named:" + k; }
    assertSameValue(m.getOrInsertComputed("k1", namedFn), "named:k1", "function declaration");
    assertSameValue(m.getOrInsertComputed("k2", (k) => "arrow:" + k), "arrow:k2", "arrow function");
    assertSameValue(m.getOrInsertComputed("k3", new Function("k", "return 'newFn:' + k;")), "newFn:k3", "Function ctor");
    assertSameValue(m.getOrInsertComputed("k4", bound), "bound:k4", "bound function");
    assertSameValue(m.getOrInsertComputed("k5", fnProxy), "proxy:k5", "callable Proxy");

    // Does not evaluate callbackfn when key is present
    var evalCount = 0;
    assertSameValue(m.getOrInsertComputed("present", function () { evalCount++; return "bad"; }), "orig", "present skips cb");
    assertSameValue(m.getOrInsertComputed("presentUndef", function () { evalCount++; return "bad"; }), undefined, "present undefined skips cb");
    assertSameValue(evalCount, 0, "callback not called for present keys");

    // Callback receives this === undefined and 1 argument (canonicalized key)
    var callCheck = new Map();
    callCheck.getOrInsertComputed(77, function () {
        assertSameValue(this, undefined, "callback this is undefined");
        assertSameValue(arguments.length, 1, "callback args.length is 1");
        assertSameValue(arguments[0], 77, "callback arg[0] is key");
        return "ok";
    });

    // -0 -> +0 canonicalization passed to callbackfn in both SimpleVarMap and ComplexVarMap
    var zSimple = new Map();
    var seenKeySimple;
    zSimple.getOrInsertComputed(-0, function (k) {
        seenKeySimple = k;
        return "zs";
    });
    assertSameValue(seenKeySimple, +0, "SimpleVarMap callback received +0");
    assertSameValue(Array.from(zSimple.keys())[0], +0, "SimpleVarMap stored +0");

    var zComplex = new Map([["str", 1]]);
    var seenKeyComplex;
    zComplex.getOrInsertComputed(-0, function (k) {
        seenKeyComplex = k;
        return "zc";
    });
    assertSameValue(seenKeyComplex, +0, "ComplexVarMap callback received +0");
    assertSameValue(Array.from(zComplex.keys())[1], +0, "ComplexVarMap stored +0");

    // Callback throwing leaves Map unchanged (or without inserting target key)
    var mThrow = new Map([["keep", 1]]);
    var sentinelErr = { tag: "boom" };
    var caught = null;
    try {
        mThrow.getOrInsertComputed("target", function () { throw sentinelErr; });
    } catch (e) {
        caught = e;
    }
    assertSameValue(caught, sentinelErr, "Map callback throw propagated");
    assert(!mThrow.has("target") && mThrow.size === 1 && mThrow.get("keep") === 1, "Map unchanged after throw");

    // Reentrancy: overwrite inner set, preserve insertion order, delete/clear, promotion + GC
    var mReent = new Map([["before", 0]]);
    var res1 = mReent.getOrInsertComputed("target", function (k) {
        mReent.set(k, "inner");
        mReent.set("after", 2);
        return "outer";
    });
    assertSameValue(res1, "outer", "overwrite inner set return value");
    assertSameValue(mReent.get("target"), "outer", "overwrite inner set stored value");
    assertSameValue(Array.from(mReent.keys()).join(","), "before,target,after", "insertion order preserved on overwrite");

    var mReentUndef = new Map();
    var resUndef = mReentUndef.getOrInsertComputed("k", function (k) {
        mReentUndef.set(k, 123);
    });
    assertSameValue(resUndef, undefined, "callback returning undefined overwrites inner set");
    assert(mReentUndef.has("k") && mReentUndef.get("k") === undefined, "stored value overwritten to undefined");

    var mReentDel = new Map();
    var resDel = mReentDel.getOrInsertComputed("k", function (k) {
        mReentDel.set(k, "temp");
        mReentDel.delete(k);
        return "afterDelete";
    });
    assertSameValue(resDel, "afterDelete", "set then delete during callback");
    assertSameValue(mReentDel.get("k"), "afterDelete", "stored after delete in callback");

    var mReentClear = new Map([["a", 1], ["b", 2]]);
    var resClear = mReentClear.getOrInsertComputed(42, function () {
        mReentClear.clear();
        return "afterClear";
    });
    assertSameValue(resClear, "afterClear", "clear during callback");
    assertSameValue(mReentClear.size, 1, "size 1 after clear in callback");
    assertSameValue(mReentClear.get(42), "afterClear", "key stored after clear in callback");

    var mPromo = new Map();
    var resPromo = mPromo.getOrInsertComputed(10, function () {
        for (var n = 0; n < 300; n++) {
            mPromo.set("str_" + n, n);
        }
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
        return "postPromoGC";
    });
    assertSameValue(resPromo, "postPromoGC", "promotion + GC during callback");
    assertSameValue(mPromo.get(10), "postPromoGC", "key present after promotion + GC");
    assertSameValue(mPromo.size, 301, "size after promotion + GC");
})();

// 5. WeakMap.prototype.getOrInsert
(function testWeakMapGetOrInsert() {
    var wm = new WeakMap();
    var obj = { a: 1 };
    var arr = [1, 2];
    var fn = function () {};
    var proxy = new Proxy({}, {});
    var revocable = Proxy.revocable({}, {});
    revocable.revoke();
    var sym = Symbol("wm-sym");
    var regSym = Symbol.for("wm-reg-sym");
    var wrappedRegSym = Object(regSym);

    assertSameValue(wm.getOrInsert(obj, "v1"), "v1", "WeakMap insert object");
    assertSameValue(wm.getOrInsert(obj, "v2"), "v1", "WeakMap existing object");
    assertSameValue(wm.getOrInsert(arr, undefined), undefined, "WeakMap insert explicit undefined");
    assertSameValue(wm.getOrInsert(arr, "v3"), undefined, "WeakMap existing undefined preserved");
    assertSameValue(wm.getOrInsert(fn), undefined, "WeakMap omitted value inserts undefined");
    assert(wm.has(fn) && wm.get(fn) === undefined, "WeakMap omitted value stored");
    assertSameValue(wm.getOrInsert(proxy, "p"), "p", "WeakMap Proxy key");
    assertSameValue(wm.getOrInsert(revocable.proxy, "rp"), "rp", "WeakMap revoked Proxy key");
    assertSameValue(wm.getOrInsert(sym, "s1"), "s1", "WeakMap unregistered Symbol key");
    assertSameValue(wm.getOrInsert(sym, "s2"), "s1", "WeakMap existing Symbol key");
    assertSameValue(wm.getOrInsert(Symbol.iterator, "wk"), "wk", "WeakMap well-known Symbol key");
    assertSameValue(wm.getOrInsert(wrappedRegSym, "ws"), "ws", "WeakMap Object(Symbol.for) key");

    // Invalid keys throw TypeError
    var invalidKeys = [undefined, null, false, true, 0, -0, 1, NaN, "str", 1n, regSym];
    for (var i = 0; i < invalidKeys.length; i++) {
        var k = invalidKeys[i];
        assertThrowsTypeError(function () { wm.getOrInsert(k, 1); }, "WeakMap.getOrInsert invalid key " + i);
    }
    assertThrowsTypeError(function () { wm.getOrInsert(); }, "WeakMap.getOrInsert no args");
})();

// 6. WeakMap.prototype.getOrInsertComputed
(function testWeakMapGetOrInsertComputed() {
    var wm = new WeakMap();
    var keyObj = { k: 1 };
    var keySym = Symbol("wm-computed-sym");
    wm.set(keyObj, "existingObj");
    wm.set(keySym, undefined);

    // IsCallable(callbackfn) checked before key lookup AND before CanBeHeldWeakly(key)
    var nonCallables = [undefined, null, 1, "", true, {}, [], Symbol("nc")];
    for (var i = 0; i < nonCallables.length; i++) {
        var nc = nonCallables[i];
        assertThrowsTypeError(function () { wm.getOrInsertComputed(keyObj, nc); }, "WeakMap non-callable present obj " + i);
        assertThrowsTypeError(function () { wm.getOrInsertComputed(keySym, nc); }, "WeakMap non-callable present sym " + i);
        assertThrowsTypeError(function () { wm.getOrInsertComputed({}, nc); }, "WeakMap non-callable absent obj " + i);
        assertThrowsTypeError(function () { wm.getOrInsertComputed(123, nc); }, "WeakMap non-callable invalid key " + i);
        assertThrowsTypeError(function () { wm.getOrInsertComputed(Symbol.for("r"), nc); }, "WeakMap non-callable reg sym " + i);
    }
    assertThrowsTypeError(function () { wm.getOrInsertComputed(keyObj); }, "WeakMap missing callbackfn");
    assertThrowsTypeError(function () { wm.getOrInsertComputed(); }, "WeakMap missing all args");

    // Invalid keys throw TypeError WITHOUT calling callbackfn
    var invalidKeys = [undefined, null, false, true, 0, -0, 1, NaN, "str", 1n, Symbol.for("wm-reg")];
    var cbCalls = 0;
    for (var j = 0; j < invalidKeys.length; j++) {
        var ik = invalidKeys[j];
        assertThrowsTypeError(function () {
            wm.getOrInsertComputed(ik, function () { cbCalls++; return 1; });
        }, "WeakMap invalid key throws before callback " + j);
    }
    assertSameValue(cbCalls, 0, "callback not called on invalid WeakMap keys");

    // Present keys skip callbackfn
    assertSameValue(wm.getOrInsertComputed(keyObj, function () { cbCalls++; return "x"; }), "existingObj", "present obj skips cb");
    assertSameValue(wm.getOrInsertComputed(keySym, function () { cbCalls++; return "x"; }), undefined, "present sym undefined skips cb");
    assertSameValue(cbCalls, 0, "callback not called on present WeakMap keys");

    // Callback args: this === undefined, 1 arg === key (for both object and symbol keys)
    var newObj = {};
    var newSym = Symbol("fresh-sym");
    assertSameValue(wm.getOrInsertComputed(newObj, function () {
        assertSameValue(this, undefined, "WeakMap obj cb this");
        assertSameValue(arguments.length, 1, "WeakMap obj cb args.length");
        assertSameValue(arguments[0], newObj, "WeakMap obj cb arg[0]");
        return "computedObj";
    }), "computedObj", "WeakMap computed obj insert");
    assertSameValue(wm.get(newObj), "computedObj", "WeakMap computed obj stored");

    assertSameValue(wm.getOrInsertComputed(newSym, function () {
        assertSameValue(this, undefined, "WeakMap sym cb this");
        assertSameValue(arguments.length, 1, "WeakMap sym cb args.length");
        assertSameValue(arguments[0], newSym, "WeakMap sym cb arg[0]");
        return "computedSym";
    }), "computedSym", "WeakMap computed sym insert");
    assertSameValue(wm.get(newSym), "computedSym", "WeakMap computed sym stored");

    // Callback throws: key not inserted
    var err = { code: "wm-err" };
    var throwObj = {};
    var throwSym = Symbol("throw-sym");
    for (var t = 0; t < 2; t++) {
        var tk = t === 0 ? throwObj : throwSym;
        var caught = null;
        try {
            wm.getOrInsertComputed(tk, function () { throw err; });
        } catch (e) {
            caught = e;
        }
        assertSameValue(caught, err, "WeakMap throw propagated " + t);
        assert(!wm.has(tk), "WeakMap key not inserted after throw " + t);
    }

    // Reentrancy: callback mutates WeakMap (set, set->undefined, set+delete, mass insert + GC)
    var rObj1 = {};
    var rObj2 = {};
    var rObj3 = {};
    var rSym1 = Symbol("rs1");
    var rSym2 = Symbol("rs2");
    var rSym3 = Symbol("rs3");

    assertSameValue(wm.getOrInsertComputed(rObj1, function (k) { wm.set(k, "inner"); return "outer"; }), "outer", "wm obj overwrite");
    assertSameValue(wm.get(rObj1), "outer", "wm obj overwritten");
    assertSameValue(wm.getOrInsertComputed(rObj2, function (k) { wm.set(k, "inner"); }), undefined, "wm obj overwrite undef");
    assert(wm.has(rObj2) && wm.get(rObj2) === undefined, "wm obj overwritten to undef");
    assertSameValue(wm.getOrInsertComputed(rObj3, function (k) {
        wm.set(k, "inner");
        wm.delete(k);
        return "readded";
    }), "readded", "wm obj delete inside cb");
    assertSameValue(wm.get(rObj3), "readded", "wm obj stored after delete in cb");

    assertSameValue(wm.getOrInsertComputed(rSym1, function (k) { wm.set(k, "inner"); return "outer"; }), "outer", "wm sym overwrite");
    assertSameValue(wm.get(rSym1), "outer", "wm sym overwritten");
    assertSameValue(wm.getOrInsertComputed(rSym2, function (k) { wm.set(k, "inner"); }), undefined, "wm sym overwrite undef");
    assert(wm.has(rSym2) && wm.get(rSym2) === undefined, "wm sym overwritten to undef");
    assertSameValue(wm.getOrInsertComputed(rSym3, function (k) {
        wm.set(k, "inner");
        wm.delete(k);
        return "readdedSym";
    }), "readdedSym", "wm sym delete inside cb");
    assertSameValue(wm.get(rSym3), "readdedSym", "wm sym stored after delete in cb");

    var stressObj = {};
    var stressSym = Symbol("stress");
    assertSameValue(wm.getOrInsertComputed(stressObj, function () {
        for (var n = 0; n < 300; n++) {
            wm.set({}, n);
            wm.set(Symbol("cb-" + n), n);
        }
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
        return "stressObjVal";
    }), "stressObjVal", "wm obj stress + GC inside cb");
    assertSameValue(wm.get(stressObj), "stressObjVal", "wm obj survived stress + GC");

    assertSameValue(wm.getOrInsertComputed(stressSym, function () {
        for (var n = 0; n < 300; n++) {
            wm.set({}, n);
            wm.set(Symbol("cbs-" + n), n);
        }
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
        return "stressSymVal";
    }), "stressSymVal", "wm sym stress + GC inside cb");
    assertSameValue(wm.get(stressSym), "stressSymVal", "wm sym survived stress + GC");
})();

// 7. GC tests with CollectGarbage()
(function testGarbageCollection() {
    if (typeof CollectGarbage !== "function") {
        return;
    }

    var wm = new WeakMap();
    var liveObj1 = { tag: "live1" };
    var liveObj2 = { tag: "live2" };
    var liveSym1 = Symbol("liveSym1");
    var liveSym2 = Symbol("liveSym2");
    var strongVal1 = { payload: "strong1" };
    var strongVal2 = { payload: "strong2" };

    wm.getOrInsert(liveObj1, strongVal1);
    wm.getOrInsertComputed(liveObj2, function () { return strongVal2; });
    wm.getOrInsert(liveSym1, { payload: "symStrong1" });
    wm.getOrInsertComputed(liveSym2, function () { return { payload: "symStrong2" }; });
    strongVal1 = null;
    strongVal2 = null;

    // Ephemeral object keys (should be collectible without corrupting WeakMap)
    for (var i = 0; i < 500; i++) {
        wm.getOrInsert({ tmp: i }, { val: i });
        wm.getOrInsertComputed({ tmpC: i }, function (k) { return { val: k.tmpC }; });
    }

    // Ephemeral key passed directly in argument position while callback triggers GC
    var heldRef = null;
    var ephemeralVal = wm.getOrInsertComputed({ tag: "argHeld" }, function (k) {
        CollectGarbage();
        heldRef = k;
        return { kept: k.tag };
    });
    assertSameValue(ephemeralVal.kept, "argHeld", "ephemeral arg key kept alive across GC in cb");
    assertSameValue(wm.get(heldRef).kept, "argHeld", "ephemeral arg key inserted after GC in cb");
    heldRef = null;

    CollectGarbage();
    CollectGarbage();

    assertSameValue(wm.get(liveObj1).payload, "strong1", "liveObj1 value survived GC");
    assertSameValue(wm.get(liveObj2).payload, "strong2", "liveObj2 value survived GC");
    assertSameValue(wm.get(liveSym1).payload, "symStrong1", "liveSym1 strong value survived GC");
    assertSameValue(wm.get(liveSym2).payload, "symStrong2", "liveSym2 strong value survived GC");
})();

// 8. JIT-warmed loops for BEGIN_SAFE_REENTRANT_CALL coverage
(function testJitWarmedLoops() {
    var ITERATIONS = 1200;
    var m = new Map();
    var wm = new WeakMap();
    var objs = [];
    for (var i = 0; i < ITERATIONS; i++) {
        objs.push({ i: i });
    }

    var mapHits = 0;
    var wmHits = 0;
    for (var j = 0; j < ITERATIONS; j++) {
        var mv = m.getOrInsertComputed(j, function (k) {
            if ((k & 3) === 0) {
                m.set(k, -1);
            }
            return k * 2;
        });
        assertSameValue(mv, j * 2, "warmed Map insert " + j);
        mapHits += m.getOrInsertComputed(j, function () { fail("should not run on hit"); });

        var wv = wm.getOrInsertComputed(objs[j], function (o) {
            if ((o.i & 3) === 0) {
                wm.set(o, -1);
            }
            return o.i + 10;
        });
        assertSameValue(wv, j + 10, "warmed WeakMap insert " + j);
        wmHits += wm.getOrInsertComputed(objs[j], function () { fail("should not run on wm hit"); });
    }
    assert(mapHits > 0 && wmHits > 0, "warmed hits accumulated");

    var throwCount = 0;
    var sentinel = new Error("warm-throw");
    for (var t = 0; t < ITERATIONS; t++) {
        try {
            m.getOrInsertComputed("throw_" + t, function () { throw sentinel; });
        } catch (e) {
            if (e === sentinel) {
                throwCount++;
            }
        }
        try {
            wm.getOrInsertComputed({}, function () { throw sentinel; });
        } catch (e) {
            if (e === sentinel) {
                throwCount++;
            }
        }
    }
    assertSameValue(throwCount, ITERATIONS * 2, "warmed throws propagated cleanly");
})();

// 9. Cross-realm marshaling via WScript.LoadScript(..., "samethread")
(function testCrossRealm() {
    if (typeof WScript === "undefined" || typeof WScript.LoadScript !== "function") {
        return;
    }

    var other = WScript.LoadScript(
        "var realmObjExisting = { origin: 'other-existing' };" +
        "var realmObjInserted = { origin: 'other-inserted' };" +
        "var realmKey = { keyOrigin: 'other' };" +
        "var realmMap = new Map([['existing', realmObjExisting]]);" +
        "var realmWeakMap = new WeakMap([[realmKey, realmObjExisting]]);" +
        "function makeOtherValue(tag) { return { origin: tag }; }",
        "samethread"
    );

    // Map cross-realm getOrInsert
    var gotExisting = Map.prototype.getOrInsert.call(other.realmMap, "existing", { origin: "local" });
    assertSameValue(gotExisting.origin, "other-existing", "Map cross-realm existing");
    assertSameValue(gotExisting, other.realmObjExisting, "Map cross-realm existing identity");

    var gotInserted = Map.prototype.getOrInsert.call(other.realmMap, "newKey", other.realmObjInserted);
    assertSameValue(gotInserted.origin, "other-inserted", "Map cross-realm inserted");

    // Map cross-realm getOrInsertComputed
    var localKey = { keyOrigin: "local" };
    var gotComp = Map.prototype.getOrInsertComputed.call(other.realmMap, localKey, function (k) {
        assertSameValue(k, localKey, "Map cross-realm callback key");
        return other.makeOtherValue("computed-other");
    });
    assertSameValue(gotComp.origin, "computed-other", "Map cross-realm computed return");
    assertSameValue(other.realmMap.get(localKey).origin, "computed-other", "Map cross-realm computed stored");

    // WeakMap cross-realm getOrInsert & getOrInsertComputed
    var wmExisting = WeakMap.prototype.getOrInsert.call(other.realmWeakMap, other.realmKey, { origin: "local" });
    assertSameValue(wmExisting.origin, "other-existing", "WeakMap cross-realm existing");
    assertSameValue(wmExisting, other.realmObjExisting, "WeakMap cross-realm existing identity");

    var localWmKey = { localWm: true };
    var wmInserted = WeakMap.prototype.getOrInsert.call(other.realmWeakMap, localWmKey, other.realmObjInserted);
    assertSameValue(wmInserted.origin, "other-inserted", "WeakMap cross-realm inserted");

    var localWmKey2 = { localWm2: true };
    var wmComp = WeakMap.prototype.getOrInsertComputed.call(other.realmWeakMap, localWmKey2, function (k) {
        assertSameValue(k, localWmKey2, "WeakMap cross-realm callback key");
        return other.makeOtherValue("wm-computed-other");
    });
    assertSameValue(wmComp.origin, "wm-computed-other", "WeakMap cross-realm computed return");
    assertSameValue(other.realmWeakMap.get(localWmKey2).origin, "wm-computed-other", "WeakMap cross-realm computed stored");
})();

print("PASS");
