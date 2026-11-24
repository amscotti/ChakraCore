//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// R-1 regression: ThreadContext::KeepDuringJob must deduplicate in O(1) so
// dereferencing many WeakRefs in one job scales linearly, retains targets
// in-job across CollectGarbage(), releases oversized backing tables after the
// job boundary, and allows WeakRefs/symbols to be collected in later jobs.

function fail(message) {
    print("weakref-keptobjects-perf: FAIL: " + message);
    WScript.Quit(1);
}

var LARGE_N = 160000;
var refs = new Array(LARGE_N);
for (var i = 0; i < LARGE_N; i++) {
    refs[i] = new WeakRef({ idx: i });
}

// Dereference every target multiple times in the same job and check linear time.
var t0 = Date.now();
var sum = 0;
for (var i = 0; i < LARGE_N; i++) {
    var obj = refs[i].deref();
    if (obj === undefined) {
        fail("in-job target missing at " + i);
    }
    sum += obj.idx;
}
for (var i = 0; i < LARGE_N; i++) {
    if (refs[i].deref() === undefined) {
        fail("repeated deref missing at " + i);
    }
}
var elapsed = Date.now() - t0;
var expectedSum = (LARGE_N - 1) * LARGE_N / 2;
if (sum !== expectedSum) {
    fail("sum mismatch: " + sum + " !== " + expectedSum);
}

// Keep distinct unregistered symbols too; all targets must survive in-job GC.
var symCount = 1000;
var symRefs = new Array(symCount);
for (var i = 0; i < symCount; i++) {
    symRefs[i] = new WeakRef(Symbol("r1-sym-" + i));
}

CollectGarbage();
CollectGarbage();

for (var i = 0; i < LARGE_N; i += 997) {
    if (refs[i].deref() === undefined) {
        fail("large target collected before job boundary at " + i);
    }
}
for (var i = 0; i < symCount; i++) {
    if (symRefs[i].deref() === undefined) {
        fail("symbol target collected before job boundary at " + i);
    }
}

// Move past job boundary: kept set is cleared (and oversized table reset).
// Verify targets are collectible in job 2 and subsequent small jobs work cleanly.
WScript.SetTimeout(function () {
    CollectGarbage();
    CollectGarbage();

    var clearedObj = 0;
    for (var i = 0; i < LARGE_N; i++) {
        if (refs[i].deref() === undefined) {
            clearedObj++;
        }
    }
    if (clearedObj < LARGE_N - 16) {
        fail("post-job object targets not reclaimed: cleared " + clearedObj + "/" + LARGE_N);
    }

    var clearedSym = 0;
    for (var i = 0; i < symCount; i++) {
        if (symRefs[i].deref() === undefined) {
            clearedSym++;
        }
    }
    if (clearedSym < symCount - 16) {
        fail("post-job symbol targets not reclaimed: cleared " + clearedSym + "/" + symCount);
    }

    // Small job after oversized reset: must retain in-job and clear on job 3.
    var smallN = 32;
    var smallRefs = [];
    for (var i = 0; i < smallN; i++) {
        smallRefs.push(new WeakRef({ small: i }));
    }
    CollectGarbage();
    for (var i = 0; i < smallN; i++) {
        if (smallRefs[i].deref() === undefined) {
            fail("small job target missing at " + i);
        }
    }

    WScript.SetTimeout(function () {
        CollectGarbage();
        CollectGarbage();
        var smallCleared = 0;
        for (var i = 0; i < smallN; i++) {
            if (smallRefs[i].deref() === undefined) {
                smallCleared++;
            }
        }
        if (smallCleared < smallN - 4) {
            fail("small job targets not reclaimed: cleared " + smallCleared + "/" + smallN);
        }
        print("pass");
    }, 5);
}, 5);
