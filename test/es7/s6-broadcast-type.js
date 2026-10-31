//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S6e host-robustness: WScript.Broadcast must reject a non-SharedArrayBuffer
// argument with a script exception BEFORE signaling any agent child.
// Pre-fix (100% repro, 15/15 SIGSEGV both profiles): the unchecked
// JsGetSharedArrayBufferContent failure left a stale/null content handle
// that live children consumed and crashed on — doc/c2-s6-cross-agent-
// waitasync.md, "Follow-up: host-broadcast-type". $262.agent.broadcast IS
// WScript.Broadcast (bin/ch/262.js), so both script entry points are
// covered. Requires -Test262 (for $262.agent) plus -ESSharedArrayBuffer.

var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("s6-broadcast-type: FAIL: " + message);
    }
}

try {

// One agent; its receiveBroadcast registration completes before
// $262.agent.start returns (the spawn waits for the child's initial
// script), so no sleep is needed for determinism.
var sab = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
var i32 = new Int32Array(sab);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(s) {
        var v = new Int32Array(s);
        Atomics.add(v, 0, 1); // signal: broadcast consumed
        $262.agent.report("got " + v[0]);
        $262.agent.leaving();
    });
`);

// --- Negative: a missing argument throws the same rejection ---------------
var threwNoArg = null;
try {
    WScript.Broadcast();
} catch (e) {
    threwNoArg = e;
}
check(threwNoArg !== null, "broadcast with no argument threw, got " + threwNoArg);
if (threwNoArg !== null) {
    check(/SharedArrayBuffer/.test(String(threwNoArg.message || threwNoArg)),
        "no-argument rejection message names SharedArrayBuffer, got " + threwNoArg.message);
}

// --- Negative: non-SAB arguments throw, and the agent is untouched --------
var badArgs = [i32, 123, "sab", {}, null, undefined];
for (var i = 0; i < badArgs.length; i++) {
    var threw = null;
    try {
        WScript.Broadcast(badArgs[i]);
    } catch (e) {
        threw = e;
    }
    check(threw !== null, "broadcast of badArgs[" + i + "] threw, got " + threw);
    if (threw !== null) {
        check(threw instanceof Error, "broadcast rejection " + i + " is a script Error");
        check(/SharedArrayBuffer/.test(String(threw.message || threw)),
            "broadcast rejection " + i + " message names SharedArrayBuffer, got " + threw.message);
    }
}

// The rejected broadcasts must not have signaled the child: nothing has
// consumed the SAB, so the agent is still parked with v[0] untouched.
check(Atomics.load(i32, 0) === 0, "no rejected broadcast reached the agent");

// --- Positive: a subsequent VALID broadcast still works with agents live --
$262.agent.broadcast(sab);
var consumed = false;
for (var j = 0; j < 50000000; j++) {
    if (Atomics.load(i32, 0) === 1) { consumed = true; break; }
}
check(consumed, "valid broadcast after rejections reaches the live agent");

var report = null;
for (var k = 0; k < 10000; k++) {
    report = $262.agent.getReport();
    if (report !== null) break;
    $262.agent.sleep(1);
}
check(report === "got 1", "agent consumed the valid broadcast, got " + report);

if (failures.length) {
    print("s6-broadcast-type: " + failures.length + " failure(s)");
    WScript.Quit(1);
}
print("pass");
WScript.Quit(0);

} catch (e) {
    print("s6-broadcast-type: unexpected failure: " + (e.stack || e));
    WScript.Quit(1);
}
