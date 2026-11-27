//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Regression coverage for Phase 2 runtime fixes (GR-RT1, GR-RT2, GR-RT3,
// GR-RT6, GR-RT8, GR-RT9, GR-RT10, GR-RT14).

var failures = [];
function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("gr_runtime_regressions: FAIL: " + msg);
    }
}

function checkThrows(errCtor, fn, msg) {
    try {
        fn();
    } catch (e) {
        check(e instanceof errCtor, msg + " (got " + e + ", expected " + errCtor.name + ")");
        return;
    }
    check(false, msg + " (did not throw " + errCtor.name + ")");
}

// --- GR-RT1: JSON.stringify large array throws catchable RangeError --------
checkThrows(RangeError, function () {
    JSON.stringify(new Array(89478486));
}, "GR-RT1: JSON.stringify(new Array(89478486)) throws RangeError");

checkThrows(RangeError, function () {
    var hugeProxy = new Proxy([], {
        get: function (t, p) {
            if (p === "length") return 100000000;
            return t[p];
        }
    });
    JSON.stringify(hugeProxy);
}, "GR-RT1: JSON.stringify on proxy array with huge length throws RangeError");

check(JSON.stringify([1, undefined, 3]) === "[1,null,3]", "GR-RT1: normal JSON.stringify unaffected");

// --- GR-RT8: Array.prototype.sort huge array-like throws catchable RangeError
checkThrows(RangeError, function () {
    Array.prototype.sort.call({ length: 134217728 });
}, "GR-RT8: Array.prototype.sort default comparator 2^27 throws RangeError");

checkThrows(RangeError, function () {
    Array.prototype.sort.call({ length: 268435456 }, function (a, b) { return 0; });
}, "GR-RT8: Array.prototype.sort custom comparator 2^28 throws RangeError");

var smallLike = { 0: 3, 1: 1, 2: 2, length: 3 };
Array.prototype.sort.call(smallLike);
check(smallLike[0] === 1 && smallLike[1] === 2 && smallLike[2] === 3, "GR-RT8: normal sort unaffected");

