//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Cross-job WeakRef GC test. The initial script is job 1; WScript.SetTimeout
// runs the callback as job 2 through a separate host call. testRunner's
// synchronous summary cannot observe a job-boundary failure, so this file
// exits nonzero on failure instead of throwing from the timer callback.

var OBJECT_COUNT = 1000;
var SYMBOL_COUNT = 1000;
var WEAKMAP_COUNT = 500;

var objectRefs = [];
var symbolRefs = [];
var weakMapRefs = [];
var weakMap = new WeakMap();

(function createObjectRefs() {
    for (var i = 0; i < OBJECT_COUNT; i++) {
        objectRefs.push(new WeakRef({ index: i }));
    }
})();

(function createSymbolRefs() {
    for (var i = 0; i < SYMBOL_COUNT; i++) {
        symbolRefs.push(new WeakRef(Symbol("weakref-gc-" + i)));
    }
})();

(function createWeakMapRefs() {
    for (var i = 0; i < WEAKMAP_COUNT; i++) {
        var key = { index: i };
        weakMap.set(key, i);
        weakMapRefs.push(new WeakRef(key));
    }
})();

// These targets stay strongly reachable through job 1 so job 2 can exercise
// deref's own KeepDuringJob step after this array is dropped.
var derefTargets = [];
var derefRefs = [];
(function createDerefRefs() {
    for (var i = 0; i < OBJECT_COUNT; i++) {
        var target = { index: i };
        derefTargets.push(target);
        derefRefs.push(new WeakRef(target));
    }
})();

function countAlive(refs) {
    var alive = 0;
    for (var i = 0; i < refs.length; i++) {
        if (refs[i].deref() !== undefined) {
            alive++;
        }
    }
    return alive;
}

function countCleared(refs) {
    var cleared = 0;
    for (var i = 0; i < refs.length; i++) {
        if (refs[i].deref() === undefined) {
            cleared++;
        }
    }
    return cleared;
}

function derefAndDrop(ref) {
    ref.deref();
}

function fail(message) {
    print("weakref-gc: FAIL: " + message);
    WScript.Quit(1);
}

// Constructor KeepDuringJob: all targets must survive real in-job GC, not just
// a cleared > 0 remnant.
CollectGarbage();
CollectGarbage();
var objectsAlive = countAlive(objectRefs);
var symbolsAlive = countAlive(symbolRefs);
if (objectsAlive !== OBJECT_COUNT) {
    fail("in-job object targets alive " + objectsAlive + "/" + OBJECT_COUNT);
}
if (symbolsAlive !== SYMBOL_COUNT) {
    fail("in-job symbol targets alive " + symbolsAlive + "/" + SYMBOL_COUNT);
}

WScript.SetTimeout(function crossJobWeakRefCheck() {
    // New job: job 1's kept set has been cleared. First give deref something
    // to keep, then drop every strong target and collect.
    derefTargets = null;
    for (var i = 0; i < derefRefs.length; i++) {
        derefAndDrop(derefRefs[i]);
    }

    CollectGarbage();
    CollectGarbage();

    var derefAlive = countAlive(derefRefs);
    if (derefAlive !== OBJECT_COUNT) {
        fail("deref KeepDuringJob targets alive " + derefAlive + "/" + OBJECT_COUNT);
    }

    // Job 1 targets had only constructor KeepDuringJob retaining them. After
    // the job boundary they must be collectible; allow one conservative-stack
    // remnant but require the high-threshold control shape.
    var objectsCleared = countCleared(objectRefs);
    var symbolsCleared = countCleared(symbolRefs);
    var weakMapCleared = countCleared(weakMapRefs);

    if (objectsCleared < OBJECT_COUNT - 1) {
        fail("cross-job object targets cleared " + objectsCleared + "/" + OBJECT_COUNT);
    }
    if (symbolsCleared < SYMBOL_COUNT - 1) {
        fail("cross-job symbol targets cleared " + symbolsCleared + "/" + SYMBOL_COUNT);
    }
    if (weakMapCleared < WEAKMAP_COUNT - 1) {
        fail("cross-job WeakMap key targets cleared " + weakMapCleared + "/" + WEAKMAP_COUNT);
    }

    // The regression harness accepts only pass/passed output for a test
    // without a baseline. Counts are checked above; keep the success line
    // exactly "pass" and use the failure path's nonzero exit otherwise.
    print("pass");
}, 10);
