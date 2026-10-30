//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S6e: real $262.agent (crossthread) Atomics.waitAsync. Requires -Test262
// (for $262.agent) plus -ESSharedArrayBuffer; the BigInt64 scenarios require
// -ESBigInt (doc/c2-s6-cross-agent-waitasync.md).
//
// Architecture under test: a foreign (other-ThreadContext) Atomics.notify
// removes the waiter's native token from the shared waiter list, counts it,
// and atomically sets the token's wake-request latch; the registrant's
// host-delayed monitor task claims, unlinks, releases the token and resolves
// "ok" on the agent thread. The child agent runtime has its own job/timer
// queue, so async receiveBroadcast functions suspend at await and resume from
// drained promise jobs. Finite timeouts and monitors arbitrate through the
// settled flag: whoever fires first wins exactly once, the late side no-ops,
// and a resolved waiter is unregistered (late notifies count 0).

var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("s6-waitasync-cross-agent: FAIL: " + message);
    }
}

// Raw host report helpers (no test262 harness includes here).
function getReport() {
    for (var i = 0; i < 10000; i++) {
        var r = $262.agent.getReport();
        if (r !== null) {
            return r;
        }
        $262.agent.sleep(1);
    }
    return null;
}

function waitUntil(view, index, expected) {
    for (var i = 0; i < 50000000; i++) {
        if (Atomics.load(view, index) === expected) {
            return true;
        }
    }
    check(false, "waitUntil(" + index + ", " + expected + ") timed out");
    return false;
}

try {

// --- 1. Int32: parent notify wakes the agent's promise ----------------------
var sab1 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a1 = new Int32Array(sab1);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var r = Atomics.waitAsync(i32a, 0, 0);
        Atomics.add(i32a, 4, 1); // signal: waiter registered
        $262.agent.report("1:async=" + r.async);
        r.value.then(function(v) {
            $262.agent.report("1:" + v);
            $262.agent.leaving();
        });
    });
`);
$262.agent.broadcast(sab1);
waitUntil(i32a1, 4, 1);
check(Atomics.notify(i32a1, 0) === 1, "int32 foreign notify counts the agent waiter");
check(getReport() === "1:async=true", "agent waitAsync result is async");
var report1 = getReport();
check(report1 === "1:ok", "agent promise resolved to ok after parent notify, got " + report1);
check(Atomics.notify(i32a1, 0) === 0, "resolved foreign waiter is unregistered");

// --- 2. BigInt64: same flow with a BigInt64Array view ------------------------
var sab2 = new SharedArrayBuffer(5 * BigInt64Array.BYTES_PER_ELEMENT);
var b64a2 = new BigInt64Array(sab2);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var b64 = new BigInt64Array(sab);
        var r = Atomics.waitAsync(b64, 0, 0n);
        Atomics.add(b64, 4, 1n);
        r.value.then(function(v) {
            $262.agent.report("2:" + v);
            $262.agent.leaving();
        });
    });
`);
$262.agent.broadcast(sab2);
waitUntil(b64a2, 4, 1n);
check(Atomics.notify(b64a2, 0) === 1, "bigint64 foreign notify counts the agent waiter");
var report2 = getReport();
check(report2 === "2:ok", "bigint64 agent promise resolved to ok, got " + report2);

// --- 3. Timeout before foreign notify ---------------------------------------
var sab3 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a3 = new Int32Array(sab3);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var r = Atomics.waitAsync(i32a, 0, 0, 20);
        Atomics.add(i32a, 4, 1);
        r.value.then(function(v) {
            $262.agent.report("3:" + v);
            $262.agent.leaving();
        });
    });
`);
$262.agent.broadcast(sab3);
waitUntil(i32a3, 4, 1);
$262.agent.sleep(100); // let the 20ms timeout win
check(Atomics.notify(i32a3, 0) === 0, "timed-out foreign waiter is unregistered before late notify");
var report3 = getReport();
check(report3 === "3:timed-out", "agent finite timeout resolved timed-out, got " + report3);

// --- 4. Notify wins over a pending finite timeout ----------------------------
var sab4 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a4 = new Int32Array(sab4);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var r = Atomics.waitAsync(i32a, 0, 0, 500);
        Atomics.add(i32a, 4, 1);
        r.value.then(function(v) {
            $262.agent.report("4:" + v);
            $262.agent.leaving();
        });
    });
`);
$262.agent.broadcast(sab4);
waitUntil(i32a4, 4, 1);
check(Atomics.notify(i32a4, 0) === 1, "notify counts the waiter before its 500ms timeout");
var report4 = getReport();
check(report4 === "4:ok", "notify beat the timeout: resolved ok, got " + report4);
check(Atomics.notify(i32a4, 0) === 0, "notified waiter unregistered; late timeout task no-ops");

