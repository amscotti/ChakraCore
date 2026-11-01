//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Warmed-caller regression for C2-JIT-1/C2-JIT-2 (doc/c2-jit-reentrancy-fix.md):
// the Set.prototype set methods and Object/Map.groupBy SIGABRT when their
// C++->JS transitions (record.has / record.keys / keys-iterator next /
// close-return / groupBy callback) run from a JIT-compiled caller without
// BEGIN_SAFE_REENTRANT_CALL arming. The interpreter's OP_Call safe-reentrancy
// region masks the defect for one-shot interpreted callers, which is why the
// one-shot shapes never caught it; the warm loops below are TOP-LEVEL with
// >=1000 iterations, the shape that deterministically tiers up under default
// flags (helpers by invocation count; the loop itself under -mic:1) and is
// force-compiled under the dynapogo/fulljit variants.
//
// The warm phase records evidence (result contents, callback call counts,
// propagated-exception identities); the tests assert it afterwards, so a
// wrong value fails even when the process does not abort.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var ITERATIONS = 1200;

var evidence = {
    union: [], intersection: [], difference: [], symmetricDifference: [],
    isSubsetOf: null, isSupersetOf: null, isDisjointFrom: null,
    hasCalls: 0, keysCalls: 0, nextCalls: 0, returnCalls: 0,
    realSetUnion: null,
    objectGroupByKeys: "", objectGroupByEven: null, mapGroupByZero: null,
    groupByCallbackCalls: 0,
    hasThrowIdentity: false, hasThrowNoClose: false,
    groupByThrowIdentity: false, groupByThrowClosed: false,
    warmCompleted: false
};

function setLike(values, counts) {
    return {
        get size() { return values.length; },
        has: function (v) {
            counts.has++;
            return values.indexOf(v) !== -1;
        },
        keys: function () {
            counts.keys++;
            var index = 0;
            return {
                next: function () {
                    counts.next++;
                    if (index >= values.length) { return { done: true }; }
                    return { done: false, value: values[index++] };
                },
                return: function () {
                    counts.return++;
                    return { done: true };
                }
            };
        }
    };
}

function contents(set) {
    var out = [];
    set.forEach(function (v) { out.push(v); });
    return out.join(",");
}

// Throwing-transition probes: fresh sentinels each pass, caught in-loop.
function exceptionPass() {
    var a = new Set([1, 2]);
    var sentinel = new Error("has");
    var counts = { has: 0, keys: 0, next: 0, return: 0 };
    var like = {
        get size() { return 2; },
        has: function () { throw sentinel; },
        keys: function () {
            counts.keys++;
            var index = 0;
            return {
                next: function () { return index++ < 2 ? { done: false, value: 3 } : { done: true }; },
                return: function () { counts.return++; return { done: true }; }
            };
        }
    };
    try {
        a.intersection(like); // size 2 <= 2: the has-walk runs first
    } catch (e) {
        evidence.hasThrowIdentity = evidence.hasThrowIdentity || e === sentinel;
    }
    evidence.hasThrowNoClose = evidence.hasThrowNoClose || counts.keys === 0;

    var cbSentinel = new Error("cb");
    var returnCalls = 0;
    var items = {
        [Symbol.iterator]: function () {
            var index = 0;
            return {
                next: function () { return index++ < 2 ? { done: false, value: index } : { done: true }; },
                return: function () { returnCalls++; return { done: true }; }
            };
        }
    };
    try {
        Object.groupBy(items, function (v, k) {
            if (k === 1) { throw cbSentinel; }
            return v;
        });
    } catch (e) {
        evidence.groupByThrowIdentity = evidence.groupByThrowIdentity || e === cbSentinel;
    }
    evidence.groupByThrowClosed = evidence.groupByThrowClosed || returnCalls === 1;
}