// --- GR-RT2: Map/Set/WeakMap/WeakSet subclass constructor order ------------
(function testGRRT2() {
    var mapLog = [];
    class SubMap extends Map {
        set(k, v) {
            mapLog.push(k);
            return super.set(String(k), v * 10);
        }
    }
    var m = new SubMap([[1, 2], [3, 4]]);
    check(mapLog.join(",") === "1,3", "GR-RT2: SubMap.set called for each entry");
    check(m.get("1") === 20 && m.get("3") === 40 && !m.has(1), "GR-RT2: SubMap transformed keys/values");

    var setLog = [];
    class SubSet extends Set {
        add(v) {
            setLog.push(v);
            return super.add(v * 2);
        }
    }
    var s = new SubSet([1, 2, 3]);
    check(setLog.join(",") === "1,2,3", "GR-RT2: SubSet.add called for each entry");
    check(s.has(2) && s.has(4) && s.has(6) && !s.has(1), "GR-RT2: SubSet transformed entries");

    var wmLog = [];
    class SubWeakMap extends WeakMap {
        set(k, v) {
            wmLog.push(k.id);
            return super.set(k, v + 1);
        }
    }
    var k1 = { id: "a" };
    var k2 = { id: "b" };
    var wm = new SubWeakMap([[k1, 10], [k2, 20]]);
    check(wmLog.join(",") === "a,b", "GR-RT2: SubWeakMap.set called");
    check(wm.get(k1) === 11 && wm.get(k2) === 21, "GR-RT2: SubWeakMap transformed values");

    var wsLog = [];
    class SubWeakSet extends WeakSet {
        add(k) {
            wsLog.push(k.id);
            return super.add(k);
        }
    }
    var ws = new SubWeakSet([k1, k2]);
    check(wsLog.join(",") === "a,b", "GR-RT2: SubWeakSet.add called");
    check(ws.has(k1) && ws.has(k2), "GR-RT2: SubWeakSet populated");

    // Non-callable set/add must throw TypeError before GetIterator
    function checkNonCallableOrder(Ctor, methodName) {
        var order = [];
        class Bad extends Ctor {}
        Object.defineProperty(Bad.prototype, methodName, {
            get: function () {
                order.push("get-" + methodName);
                return 5;
            }
        });
        var iterable = {};
        iterable[Symbol.iterator] = function () {
            order.push("get-iterator");
            return [][Symbol.iterator]();
        };
        checkThrows(TypeError, function () {
            new Bad(iterable);
        }, "GR-RT2: non-callable " + Ctor.name + "." + methodName + " throws TypeError");
        check(order.join(",") === "get-" + methodName, "GR-RT2: " + Ctor.name + " checks " + methodName + " before GetIterator, got " + order.join(","));

        // Null/undefined iterable does not read set/add
        order.length = 0;
        var empty1 = new Bad();
        var empty2 = new Bad(null);
        var empty3 = new Bad(undefined);
        check(empty1 instanceof Bad && empty2 instanceof Bad && empty3 instanceof Bad, "GR-RT2: empty " + Ctor.name + " subclass succeeds");
        check(order.length === 0, "GR-RT2: empty " + Ctor.name + " does not read " + methodName);
    }
    checkNonCallableOrder(Map, "set");
    checkNonCallableOrder(Set, "add");
    checkNonCallableOrder(WeakMap, "set");
    checkNonCallableOrder(WeakSet, "add");

    // Reflect.construct with custom newTarget.prototype and full step order
    var rcOrder = [];
    var rcProto = {};
    Object.defineProperty(rcProto, "add", {
        get: function () {
            rcOrder.push("get-add");
            return function (v) { rcOrder.push("call-add:" + v); };
        }
    });
    var CustomTarget = (function () {}).bind(null);
    Object.defineProperty(CustomTarget, "prototype", {
        get: function () {
            rcOrder.push("get-proto");
            return rcProto;
        }
    });
    var rcIter = {};
    rcIter[Symbol.iterator] = function () {
        rcOrder.push("get-iter");
        return [42][Symbol.iterator]();
    };
    var rcInst = Reflect.construct(Set, [rcIter], CustomTarget);
    check(Object.getPrototypeOf(rcInst) === rcProto, "GR-RT2: Reflect.construct sets prototype");
    check(rcOrder.join(",") === "get-proto,get-add,get-iter,call-add:42", "GR-RT2: Reflect.construct ES2025 order, got " + rcOrder.join(","));

    // Null prototype chain
    class NullProtoSet extends Set {}
    Object.setPrototypeOf(NullProtoSet.prototype, null);
    checkThrows(TypeError, function () {
        new NullProtoSet([1]);
    }, "GR-RT2: null prototype chain throws TypeError when iterable provided");
    check(Object.getPrototypeOf(new NullProtoSet()) === NullProtoSet.prototype, "GR-RT2: null prototype chain empty succeeds");
})();

// --- GR-RT3: BVSparse::NodeFromIndex tail cursor on integer Set ------------
(function testGRRT3() {
    var n = 50000;
    var s = new Set();
    for (var i = 0; i < n; i++) {
        s.add(i);
    }
    var misses = 0;
    for (var j = 0; j < 20000; j++) {
        if (!s.has(n + 1 + j)) misses++;
    }
    check(misses === 20000, "GR-RT3: 20000 misses past tail all return false");
    check(s.has(0) === true && s.has(n - 1) === true && s.has(-1) === false, "GR-RT3: head/tail/negative checks");

    var sparse = new Set();
    for (var k = 0; k < 2000; k++) {
        sparse.add(k * 1000);
    }
    check(sparse.has(1999 * 1000) === true, "GR-RT3: sparse tail hit");
    check(sparse.has(2000 * 1000) === false, "GR-RT3: sparse miss past tail");
    check(sparse.has(2001 * 1000) === false, "GR-RT3: sparse second miss past tail");
    check(sparse.has(500) === false, "GR-RT3: sparse interior miss");
    check(sparse.has(500 * 1000) === true, "GR-RT3: sparse interior hit");

    // Empty integer set edge case
    var empty = new Set();
    check(empty.has(10) === false, "GR-RT3: empty set miss");
})();