// --- 5. Multiple agents on separate indexes ----------------------------------
var sab5 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a5 = new Int32Array(sab5);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var r = Atomics.waitAsync(i32a, 0, 0);
        Atomics.add(i32a, 4, 1);
        r.value.then(function(v) { $262.agent.report("5A@" + 0 + ":" + v); $262.agent.leaving(); });
    });
`);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var r = Atomics.waitAsync(i32a, 2, 0);
        Atomics.add(i32a, 4, 1);
        r.value.then(function(v) { $262.agent.report("5B@" + 2 + ":" + v); $262.agent.leaving(); });
    });
`);
$262.agent.broadcast(sab5);
waitUntil(i32a5, 4, 2);
check(Atomics.notify(i32a5, 1) === 0, "notify on an unwaited index counts 0");
check(Atomics.notify(i32a5, 2) === 1, "notify on agent B's index counts 1");
var report5b = getReport();
check(report5b === "5B@2:ok", "agent B resolved ok, got " + report5b);
check(Atomics.notify(i32a5, 0) === 1, "notify on agent A's index counts 1");
var report5a = getReport();
check(report5a === "5A@0:ok", "agent A resolved ok, got " + report5a);

// --- 6. FIFO resolution order within one agent -------------------------------
var sab6 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a6 = new Int32Array(sab6);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        var order = [];
        var p1 = Atomics.waitAsync(i32a, 0, 0).value;
        var p2 = Atomics.waitAsync(i32a, 0, 0).value;
        p1.then(function() { order.push("A"); });
        p2.then(function() { order.push("B"); });
        Atomics.add(i32a, 4, 1);
        Promise.all([p1, p2]).then(function() {
            $262.agent.report("6:" + order.join(","));
            $262.agent.leaving();
        });
    });
`);
$262.agent.broadcast(sab6);
waitUntil(i32a6, 4, 1);
if (typeof CollectGarbage === "function") {
    CollectGarbage(); // both waiters pending: the queued tasks must root them
}
check(Atomics.notify(i32a6, 0, 2) === 2, "notify counts both registered foreign waiters");
var report6 = getReport();
check(report6 === "6:A,B", "FIFO resolution order A,B, got " + report6);

// --- 7. Async receiveBroadcast: job draining across awaits -------------------
var sab7 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a7 = new Int32Array(sab7);
$262.agent.start(`
    $262.agent.receiveBroadcast(async function(sab) {
        var i32a = new Int32Array(sab);
        var pre = await Promise.resolve(7);
        var r = Atomics.waitAsync(i32a, 0, 0);
        Atomics.add(i32a, 4, 1);
        var v = await r.value;
        $262.agent.report("7:" + pre + ":" + v);
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab7);
waitUntil(i32a7, 4, 1);
check(Atomics.notify(i32a7, 0) === 1, "notify counts the async-function waiter");
var report7 = getReport();
check(report7 === "7:7:ok", "async receiveBroadcast drained across awaits: 7:ok, got " + report7);

// --- 8. Reverse direction: main-thread waiter, agent notifies ----------------
var sab8 = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
var i32a8 = new Int32Array(sab8);
var parentOutcome = "pending";
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var i32a = new Int32Array(sab);
        Atomics.add(i32a, 1, 1);
        while (Atomics.load(i32a, 2) === 0) { }
        $262.agent.report("8:notify=" + Atomics.notify(i32a, 0));
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab8);
waitUntil(i32a8, 1, 1);
var parentResult = Atomics.waitAsync(i32a8, 0, 0);
parentResult.value.then(function(v) { parentOutcome = v; });
Atomics.store(i32a8, 2, 1); // release the agent: it notifies our waiter
var report8 = getReport();
check(report8 === "8:notify=1", "agent's notify of the main-thread waiter counts 1, got " + report8);

WScript.SetTimeout(function() {
    try {
        check(parentResult.async === true, "parent waitAsync result is async");
        check(parentOutcome === "ok", "main-thread waiter resolved ok via its monitor, got " + parentOutcome);
        check(Atomics.notify(i32a8, 0) === 0, "resolved main-thread waiter is unregistered");
        if (failures.length) {
            print("s6-waitasync-cross-agent: " + failures.length + " failure(s)");
            WScript.Quit(1);
        }
        print("pass");
        WScript.Quit(0);
    } catch (e) {
        print("s6-waitasync-cross-agent: unexpected failure: " + (e.stack || e));
        WScript.Quit(1);
    }
}, 150);

} catch (e) {
    print("s6-waitasync-cross-agent: unexpected failure: " + (e.stack || e));
    WScript.Quit(1);
}
