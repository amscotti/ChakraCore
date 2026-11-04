//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Detached typed-array element semantics (tc39/ecma262#2164 "web reality"):
// a detached view behaves like a length-0 view for element reads, has, deletes
// and stores (value conversion still runs first and stays observable), and the
// iteration builtins keep looping over the captured length reading undefined.

function assertEq(actual, expected, msg) {
    if (actual !== expected) {
        console.log("FAIL: " + msg + ": expected " + String(expected) + ", got " + String(actual));
        throw new Error("assert");
    }
}

function detach(buffer) {
    ArrayBuffer.detach(buffer);
}

(function testGetHasDelete() {
    var ta = new Float64Array([42, 43]);
    detach(ta.buffer);
    assertEq(ta[0], undefined, "detached get [0]");
    assertEq(ta["1.1"], undefined, "detached get [\"1.1\"]");
    assertEq(ta["-0"], undefined, "detached get [\"-0\"]");
    assertEq(ta[-1], undefined, "detached get [-1]");
    assertEq(ta[2], undefined, "detached get [2]");
    assertEq(Reflect.has(ta, 0), false, "detached has");
    assertEq(Object.getOwnPropertyDescriptor(ta, 0), undefined, "detached getOwnPropertyDescriptor");
    assertEq(delete ta[0], true, "detached sloppy delete");

    var big = new BigInt64Array([42n]);
    detach(big.buffer);
    assertEq(big[0], undefined, "BigInt detached get");
    assertEq(delete big[0], true, "BigInt detached delete");
})();

(function testStrictGetDelete() {
    "use strict";
    var ta = new Float64Array([42]);
    detach(ta.buffer);
    assertEq(ta[0], undefined, "strict detached get");
    assertEq(delete ta[0], true, "strict detached delete must not throw");
})();

(function testStores() {
    var ta = new Float64Array([42]);
    detach(ta.buffer);
    ta[0] = 1;          // sloppy detached store: silent no-op
    ta["1.1"] = 1;      // canonical-numeric-string store: silent no-op
    ta[-1] = 1;
    assertEq(ta[0], undefined, "sloppy store did not throw and did not write");

    var conversionRan = false;
    try {
        ta[0] = { valueOf: function () { conversionRan = true; throw new Error("vo"); } };
    } catch (e) {
        assertEq(e.message, "vo", "detached store still converts the value first");
    }
    assertEq(conversionRan, true, "valueOf observable on detached store");

    var strict = function () {
        "use strict";
        var t = new Int32Array([1]);
        detach(t.buffer);
        t[0] = 1;       // strict detached store: silent no-op, no TypeError
        return t[0];
    };
    assertEq(strict(), undefined, "strict detached store is silent");
})();

(function testDefineProperty() {
    var ta = new Float64Array([17]);
    var res = Reflect.defineProperty(ta, 0, {
        value: { valueOf: function () { detach(ta.buffer); return 42; } }
    });
    assertEq(res, true, "defineProperty returns true when the value conversion detaches");
    assertEq(ta[0], undefined, "defineProperty wrote nothing");

    var already = new Float64Array([1]);
    detach(already.buffer);
    var threw = false;
    try { Object.defineProperty(already, "0", { value: 1 }); } catch (e) { threw = e instanceof TypeError; }
    assertEq(threw, true, "defineProperty on an already-detached view still throws TypeError");
})();

function detachDuringIteration(ctor, first) {
    var loops = 0;
    var ta = new ctor(2);
    ta.every(function (v) {
        if (loops === 0) { detach(ta.buffer); } else { assertEq(v, undefined, "iteration reads undefined after detach"); }
        loops++;
        return true;
    });
    return loops;
}
assertEq(detachDuringIteration(Float64Array, 0), 2, "every runs all captured iterations");
assertEq(detachDuringIteration(Int32Array, 0), 2, "every (int) runs all captured iterations");
assertEq(detachDuringIteration(BigInt64Array, 0), 2, "every (BigInt) runs all captured iterations");

