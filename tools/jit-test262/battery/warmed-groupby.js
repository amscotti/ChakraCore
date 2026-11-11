// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
// C2-JIT-2 warmed-caller battery (jit-reentrancy-fix lane).
//
// Drives Object.groupBy and Map.groupBy from a >=1000-iteration loop so the
// calling function is JIT-compiled, and asserts grouping contents, key order,
// callback/iterator call counts, -0 key canonicalization (Map) and property-key
// coercion (Object). SIGABRT or wrong values fail.
//
// Expected-value oracle: the interpreter (-nonative) run of this same script.
//
// Usage: ch warmed-groupby.js   -> prints "pass", exit 0

var ITERATIONS = 1200;

function fail(msg) {
    throw new Error("groupBy battery: " + msg);
}

function eqArray(a, b, what) {
    if (a.length !== b.length) { fail(what + ": length " + a.length + " != " + b.length); }
    for (var i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) { fail(what + "[" + i + "]: " + a[i] + " != " + b[i]); }
    }
}

function oneGroupByPass() {
    var items = [6, 1, 5, 2];
    var cbCalls = 0;
    var nextCalls = 0;

    var iterable = {
        [Symbol.iterator]: function () {
            var index = 0;
            return {
                next: function () {
                    nextCalls++;
                    return index >= items.length
                        ? { done: true }
                        : { done: false, value: items[index++] };
                },
                return: function () { return { done: true }; }
            };
        }
    };

    function cb(v, k) {
        cbCalls++;
        if (k < 0 || k >= items.length) { fail("groupBy callback index " + k + " out of range"); }
        if (items[k] !== v) { fail("groupBy callback value/index desync at " + k); }
        return v % 2;
    }

    var obj = Object.groupBy(iterable, cb);
    var keys = Object.keys(obj);
    if (keys.join(";") !== "0;1") { fail("Object.groupBy key order: " + keys.join(";")); }
    eqArray(obj["0"], [6, 2], "Object.groupBy even group");
    eqArray(obj["1"], [1, 5], "Object.groupBy odd group");
    if (Object.getPrototypeOf(obj) !== null) { fail("Object.groupBy prototype not null"); }

    cbCalls = 0;
    nextCalls = 0;
    var map = Map.groupBy(iterable, cb);
    if (map.size !== 2) { fail("Map.groupBy size " + map.size); }
    eqArray(map.get(1), [1, 5], "Map.groupBy odd group");
    eqArray(map.get(0), [6, 2], "Map.groupBy even group");
    var mapKeys = [];
    map.forEach(function (v, k) { mapKeys.push(k); });
    eqArray(mapKeys, [0, 1], "Map.groupBy key order");

    if (cbCalls !== items.length) { fail("Map.groupBy cb calls " + cbCalls); }
    if (nextCalls !== items.length + 1) { fail("Map.groupBy next calls " + nextCalls); }
}

function negZeroKeyPass() {
    // Map.groupBy canonicalizes -0 to +0: both entries land in ONE group under
    // key +0. Object.groupBy ToPropertyKey(-0) is "0": also one group.
    var vals = [-0, 3];
    function cb(v) { return v === 0 ? -0 : v; }
    var map = Map.groupBy([0, 3], function (v) { return v === 0 ? -0 : v; });
    if (map.size !== 2) { fail("Map.groupBy(-0): size " + map.size); }
    var zeroGroup = map.get(0);
    if (zeroGroup === undefined || map.get(-0) !== zeroGroup) {
        fail("Map.groupBy(-0): -0 not canonicalized to +0");
    }
    var obj = Object.groupBy([0, 3], cb);
    if (Object.keys(obj).join(";") !== "0;3") { fail("Object.groupBy(-0) keys"); }
}

function oddShapesPass() {
    // empty iterable -> empty result, no callback calls
    var called = 0;
    var emptyObj = Object.groupBy([], function () { called++; return 1; });
    if (Object.keys(emptyObj).length !== 0 || called !== 0) { fail("Object.groupBy(empty)"); }
    if (Map.groupBy([], function () { called++; return 1; }).size !== 0 || called !== 0) {
        fail("Map.groupBy(empty)");
    }

    // string items (iterable), symbol keys preserved by Map.groupBy
    var sym = Symbol("g");
    var map = Map.groupBy("ab", function (c, k) { return k === 0 ? sym : "b"; });
    if (map.size !== 2 || map.get(sym).join("") !== "a") { fail("Map.groupBy symbol key"); }

    // Object.groupBy: symbol keys become strings ("Symbol(g)")? No:
    // ToPropertyKey of a symbol stays a symbol key on the null-proto object.
    var obj = Object.groupBy("xy", function (c, k) { return k === 0 ? sym : "y"; });
    if (Object.keys(obj).join(";") !== "y") { fail("Object.groupBy string keys leaked symbol"); }
    eqArray(obj[sym], ["x"], "Object.groupBy symbol-keyed group");
}

// Top-level warmed loop (helpers tier by invocation under default flags; the
// loop itself tiers under -mic:1; everything compiles under -forceNative).
var sink = 0;
for (var i = 0; i < ITERATIONS; i++) {
    oneGroupByPass();
    negZeroKeyPass();
    oddShapesPass();
    sink += i & 1;
}

if (sink !== ITERATIONS / 2) { fail("loop did not run to completion"); }
print("pass");