// --- GR-RT6: TypedArray.prototype.fill fast path and -0.0 preservation -----
(function testGRRT6() {
    var f64 = new Float64Array(1024);
    f64.fill(-0.0);
    check(Object.is(f64[0], -0.0) && Object.is(f64[511], -0.0) && Object.is(f64[1023], -0.0),
        "GR-RT6: Float64Array.fill(-0.0) preserves -0.0");
    f64.fill(0.0);
    check(Object.is(f64[0], 0.0) && Object.is(f64[1023], 0.0),
        "GR-RT6: Float64Array.fill(+0.0) writes +0.0");

    var f32 = new Float32Array(1024);
    f32.fill(-0.0);
    check(Object.is(f32[0], -0.0) && Object.is(f32[1023], -0.0),
        "GR-RT6: Float32Array.fill(-0.0) preserves -0.0");

    if (typeof Float16Array === "function") {
        var f16 = new Float16Array(1024);
        f16.fill(-0.0);
        check(Object.is(f16[0], -0.0) && Object.is(f16[1023], -0.0),
            "GR-RT6: Float16Array.fill(-0.0) preserves -0.0");
        f16.fill(1.5, 10, 20);
        check(f16[9] === 0 && Object.is(f16[9], -0.0) && f16[10] === 1.5 && f16[19] === 1.5 && Object.is(f16[20], -0.0),
            "GR-RT6: Float16Array partial fill");
    }

    // JIT OP_Memset loop with -0.0
    function fillNegZeroLoop(arr) {
        for (var i = 0; i < arr.length; i++) {
            arr[i] = -0.0;
        }
    }
    var f64Loop = new Float64Array(1024);
    for (var rep = 0; rep < 20; rep++) {
        fillNegZeroLoop(f64Loop);
    }
    check(Object.is(f64Loop[0], -0.0) && Object.is(f64Loop[1023], -0.0),
        "GR-RT6: loop assignment of -0.0 preserves -0.0");

    // Partial ranges across typed array families
    var u8 = new Uint8Array(16);
    u8.fill(7, 4, 12);
    check(u8[3] === 0 && u8[4] === 7 && u8[11] === 7 && u8[12] === 0, "GR-RT6: Uint8Array partial fill");

    var u8c = new Uint8ClampedArray(8);
    u8c.fill(300, 1, 5);
    check(u8c[0] === 0 && u8c[1] === 255 && u8c[4] === 255 && u8c[5] === 0, "GR-RT6: Uint8ClampedArray clamp");

    var i32 = new Int32Array(16);
    i32.fill(-1, 2, 10);
    check(i32[1] === 0 && i32[2] === -1 && i32[9] === -1 && i32[10] === 0, "GR-RT6: Int32Array partial fill");

    if (typeof BigInt64Array === "function") {
        var b64 = new BigInt64Array(8);
        b64.fill(-5n, 2, 6);
        check(b64[1] === 0n && b64[2] === -5n && b64[5] === -5n && b64[6] === 0n, "GR-RT6: BigInt64Array fill");
        checkThrows(TypeError, function () {
            b64.fill();
        }, "GR-RT6: BigInt64Array.fill() without arg throws TypeError");
    }

    // Generic Array.prototype.fill with reentrant valueOf must still coerce per element
    var count = 0;
    var gen = new Int32Array(5);
    Array.prototype.fill.call(gen, {
        valueOf: function () { return ++count; }
    });
    check(count === 5 && gen[0] === 1 && gen[4] === 5,
        "GR-RT6: Array.prototype.fill.call runs valueOf per element");

    // Generic Array.prototype.fill with primitive takes fast path cleanly
    var gen2 = new Uint8Array(16);
    Array.prototype.fill.call(gen2, 9, 2, 14);
    check(gen2[1] === 0 && gen2[2] === 9 && gen2[13] === 9 && gen2[14] === 0,
        "GR-RT6: Array.prototype.fill.call primitive fill");
})();

