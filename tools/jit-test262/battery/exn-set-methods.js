// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
// Exception-semantics battery for the Set.prototype set methods
// (jit-reentrancy-fix lane). Verifies the BEGIN_SAFE_REENTRANT_CALL arming did
// not change the spec's abrupt-completion behavior: the USER exception object
// propagates with identity preserved, throwing `has`/`keys`/`next` abort the
// method WITHOUT closing the keys iterator (IteratorStepValue has no
// IfAbruptCloseIterator), and the normal-completion close path
// (CloseKeysIteratorNormalCompletion) runs `return` observably and propagates
// its errors/non-object results per spec.
//
// Method -> callback map (source: the Entry* implementations):
//   union, symmetricDifference, isSupersetOf: keys iterator only
//   intersection, difference, isDisjointFrom: has-walk when |receiver| <=
//     |record|, keys-walk otherwise
//   isSubsetOf: has-walk only (keys never read)
//
// Expected-value oracle: the interpreter (-nonative) run of this same script
// (matches test262, 372/372).
//
// Usage: ch exn-set-methods.js   -> prints "pass", exit 0

function fail(msg) {
    throw new Error("exn battery: " + msg);
}

function expectSentinel(fn, sentinel, what) {
    try {
        fn();
    } catch (e) {
        if (e !== sentinel) {
            fail(what + ": wrong exception: expected sentinel, got " + e);
        }
        return;
    }
    fail(what + ": did not throw");
}

function expectTypeError(fn, what) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof TypeError)) {
            fail(what + ": expected TypeError, got " + e);
        }
        return;
    }
    fail(what + ": did not throw");
}

// keys() yields `values`, counts every observable call, and can throw at each
// stage via opts: {throwSize, throwHas, hasValueOf, throwKeys, nonObjectKeys,
// throwNextAt+throwNext, throwDoneAt+throwDone, throwValueAt+throwValue,
// throwReturn, returnNonObject, sizeOf}
function exnRecorder(values, opts) {
    opts = opts || {};
    var counts = { size: 0, has: 0, keys: 0, next: 0, return: 0 };
    return {
        counts: counts,
        get size() {
            counts.size++;
            if (opts.throwSize) { throw opts.throwSize; }
            return opts.sizeOf === undefined ? values.length : opts.sizeOf;
        },
        has: function (v) {
            counts.has++;
            if (opts.throwHas) { throw opts.throwHas; }
            if (opts.hasValueOf) { return { valueOf: function () { throw opts.hasValueOf; } }; }
            return values.indexOf(v) !== -1;
        },
        keys: function () {
            counts.keys++;
            if (opts.throwKeys) { throw opts.throwKeys; }
            if (opts.nonObjectKeys !== undefined) { return opts.nonObjectKeys; }
            var index = 0;
            return {
                next: function () {
                    counts.next++;
                    if (opts.throwNextAt !== undefined && counts.next > opts.throwNextAt) {
                        throw opts.throwNext;
                    }
                    if (index >= values.length) { return { done: true }; }
                    var i = index++;
                    if (opts.throwDoneAt === i) {
                        return { get done() { throw opts.throwDone; }, value: values[i] };
                    }
                    if (opts.throwValueAt === i) {
                        return { done: false, get value() { throw opts.throwValue; } };
                    }
                    return { done: false, value: values[i] };
                },
                return: function () {
                    counts.return++;
                    if (opts.throwReturn) { throw opts.throwReturn; }
                    if (opts.returnNonObject) { return opts.returnNonObject; }
                    return { done: true };
                }
            };
        }
    };
}

// Methods whose keys-iterator walk runs when the receiver {1,2,3} (size 3)
// meets a record of size 2 (or always, for union/symmetricDifference/
// isSupersetOf). isSubsetOf is excluded: it never reads keys.
var KEYS_METHODS = ["union", "intersection", "difference", "symmetricDifference",
                    "isSupersetOf", "isDisjointFrom"];