(function testIterationFamily() {
    var count;
    var ta = new Float64Array(2);
    count = 0; ta.some(function (v) { if (count === 0) detach(ta.buffer); count++; return false; });
    assertEq(count, 2, "some loops");
    ta = new Float64Array(2);
    count = 0; ta.forEach(function (v) { if (count === 0) detach(ta.buffer); count++; });
    assertEq(count, 2, "forEach loops");

    ta = new Float64Array(2);
    count = 0;
    var found = ta.find(function (v) { if (count === 0) detach(ta.buffer); count++; return v === 7; });
    assertEq(count, 2, "find loops");
    assertEq(found, undefined, "find result");
    ta = new Float64Array(2);
    var idx = ta.findIndex(function (v, i) { if (i === 0) detach(ta.buffer); return v === 7; });
    assertEq(idx, -1, "findIndex result");
    ta = new Float64Array(2);
    var last = ta.findLast(function (v, i) { if (i === 1) detach(ta.buffer); return v === 7; });
    assertEq(last, undefined, "findLast result");
    ta = new Float64Array(2);
    var lastIdx = ta.findLastIndex(function (v, i) { if (i === 1) detach(ta.buffer); return v === 7; });
    assertEq(lastIdx, -1, "findLastIndex result");

    // reduce: the captured-length loop keeps calling with undefined elements
    // after the detach (no HasProperty gate in the TypedArray algorithm).
    ta = new Float64Array([5, 6]);
    count = 0;
    var sawAfterDetach = -1;
    var reduced = ta.reduce(function (acc, v, i) {
        if (i === 0) { detach(ta.buffer); } else { sawAfterDetach = v; }
        count++;
        return acc + 1;
    }, 100);
    assertEq(count, 2, "reduce callbacks");
    assertEq(sawAfterDetach, undefined, "reduce reads undefined after detach");
    assertEq(reduced, 102, "reduce result");

    // filter keeps raw values during the loop and converts them after.
    ta = new Float64Array(2);
    count = 0;
    var filtered = ta.filter(function (v) { if (count === 0) detach(ta.buffer); count++; return true; });
    assertEq(count, 2, "filter callbacks");
    assertEq(filtered.length, 2, "filter result length");
    assertEq(filtered[0], 0, "filter converts kept raw value");
    assertEq(String(filtered[1]), "NaN", "filter converts kept undefined to NaN");

    var big = new BigInt64Array([1n, 2n, 3n]);
    var calls = 0;
    var filterThrew = false;
    try {
        big.filter(function (v) { if (calls === 0) detach(big.buffer); calls++; return true; });
    } catch (e) {
        filterThrew = e instanceof TypeError;
    }
    assertEq(calls, 3, "BigInt filter runs all callbacks before converting kept values");
    assertEq(filterThrew, true, "BigInt filter conversion of kept undefined throws TypeError");
})();

(function testSearchMethods() {
    var ta = new Float64Array(1);
    assertEq(ta.includes(undefined, { valueOf: function () { detach(ta.buffer); return 0; } }), true, "includes(undefined) after fromIndex detach");
    ta = new Float64Array(1);
    assertEq(ta.includes(0, { valueOf: function () { detach(ta.buffer); return 0; } }), false, "includes(0) after fromIndex detach");
    ta = new Float64Array(1);
    assertEq(ta.indexOf(0, { valueOf: function () { detach(ta.buffer); return 0; } }), -1, "indexOf after fromIndex detach");
    ta = new Float64Array(1);
    assertEq(ta.lastIndexOf(0, { valueOf: function () { detach(ta.buffer); return 0; } }), -1, "lastIndexOf after fromIndex detach");

    var big = new BigInt64Array(1);
    assertEq(big.includes(undefined, { valueOf: function () { detach(big.buffer); return 0; } }), true, "BigInt includes(undefined)");
})();

(function testSetWithDetachingGetter() {
    var ta = new Float64Array(3);
    var getters = 0;
    var src = {
        length: 3,
        get 0() { getters++; return 0; },
        get 1() { getters++; detach(ta.buffer); return 1; },
        get 2() { getters++; return 2; }
    };
    ta.set(src);
    assertEq(getters, 3, "set reads every source value");
    assertEq(ta.length, 0, "set target stays detached");
})();

(function testFromMapperDetachesResult() {
    var target = new Float64Array(3);
    var result = Float64Array.from.call(function () { return target; }, [0, 1, 2], function (v) {
        if (v === 1) detach(target.buffer);
        return v + 10;
    });
    assertEq(result, target, "from returns the constructed target");
    assertEq(result.length, 0, "detached result length");
})();

(function testFloat16DetachedRead() {
    var ta = new Float16Array([1.5]);
    detach(ta.buffer);
    assertEq(ta[0], undefined, "Float16 detached get");
    assertEq(Reflect.has(ta, 0), false, "Float16 detached has");
})();

console.log("pass");

// B3-review fold: TypedArray [[DefineOwnProperty]] uses the post-ES2016 rule —
// configurable/enumerable/writable fields reject only when present-and-FALSE
// (the ES2016 present-and-true rule for configurable was stale; Node-verified).
function chk(name, actual, expected) { if (actual !== expected) { throw new Error(name + ": expected " + expected + ", got " + actual); } }
var dPfull = new Uint8Array(2);
chk("full descriptor with configurable:true accepted", Reflect.defineProperty(dPfull, 0, { value: 42, writable: true, enumerable: true, configurable: true }), true);
chk("full descriptor stores the value", dPfull[0], 42);
chk("configurable:false rejected", Reflect.defineProperty(dPfull, 1, { configurable: false }), false);
chk("enumerable:false rejected", Reflect.defineProperty(dPfull, 1, { enumerable: false }), false);
chk("writable:false rejected", Reflect.defineProperty(dPfull, 1, { writable: false }), false);
