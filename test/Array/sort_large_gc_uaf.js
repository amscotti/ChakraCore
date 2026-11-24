//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("sort_large_gc_uaf: FAIL: " + message);
    if (typeof WScript !== "undefined" && typeof WScript.Quit === "function") {
        WScript.Quit(1);
    }
    throw new Error(message);
}

function assert(cond, message) {
    if (!cond) {
        fail(message);
    }
}

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// 1. Default sort of 350,000 non-integral doubles (each ToString allocates a fresh string).
function testLargeDoubleDefaultSort() {
    var N = 350000;
    var a = new Array(N);
    var sumBefore = 0;
    for (var i = 0; i < N; i++) {
        var val = ((i * 2654435761) % 1000003) * 0.5;
        a[i] = val;
        sumBefore += val;
    }
    a.sort();
    assert(a.length === N, "large double sort length mismatch: " + a.length);
    assert(a[0] === 0, "unexpected first element: " + a[0]);

    var prevStr = "" + a[0];
    var sumAfter = a[0];
    for (var i = 1; i < N; i++) {
        var cur = a[i];
        var curStr = "" + cur;
        if (prevStr > curStr) {
            fail("large double sort out of lexicographical order at " + i + ": " + prevStr + " > " + curStr);
        }
        prevStr = curStr;
        sumAfter += cur;
    }
    assert(sumAfter === sumBefore, "large double sort sum mismatch: " + sumAfter + " !== " + sumBefore);
}

// 2. Default sort of objects whose toString() produces fresh unrooted strings and forces GC.
function testDefaultSortFreshToStringWithGC() {
    var N = 1200;
    var a = new Array(N);
    for (var i = 0; i < N; i++) {
        a[i] = {
            v: (i * 48271) % 10007,
            id: i,
            toString: function () {
                if ((this.id & 63) === 0 || this.id === N - 1) {
                    gcNow();
                }
                return "k_" + (10000 + this.v) + "_" + (10000 + this.id);
            }
        };
    }

    a.sort();
    assert(a.length === N, "fresh toString sort length mismatch");
    for (var i = 1; i < N; i++) {
        var prev = a[i - 1];
        var cur = a[i];
        var prevKey = "k_" + (10000 + prev.v) + "_" + (10000 + prev.id);
        var curKey = "k_" + (10000 + cur.v) + "_" + (10000 + cur.id);
        if (prevKey >= curKey) {
            fail("fresh toString sort order mismatch at " + i + ": " + prevKey + " >= " + curKey);
        }
    }

    // Exercise Array.prototype.toSorted on the same SortHelper<StringItem> path.
    var b = new Array(700);
    for (var i = 0; i < b.length; i++) {
        b[i] = {
            v: (700 - i),
            toString: function () {
                if ((this.v & 63) === 0) {
                    gcNow();
                }
                return "ts_" + (1000 + this.v);
            }
        };
    }
    var bSorted = b.toSorted();
    assert(bSorted.length === 700, "toSorted length mismatch");
    assert(bSorted[0].v === 1 && bSorted[699].v === 700, "toSorted endpoints mismatch");
    assert(b[0].v === 700, "toSorted mutated source array");
}

// 3. Comparator MergeSort (>= 600 objects) where comparator drops external refs and triggers GC.
// Clearing the target array inside compareFn ensures unmerged right-half elements are held only
// in MergeSort's scratch buffer (and list) across CollectGarbage().
function testComparatorMergeSortWithGC() {
    var N = 800;
    var a = new Array(N);
    var shadow = new Array(N);
    for (var i = 0; i < N; i++) {
        var obj = { k: (i * 997) % 1009, seq: i };
        a[i] = obj;
        shadow[i] = obj;
    }

    var calls = 0;
    var cleared = false;
    a.sort(function (x, y) {
        calls++;
        if (!cleared && calls > 50) {
            cleared = true;
            shadow = null;
            for (var j = 0; j < N; j++) {
                a[j] = 0;
            }
            gcNow();
        } else if ((calls & 127) === 0) {
            gcNow();
        }
        return x.k - y.k || x.seq - y.seq;
    });

    assert(cleared, "comparator did not reach clearing phase");
    assert(a.length === N, "comparator MergeSort length mismatch");
    for (var i = 0; i < N; i++) {
        assert(typeof a[i] === "object" && a[i] !== null, "element missing after GC at " + i);
        if (i > 0) {
            var cmp = (a[i - 1].k - a[i].k) || (a[i - 1].seq - a[i].seq);
            if (cmp >= 0) {
                fail("comparator MergeSort out of order at " + i);
            }
        }
    }
}

// 4. Exception paths mid-population (toString throws) and mid-MergeSort (compareFn throws),
// followed by repeated healthy sorts.
function testThrowRecovery() {
    var toStrArr = new Array(700);
    for (var i = 0; i < toStrArr.length; i++) {
        toStrArr[i] = {
            v: i,
            toString: function () {
                if (this.v === 350) {
                    gcNow();
                    throw new RangeError("boom-toString");
                }
                return "e_" + this.v;
            }
        };
    }
    var caughtToString = false;
    try {
        toStrArr.sort();
    } catch (e) {
        caughtToString = (e instanceof RangeError && e.message === "boom-toString");
    }
    assert(caughtToString, "expected RangeError from toString");

    var cmpArr = new Array(800);
    for (var i = 0; i < cmpArr.length; i++) {
        cmpArr[i] = { v: 800 - i };
    }
    var cmpCalls = 0;
    var caughtCmp = false;
    try {
        cmpArr.sort(function (x, y) {
            cmpCalls++;
            if (cmpCalls === 200) {
                gcNow();
                throw new TypeError("boom-compare");
            }
            return x.v - y.v;
        });
    } catch (e) {
        caughtCmp = (e instanceof TypeError && e.message === "boom-compare");
    }
    assert(caughtCmp, "expected TypeError from compareFn");

    // Repeated sorts afterwards must remain completely healthy.
    for (var round = 0; round < 3; round++) {
        var okDefault = new Array(1000);
        for (var i = 0; i < okDefault.length; i++) {
            okDefault[i] = ((i * 7919) % 1009) * 0.25;
        }
        okDefault.sort();
        for (var i = 1; i < okDefault.length; i++) {
            if (String(okDefault[i - 1]) > String(okDefault[i])) {
                fail("post-throw default sort out of order");
            }
        }

        var okCmp = new Array(800);
        for (var i = 0; i < okCmp.length; i++) {
            okCmp[i] = { v: (i * 313) % 809 };
        }
        okCmp.sort(function (x, y) { return x.v - y.v; });
        for (var i = 1; i < okCmp.length; i++) {
            if (okCmp[i - 1].v > okCmp[i].v) {
                fail("post-throw comparator sort out of order");
            }
        }
        gcNow();
    }
}

function runGCAndThrowSuite() {
    testDefaultSortFreshToStringWithGC();
    testComparatorMergeSortWithGC();
    testThrowRecovery();
}

testLargeDoubleDefaultSort();
runGCAndThrowSuite();
runGCAndThrowSuite();

print("PASS");
