// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
// Exception-semantics battery for Object.groupBy / Map.groupBy
// (jit-reentrancy-fix lane). Verifies the BEGIN_SAFE_REENTRANT_CALL arming of
// GroupByCallCallback / MapGroupByCallCallback did not change the spec's
// abrupt-completion behavior:
//   - callback throw: the USER exception propagates with identity preserved,
//     and the items iterator IS closed (IfAbruptCloseIterator at the Call
//     callbackfn site); a throwing return() loses to the original error
//   - ToPropertyKey throw (Object.groupBy only): also a close site
//   - next()/done/value throws (IteratorStepValue): propagates WITHOUT close
//
// Expected-value oracle: the interpreter (-nonative) run of this same script.
//
// Usage: ch exn-groupby.js   -> prints "pass", exit 0

// The battery pass loop is at the BOTTOM of this file (top-level, invoking
// groupByExnPass() by name) so the throwing-transition paths also execute
// under JIT-compiled callers in every mode (default tier-up, -mic:1,
// -forceNative); each pass re-enters after caught throws, proving no state
// corruption on repeat.
var PASSES = 1200;

function fail(msg) {
    throw new Error("groupBy exn battery: " + msg);
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

// items iterable over [6,1,5,2] with observable next()/return() and
// per-stage throw hooks.
function itemsIterable(opts) {
    opts = opts || {};
    var items = [6, 1, 5, 2];
    var counts = { iterator: 0, next: 0, return: 0 };
    return {
        counts: counts,
        [Symbol.iterator]: function () {
            counts.iterator++;
            var index = 0;
            return {
                next: function () {
                    counts.next++;
                    if (opts.throwNextAt !== undefined && counts.next > opts.throwNextAt) {
                        throw opts.throwNext;
                    }
                    if (index >= items.length) { return { done: true }; }
                    var i = index++;
                    if (opts.throwDoneAt === i) {
                        return { get done() { throw opts.throwDone; }, value: items[i] };
                    }
                    if (opts.throwValueAt === i) {
                        return { done: false, get value() { throw opts.throwValue; } };
                    }
                    return { done: false, value: items[i] };
                },
                return: function (r) {
                    counts.return++;
                    if (opts.throwReturn) { throw opts.throwReturn; }
                    return { done: true };
                }
            };
        }
    };
}

function bothGroupBys(fn) {
    fn(Object.groupBy, "Object.groupBy");
    fn(Map.groupBy, "Map.groupBy");
}

function groupByExnPass() {
    // 1. Callback throws on the 2nd item: sentinel propagates, iterator
    // closed exactly once, and the callback saw item 0 and its index.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("cb:" + name);
        var seen = [];
        var items = itemsIterable({});
        try {
            groupBy(items, function (v, k) {
                seen.push([v, k]);
                if (seen.length === 2) { throw sentinel; }
                return v;
            });
        } catch (e) {
            if (e !== sentinel) { fail(name + " cb throw: wrong exception " + e); }
            if (items.counts.return !== 1) {
                fail(name + " cb throw: iterator not closed exactly once (return=" +
                     items.counts.return + ")");
            }
            if (seen[0][0] !== 6 || seen[0][1] !== 0 || seen[1][0] !== 1 || seen[1][1] !== 1) {
                fail(name + " cb throw: (value,index) desync: " + JSON.stringify(seen));
            }
            return;
        }
        fail(name + " cb throw: did not throw");
    });

    // 2. Callback throws AND return() throws: the ORIGINAL error wins.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("cb-wins:" + name);
        var other = new Error("return-loses:" + name);
        var items = itemsIterable({ throwReturn: other });
        try {
            groupBy(items, function () { throw sentinel; });
        } catch (e) {
            if (e !== sentinel) { fail(name + ": original error must win, got " + e); }
            return;
        }
        fail(name + ": did not throw");
    });

    // 3. Object.groupBy only: ToPropertyKey of the callback result throws
    // (toString). Also a close site; the sentinel wins over a throwing
    // return().
    var keySentinel = new Error("toPropertyKey");
    var other2 = new Error("return-loses-2");
    var items3 = itemsIterable({ throwReturn: other2 });
    try {
        Object.groupBy(items3, function (v, k) {
            return k === 1 ? { toString: function () { throw keySentinel; } } : v;
        });
        fail("Object.groupBy ToPropertyKey: did not throw");
    } catch (e) {
        if (e !== keySentinel) { fail("Object.groupBy ToPropertyKey: wrong exception " + e); }
        if (items3.counts.return !== 1) { fail("Object.groupBy ToPropertyKey: not closed"); }
    }

    // 4. next() throws (IteratorStepValue sits OUTSIDE the close-wrapping
    // try): sentinel propagates, NO close.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("next:" + name);
        var items = itemsIterable({ throwNextAt: 1, throwNext: sentinel });
        try {
            groupBy(items, function (v) { return v; });
        } catch (e) {
            if (e !== sentinel) { fail(name + " next throw: wrong exception " + e); }
            if (items.counts.return !== 0) { fail(name + " next throw: iterator must NOT close"); }
            return;
        }
        fail(name + " next throw: did not throw");
    });

    // 5. done getter throws: propagates, no close.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("done:" + name);
        var items = itemsIterable({ throwDoneAt: 0, throwDone: sentinel });
        try {
            groupBy(items, function (v) { return v; });
        } catch (e) {
            if (e !== sentinel) { fail(name + " done throw: wrong exception " + e); }
            if (items.counts.return !== 0) { fail(name + " done throw: iterator must NOT close"); }
            return;
        }
        fail(name + " done throw: did not throw");
    });

    // 6. value getter throws: propagates, no close.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("value:" + name);
        var items = itemsIterable({ throwValueAt: 0, throwValue: sentinel });
        try {
            groupBy(items, function (v) { return v; });
        } catch (e) {
            if (e !== sentinel) { fail(name + " value throw: wrong exception " + e); }
            if (items.counts.return !== 0) { fail(name + " value throw: iterator must NOT close"); }
            return;
        }
        fail(name + " value throw: did not throw");
    });

    // 7. @@iterator itself throws (GetIterator, armed implicit machinery):
    // propagates under a JIT caller.
    bothGroupBys(function (groupBy, name) {
        var sentinel = new Error("iteratorMethod:" + name);
        var bad = {};
        Object.defineProperty(bad, Symbol.iterator, {
            get: function () { throw sentinel; }
        });
        try {
            groupBy(bad, function (v) { return v; });
        } catch (e) {
            if (e !== sentinel) { fail(name + " @@iterator throw: wrong exception " + e); }
            return;
        }
        fail(name + " @@iterator throw: did not throw");
    });

    // 8. Non-callable callback: TypeError before items is touched.
    bothGroupBys(function (groupBy, name) {
        var touched = false;
        var items = itemsIterable({});
        items[Symbol.iterator] = function () { touched = true; return [][Symbol.iterator](); };
        try {
            groupBy(items, "not a function");
        } catch (e) {
            if (!(e instanceof TypeError)) { fail(name + " non-callable: expected TypeError"); }
            if (touched) { fail(name + " non-callable: items touched before check"); }
            return;
        }
        fail(name + " non-callable: did not throw");
    });
}

for (var pass = 0; pass < PASSES; pass++) {
    groupByExnPass();
}
print("pass");
