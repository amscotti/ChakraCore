//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// A cleanup callback that throws must leave the host job healthy and leave the
// registry's remaining dead cells eligible for a retry at a later boundary.
// The expected host error is recorded in finalization-registry-errors.js.baseline.

var TARGET_COUNT = 64;
var calls = 0;
var recovered = [];
var registry = new FinalizationRegistry(function (heldValue) {
    calls++;
    if (calls === 1) {
        throw new Error("expected cleanup callback failure");
    }
    recovered.push(heldValue);
});

(function registerTargets() {
    for (var i = 0; i < TARGET_COUNT; i++) {
        registry.register({ index: i }, "held-" + i);
    }
})();

CollectGarbage();
CollectGarbage();

var attempts = 0;
WScript.SetTimeout(function checkRetry() {
    attempts++;
    // Conservative stack scanning can retain a few targets for more than one
    // collection. Retry GC at later job boundaries rather than failing on a
    // single timing sample.
    CollectGarbage();
    if (calls < 2 && attempts < 20) {
        WScript.SetTimeout(checkRetry, 100);
        return;
    }
    if (calls < 2) {
        print("finalization-registry-errors: FAIL: no retry after callback throw: " + calls);
        WScript.Quit(1);
    }
    if (recovered.length !== calls - 1) {
        print("finalization-registry-errors: FAIL: remaining cells lost: " + calls + "/" + recovered.length);
        WScript.Quit(1);
    }
    print("pass");
}, 100);
