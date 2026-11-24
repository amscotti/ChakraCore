//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// R-4 regression: settled Atomics.waitAsync with a long finite timeout cancels
// its host timer immediately upon Atomics.notify so ch exits immediately
// instead of waiting for the timeout deadline. Runs with -ESSharedArrayBuffer.

var failures = [];
function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("s6-waitasync-settled-exit: FAIL: " + msg);
    }
}

var i32 = new Int32Array(new SharedArrayBuffer(16));
var r1 = Atomics.waitAsync(i32, 0, 0, 1500);
check(r1.async === true, "waitAsync returns async result");

var resolved = "";
r1.value.then(function (v) {
    resolved = v;
});

var notified = Atomics.notify(i32, 0, 1);
check(notified === 1, "notify returns 1");

WScript.SetTimeout(function () {
    check(resolved === "ok", "resolved value is ok, got " + resolved);
    if (failures.length === 0) {
        print("PASS");
    } else {
        throw new Error("Failures: " + failures.join("; "));
    }
}, 0);