function exnPass() {
    var a = new Set([1, 2, 3]);
    var small = new Set([1, 2]);

    // 1. has() throws: every method that consults has (receiver size 2 <=
    // record size 2 selects the has-walk) propagates the sentinel, never
    // reads keys, and never closes an iterator.
    var methodsWithHas = ["intersection", "difference", "isSubsetOf", "isDisjointFrom"];
    for (var i = 0; i < methodsWithHas.length; i++) {
        var m = methodsWithHas[i];
        var sentinel = new Error("has:" + m);
        var rec = exnRecorder([3, 4], { throwHas: sentinel });
        (function (method) {
            expectSentinel(function () { small[method](rec); }, sentinel, "has throws in " + method);
        })(m);
        if (rec.counts.keys !== 0) { fail(m + ": keys() read after throwing has"); }
        if (rec.counts.has < 1) { fail(m + ": has never consulted (test shape wrong)"); }
    }

    // 2. keys() throws (GetSetLikeKeysIterator's CALL_FUNCTION): every
    // keys-reading method propagates; no next/return calls happen.
    for (var i = 0; i < KEYS_METHODS.length; i++) {
        var m = KEYS_METHODS[i];
        var sentinel = new Error("keys:" + m);
        var rec = exnRecorder([3, 4], { throwKeys: sentinel });
        (function (method) {
            expectSentinel(function () { a[method](rec); }, sentinel, "keys throws in " + method);
        })(m);
        if (rec.counts.next !== 0 || rec.counts.return !== 0) {
            fail(m + ": iterator touched after throwing keys()");
        }
    }

    // 3. next() throws (SetLikeKeysStep): the sentinel propagates and the
    // iterator is NOT closed (no IfAbruptCloseIterator on IteratorStepValue).
    // Values chosen so exactly one next() completes before the throw and no
    // method early-exits first: union/intersection/difference/
    // symmetricDifference never early-exit; isSupersetOf's first key must be
    // present (3 is), isDisjointFrom's must be absent (9 is).
    var CASE3 = {
        union: [3, 4],
        intersection: [3, 4],
        difference: [3, 4],
        symmetricDifference: [3, 4],
        isSupersetOf: [3, 9],
        isDisjointFrom: [9, 4]
    };
    for (var i = 0; i < KEYS_METHODS.length; i++) {
        var m = KEYS_METHODS[i];
        var sentinel = new Error("next:" + m);
        var rec = exnRecorder(CASE3[m], { throwNextAt: 1, throwNext: sentinel });
        (function (method) {
            expectSentinel(function () { a[method](rec); }, sentinel, "next throws in " + method);
        })(m);
        if (rec.counts.return !== 0) { fail(m + ": iterator closed after abrupt next()"); }
        if (rec.counts.next < 2) { fail(m + ": next() never called (test shape wrong)"); }
    }

    // 4. done getter throws (IteratorComplete, armed implicit-call machinery):
    // propagates, no close.
    var sentinelDone = new Error("doneGetter");
    var recDone = exnRecorder([3, 4], { throwDoneAt: 0, throwDone: sentinelDone });
    expectSentinel(function () { a.union(recDone); }, sentinelDone, "done getter throws");
    if (recDone.counts.return !== 0) { fail("union: iterator closed after abrupt done-get"); }

    // 5. value getter throws (IteratorValue): propagates, no close.
    var sentinelValue = new Error("valueGetter");
    var recValue = exnRecorder([3, 4], { throwValueAt: 0, throwValue: sentinelValue });
    expectSentinel(function () { a.union(recValue); }, sentinelValue, "value getter throws");
    if (recValue.counts.return !== 0) { fail("union: iterator closed after abrupt value-get"); }

    // 6. keys() returns a non-Object: TypeError from GetSetLikeKeysIterator.
    expectTypeError(function () { a.union(exnRecorder([3, 4], { nonObjectKeys: 42 })); },
                    "keys() returning 42");

    // 7. Normal-completion close (CloseKeysIteratorNormalCompletion's
    // CALL_FUNCTION), reached on the early-false paths of isSupersetOf and
    // isDisjointFrom: `return` runs observably.
    var recClose = exnRecorder([9, 3, 4], {});
    if (a.isSupersetOf(recClose) !== false) { fail("isSupersetOf(9,3,4) of {1,2,3}"); }
    if (recClose.counts.return !== 1) { fail("isSupersetOf: return() not called on early close"); }
    var recClose2 = exnRecorder([9, 1], {}); // size 2 < 3: keys walk, 1 hits
    if (a.isDisjointFrom(recClose2) !== false) { fail("isDisjointFrom(9,1) of {1,2,3}"); }
    if (recClose2.counts.return !== 1) { fail("isDisjointFrom: return() not called on early close"); }

    // 8. return() throws during that close: the error propagates (this close
    // site is on a normal completion; it must not swallow the throw).
    var sentinelReturn = new Error("returnThrows");
    var recReturn = exnRecorder([9, 3], { throwReturn: sentinelReturn });
    expectSentinel(function () { a.isSupersetOf(recReturn); }, sentinelReturn,
                   "return() throws on early close");
    if (recReturn.counts.return !== 1) { fail("throwing return(): call count"); }

    // 9. return() returns a non-Object during the close: TypeError.
    expectTypeError(
        function () { a.isSupersetOf(exnRecorder([9, 3], { returnNonObject: 7 })); },
        "return() returning 7 on early close");

    // 10. ToBoolean of has()'s result never runs user code: an object with a
    // throwing valueOf is truthy and valueOf is NOT called.
    var valueOfSentinel = new Error("valueOf");
    var recTruthy = exnRecorder([1, 2], { hasValueOf: valueOfSentinel });
    if (small.intersection(recTruthy).size !== 2) {
        fail("intersection: throwing-valueOf object must be truthy without calling valueOf");
    }
    if (recTruthy.counts.has !== 2) { fail("intersection: has calls with valueOf object"); }

    // 11. size getter throws and size valueOf throws (the armed implicit-call
    // sites OUTSIDE the sweep list): still propagate under a JIT caller.
    var sentinelSize = new Error("sizeGetter");
    expectSentinel(
        function () { a.union(exnRecorder([3], { throwSize: sentinelSize })); },
        sentinelSize, "size getter throws");
    var sentinelToNumber = new Error("sizeToNumber");
    expectSentinel(
        function () {
            a.union(exnRecorder([3], {
                sizeOf: { valueOf: function () { throw sentinelToNumber; } }
            }));
        },
        sentinelToNumber, "size valueOf throws");
}

// The battery pass loop is TOP-LEVEL and invokes exnPass() by name, so the
// throwing-transition paths also execute under JIT-compiled callers in every
// mode (default tier-up, -mic:1, -forceNative); each pass re-enters after
// caught throws, proving no state corruption on repeat.
var PASSES = 1200;
for (var pass = 0; pass < PASSES; pass++) {
    exnPass();
}
print("pass");