function warmPass(i) {
    var a = new Set([1, 2, 3]);
    var counts = { has: 0, keys: 0, next: 0, return: 0 };

    // Receiver larger than the record: union/symmetricDifference take the
    // keys walk; isSupersetOf early-exits on a missing key, closing the
    // iterator (return runs).
    var large = setLike([3, 4], counts);
    evidence.union.push(contents(a.union(large)));
    evidence.symmetricDifference.push(contents(a.symmetricDifference(large)));
    evidence.isSupersetOf = a.isSupersetOf(large);

    // Receiver not larger: intersection consults record.has per entry.
    var small = new Set([1, 2]);
    var like2 = setLike([3, 4], counts);
    evidence.intersection.push(contents(small.intersection(like2)));
    evidence.difference.push(contents(small.difference(like2)));
    evidence.isSubsetOf = small.isSubsetOf(like2);
    evidence.isDisjointFrom = small.isDisjointFrom(like2);

    evidence.realSetUnion = contents(a.union(new Set([3, 4])));

    var items = [6, 1, 5, 2];
    var cbCount = 0;
    var countCb = function (v) { cbCount++; return v % 2; };
    evidence.objectGroupByKeys = Object.keys(Object.groupBy(items, countCb)).join(";");
    evidence.objectGroupByEven = Object.groupBy(items, countCb)["0"].join(",");
    evidence.mapGroupByZero = Map.groupBy(items, countCb).get(0).join(",");
    evidence.groupByCallbackCalls += cbCount;

    evidence.hasCalls += counts.has;
    evidence.keysCalls += counts.keys;
    evidence.nextCalls += counts.next;
    evidence.returnCalls += counts.return;

    exceptionPass();
}

// The warmed loops are TOP-LEVEL: under default flags the per-pass helpers
// tier up by invocation count and the loop itself tiers under -mic:1; the
// dynapogo/fulljit variants force-compile everything up front. A missing
// BEGIN_SAFE_REENTRANT_CALL at any of the armed transitions SIGABRTs here
// (exit 134) before any test reports.
for (var i = 0; i < ITERATIONS; i++) {
    warmPass(i);
}
evidence.warmCompleted = true;

var tests = [
    {
        name: "Set methods return the spec results from a warmed caller",
        body() {
            assert.areEqual("1,2,3,4", evidence.union[0], "union contents");
            assert.areEqual("1,2,4", evidence.symmetricDifference[0], "symmetricDifference contents");
            assert.areEqual("", evidence.intersection[0], "intersection contents (nothing shared)");
            assert.areEqual("1,2", evidence.difference[0], "difference contents");
            assert.areEqual("1,2,3,4", evidence.realSetUnion, "union of a real Set argument");
            assert.isFalse(evidence.isSupersetOf, "isSupersetOf with a missing key");
            assert.isFalse(evidence.isSubsetOf, "isSubsetOf with an absent entry");
            assert.isTrue(evidence.isDisjointFrom, "isDisjointFrom with no shared entry");
        }
    },
    {
        name: "The set-like record's user callbacks actually ran (the C++->JS transitions executed)",
        body() {
            assert.isTrue(evidence.hasCalls >= ITERATIONS, "record.has consulted on the has-walk: " + evidence.hasCalls);
            assert.isTrue(evidence.keysCalls >= 2 * ITERATIONS, "record.keys called: " + evidence.keysCalls);
            assert.isTrue(evidence.nextCalls >= 3 * ITERATIONS, "keys-iterator next called: " + evidence.nextCalls);
            assert.isTrue(evidence.returnCalls >= ITERATIONS, "close-return called on the early exit: " + evidence.returnCalls);
        }
    },
    {
        name: "groupBy groups, orders, and coerces from a warmed caller",
        body() {
            assert.areEqual("0;1", evidence.objectGroupByKeys, "Object.groupBy key order");
            assert.areEqual("6,2", evidence.objectGroupByEven, "Object.groupBy even group");
            assert.areEqual("6,2", evidence.mapGroupByZero, "Map.groupBy +0 group");
            assert.isTrue(evidence.groupByCallbackCalls >= 12 * ITERATIONS,
                          "groupBy callback invocations: " + evidence.groupByCallbackCalls);
        }
    },
    {
        name: "Throwing has/groupBy callback propagate with identity and spec close semantics",
        body() {
            assert.isTrue(evidence.hasThrowIdentity, "a throwing record.has propagates the user error object");
            assert.isTrue(evidence.hasThrowNoClose, "a throwing has aborts before keys() is read");
            assert.isTrue(evidence.groupByThrowIdentity, "a throwing groupBy callback propagates the user error object");
            assert.isTrue(evidence.groupByThrowClosed, "groupBy closes the items iterator at the callback site");
        }
    },
    {
        name: "The warm loops ran to completion",
        body() {
            assert.isTrue(evidence.warmCompleted, "warmCompleted");
            assert.areEqual(ITERATIONS, evidence.union.length, "one union result per pass");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