// --- GR-RT9: TypedArray.prototype.set length and Atomics.isLockFree --------
(function testGRRT9() {
    checkThrows(RangeError, function () {
        new Uint8Array(8).set({ length: 4294967298, 0: 7, 1: 8, 2: 9 }, 3);
    }, "GR-RT9: set({length: 2^32 + 2}, 3) throws RangeError");

    checkThrows(RangeError, function () {
        new Uint8Array(8).set({ length: 4294967296 });
    }, "GR-RT9: set({length: 2^32}) throws RangeError");

    var ta = new Uint8Array([1, 2, 3, 4]);
    ta.set({ length: -1, 0: 99 });
    check(ta[0] === 1 && ta[3] === 4, "GR-RT9: set({length: -1}) is a no-op");

    ta.set({ length: -5, 0: 99 }, 4);
    check(ta[0] === 1 && ta[3] === 4, "GR-RT9: set({length: -5}, 4) is a no-op");

    checkThrows(RangeError, function () {
        ta.set({ length: -1 }, 5);
    }, "GR-RT9: set({length: -1}, 5) out of bounds offset still throws RangeError");

    if (typeof Atomics === "object") {
        check(Atomics.isLockFree(1) === true, "GR-RT9: isLockFree(1)");
        check(Atomics.isLockFree(2) === true, "GR-RT9: isLockFree(2)");
        check(Atomics.isLockFree(4) === true, "GR-RT9: isLockFree(4)");
        check(Atomics.isLockFree(4.9) === true, "GR-RT9: isLockFree(4.9) uses ToIntegerOrInfinity");
        check(Atomics.isLockFree(4294967300) === false, "GR-RT9: isLockFree(2^32 + 4) is false");
        check(Atomics.isLockFree(-4294967292) === false, "GR-RT9: isLockFree(-(2^32 - 4)) is false");
        check(Atomics.isLockFree(4294967297) === false, "GR-RT9: isLockFree(2^32 + 1) is false");
        check(Atomics.isLockFree(NaN) === false, "GR-RT9: isLockFree(NaN) is false");
        check(Atomics.isLockFree(Infinity) === false, "GR-RT9: isLockFree(Infinity) is false");
        check(Atomics.isLockFree(-Infinity) === false, "GR-RT9: isLockFree(-Infinity) is false");
    }
})();

// --- GR-RT10: TypedArray constructor and set read inherited properties -----
(function testGRRT10() {
    var fromProto = new Uint8Array({ __proto__: { 0: 5, 1: 6 }, length: 2 });
    check(fromProto[0] === 5 && fromProto[1] === 6, "GR-RT10: constructor reads inherited data properties");

    var fromGetter = new Uint8Array({ __proto__: { get 0() { return 5; } }, length: 2 });
    check(fromGetter[0] === 5 && fromGetter[1] === 0, "GR-RT10: constructor invokes inherited getter");

    var ownShadows = new Uint8Array({ __proto__: { 0: 5, 1: 6 }, 0: 9, length: 2 });
    check(ownShadows[0] === 9 && ownShadows[1] === 6, "GR-RT10: own property shadows prototype");

    checkThrows(ReferenceError, function () {
        new Uint8Array({ __proto__: { get 0() { throw new ReferenceError("boom"); } }, length: 1 });
    }, "GR-RT10: constructor propagates exception from inherited getter");

    var target = new Uint8Array(3);
    target.set({ __proto__: { 0: 11, get 1() { return 22; } }, length: 2 }, 1);
    check(target[0] === 0 && target[1] === 11 && target[2] === 22, "GR-RT10: set reads inherited data and getter properties");

    checkThrows(ReferenceError, function () {
        target.set({ __proto__: { get 0() { throw new ReferenceError("boom2"); } }, length: 1 });
    }, "GR-RT10: set propagates exception from inherited getter");
})();

// --- GR-RT14: Modern Intl accepts and deduplicates Unicode u attributes ----
(function testGRRT14() {
    if (typeof Intl === "object") {
        var c1 = Intl.getCanonicalLocales("en-u-abc-abc");
        check(c1.length === 1 && c1[0] === "en-u-abc", "GR-RT14: getCanonicalLocales collapses duplicate attribute, got " + c1);

        var c2 = Intl.getCanonicalLocales("en-u-def-ABC-abc-def");
        check(c2.length === 1 && c2[0] === "en-u-abc-def", "GR-RT14: getCanonicalLocales sorts and collapses case-insensitively, got " + c2);

        var longTag = "en-u" + "-abc".repeat(40);
        var c3 = Intl.getCanonicalLocales(longTag);
        check(c3.length === 1 && c3[0] === "en-u-abc", "GR-RT14: 40 duplicate attributes collapse to en-u-abc, got " + c3);

        if (typeof Intl.Locale === "function") {
            var loc = new Intl.Locale(longTag);
            check(loc.toString() === "en-u-abc", "GR-RT14: Intl.Locale accepts duplicate attributes, got " + loc.toString());
        }
        if (typeof Intl.ListFormat === "function") {
            var lf = new Intl.ListFormat(longTag);
            check(lf.resolvedOptions().locale === "en", "GR-RT14: Intl.ListFormat accepts duplicate attributes");
        }
        if (typeof Intl.RelativeTimeFormat === "function") {
            var rtf = new Intl.RelativeTimeFormat(longTag);
            check(rtf.resolvedOptions().locale === "en", "GR-RT14: Intl.RelativeTimeFormat accepts duplicate attributes");
        }
        if (typeof Intl.DisplayNames === "function") {
            var dn = new Intl.DisplayNames(longTag, { type: "language" });
            check(dn.resolvedOptions().locale === "en", "GR-RT14: Intl.DisplayNames accepts duplicate attributes");
        }
        if (typeof Intl.Segmenter === "function") {
            var seg = new Intl.Segmenter(longTag);
            check(seg.resolvedOptions().locale === "en", "GR-RT14: Intl.Segmenter accepts duplicate attributes");
        }
        if (typeof Intl.DurationFormat === "function") {
            var df = new Intl.DurationFormat(longTag);
            check(df.resolvedOptions().locale === "en", "GR-RT14: Intl.DurationFormat accepts duplicate attributes");
        }
    }
})();

// --- GR-FE8: Generator / AsyncGenerator .prototype read after complex params --
(function testGRFE8() {
    var marker1 = { tag: "gen" };
    function* g3(a = (g3.prototype = marker1)) {
        return a;
    }
    var inst1 = g3();
    check(Object.getPrototypeOf(inst1) === marker1, "GR-FE8: generator reads prototype after default init");

    var marker2 = { tag: "async-gen" };
    async function* ag3(a = (ag3.prototype = marker2)) {
        return a;
    }
    var inst2 = ag3();
    check(Object.getPrototypeOf(inst2) === marker2, "GR-FE8: async generator reads prototype after default init");

    // Generator & AsyncGenerator with non-object prototype fallback after default init
    function* gNonObj(a = (gNonObj.prototype = 42)) {
        return a;
    }
    var genProto = Object.getPrototypeOf((function* () {}).prototype);
    check(Object.getPrototypeOf(gNonObj()) === genProto, "GR-FE8: non-object prototype falls back to %GeneratorPrototype%");

    var asyncGenProto = Object.getPrototypeOf((async function* () {}).prototype);
    async function* agNonObj(a = (agNonObj.prototype = null)) {
        return a;
    }
    check(Object.getPrototypeOf(agNonObj()) === asyncGenProto, "GR-FE8: non-object async-gen prototype falls back to %AsyncGeneratorPrototype%");

    // Method syntax generator & async-generator with default params mutating .prototype
    var log = [];
    var marker3 = { tag: "gm" };
    var objOrder = {
        *gm(a = (log.push("param"), (objOrder.gm.prototype = marker3), 1)) {
            return a;
        }
    };
    var inst3 = objOrder.gm();
    check(Object.getPrototypeOf(inst3) === marker3, "GR-FE8: generator method reads prototype after default init");
    check(log.join(",") === "param", "GR-FE8: generator method parameter initializer ran, got " + log.join(","));

    var marker4 = { tag: "agm" };
    var objAsync = {
        async *agm(a = (objAsync.agm.prototype = marker4, 2)) {
            return a;
        }
    };
    check(Object.getPrototypeOf(objAsync.agm()) === marker4,
        "GR-FE8: async generator method reads prototype after default init");

    // Throwing parameter initializer aborts cleanly without returning a generator
    var throwLog = [];
    function* gThrow(a = (throwLog.push("param"), (function () { throw new RangeError("boom"); })())) {
        return a;
    }
    checkThrows(RangeError, function () {
        gThrow();
    }, "GR-FE8: throwing default param aborts before returning generator");
    check(throwLog.join(",") === "param", "GR-FE8: throwing param ran once");

    // Explicit argument skips default initializer and reads current .prototype
    var overrideProto = { tag: "override" };
    gThrow.prototype = overrideProto;
    check(Object.getPrototypeOf(gThrow(99)) === overrideProto,
        "GR-FE8: subsequent non-throwing call reads current .prototype");
})();

if (typeof CollectGarbage === "function") {
    CollectGarbage();
}

if (failures.length > 0) {
    print("gr_runtime_regressions: " + failures.length + " failure(s)");
    if (typeof WScript !== "undefined" && WScript.Quit) {
        WScript.Quit(1);
    }
    throw new Error("gr_runtime_regressions failed");
}
print("pass");
