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

// --- 9. Multi-waiter partial notify: 3 agents x 2 waiters + main -----------
var sab9 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a9 = new Int32Array(sab9);
var src9 = `
    $262.agent.receiveBroadcast(function(sab) {
        var v = new Int32Array(sab);
        var p1 = Atomics.waitAsync(v, 0, 0).value;
        var p2 = Atomics.waitAsync(v, 0, 0).value;
        Atomics.add(v, 4, 1);
        Promise.all([p1, p2]).then(function(vals) {
            $262.agent.report("9:" + vals[0] + ":" + vals[1]);
            $262.agent.leaving();
        });
    });
`;
$262.agent.start(src9);
$262.agent.start(src9);
$262.agent.start(src9);
$262.agent.broadcast(sab9);
waitUntil(i32a9, 4, 3);
var main9Outcome = "pending";
var main9 = Atomics.waitAsync(i32a9, 0, 0);
main9.value.then(function(v) { main9Outcome = v; });
check(Atomics.notify(i32a9, 0, 4) === 4, "partial notify wakes exactly 4 of the 7 waiters");
check(Atomics.notify(i32a9, 0) === 3, "notify rest wakes the remaining 3");
for (var i = 0; i < 3; i++) {
    var report9 = getReport();
    check(report9 === "9:ok:ok", "agent waiter pair both resolved ok, got " + report9);
}
check(Atomics.notify(i32a9, 0) === 0, "all 7 waiters unregistered after both notifies");

// --- 10. Repeated broadcasts to the same three agents ----------------------
// Tag cell v[3] carries the round (1..3); after round 3 the agents leave,
// and they ignore later broadcasts with other tags (scenario 11 reuses the
// broadcast machinery with fresh buffers whose tag cell stays 0).
var src10 = `
    $262.agent.receiveBroadcast(function(sab) {
        var v = new Int32Array(sab);
        var tag = Atomics.load(v, 3);
        if (tag < 1 || tag > 3) { return; }
        var p = Atomics.waitAsync(v, 0, 0).value;
        Atomics.add(v, 4, 1);
        p.then(function(val) {
            $262.agent.report("10:" + val);
            if (tag === 3) { $262.agent.leaving(); }
        });
    });
`;
$262.agent.start(src10);
$262.agent.start(src10);
$262.agent.start(src10);
for (var round = 1; round <= 3; round++) {
    var sabR = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
    var i32aR = new Int32Array(sabR);
    Atomics.store(i32aR, 3, round);
    $262.agent.broadcast(sabR);
    waitUntil(i32aR, 4, 3);
    check(Atomics.notify(i32aR, 0, 2) === 2, "round " + round + ": partial notify wakes 2 of 3");
    check(Atomics.notify(i32aR, 0) === 1, "round " + round + ": notify rest wakes the last one");
    for (var i = 0; i < 3; i++) {
        var reportR = getReport();
        check(reportR === "10:ok", "round " + round + " agent resolved ok, got " + reportR);
    }
}
$262.agent.sleep(150); // let the round-3 agents finish leaving

// --- 11. Pre-share waiters: registered before the first broadcast ----------
// A waitAsync registered while the buffer is still single-runtime has no
// monitor yet; the first broadcast must retroactively monitor it so the
// agent's foreign notify counts (returns 1) and resolves "ok".
var sab11 = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a11 = new Int32Array(sab11);
var preShareOutcome = "pending";
var preShareBigOutcome = "pending";
var preShareTimeoutOutcome = "pending";
var pre11 = Atomics.waitAsync(i32a11, 0, 0);
pre11.value.then(function(v) { preShareOutcome = v; });
var sab11b = new SharedArrayBuffer(5 * BigInt64Array.BYTES_PER_ELEMENT);
var b64a11 = new BigInt64Array(sab11b);
var preBig11 = Atomics.waitAsync(b64a11, 0, 0n);
preBig11.value.then(function(v) { preShareBigOutcome = v; });
var sab11c = new SharedArrayBuffer(5 * Int32Array.BYTES_PER_ELEMENT);
var i32a11c = new Int32Array(sab11c);
var preTimeout11 = Atomics.waitAsync(i32a11c, 0, 0, 300);
preTimeout11.value.then(function(v) { preShareTimeoutOutcome = v; });

$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var v = new Int32Array(sab);
        $262.agent.report("11a:notify=" + Atomics.notify(v, 0));
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab11);
var report11a = getReport();
check(report11a === "11a:notify=1", "pre-share Int32 waiter: agent notify counts 1, got " + report11a);

$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var v = new BigInt64Array(sab);
        $262.agent.report("11b:notify=" + Atomics.notify(v, 0));
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab11b);
var report11b = getReport();
check(report11b === "11b:notify=1", "pre-share BigInt64 waiter: agent notify counts 1, got " + report11b);

// Timeout-before-share control: the finite waiter is registered pre-share,
// never notified; after the share nobody notifies it either — its 300 ms
// timeout must still win (retro-monitor must not reschedule it forever).
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var v = new Int32Array(sab);
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab11c);
$262.agent.sleep(450); // timeout fires well before the final callback

// Post-share control: a waiter registered after the share keeps working.
var post11 = Atomics.waitAsync(i32a11, 2, 0);
var post11Outcome = "pending";
post11.value.then(function(v) { post11Outcome = v; });
$262.agent.start(`
    $262.agent.receiveBroadcast(function(sab) {
        var v = new Int32Array(sab);
        $262.agent.report("11c:notify=" + Atomics.notify(v, 2));
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab11);
var report11c = getReport();
check(report11c === "11c:notify=1", "post-share control waiter: notify counts 1, got " + report11c);

WScript.SetTimeout(function() {
    try {
        check(parentResult.async === true, "parent waitAsync result is async");
        check(parentOutcome === "ok", "main-thread waiter resolved ok via its monitor, got " + parentOutcome);
        check(Atomics.notify(i32a8, 0) === 0, "resolved main-thread waiter is unregistered");
        check(main9Outcome === "ok", "scenario 9 main waiter resolved ok, got " + main9Outcome);
        check(preShareOutcome === "ok", "pre-share Int32 waiter resolved ok, got " + preShareOutcome);
        check(preShareBigOutcome === "ok", "pre-share BigInt64 waiter resolved ok, got " + preShareBigOutcome);
        check(preShareTimeoutOutcome === "timed-out", "pre-share finite waiter timed out, got " + preShareTimeoutOutcome);
        check(post11Outcome === "ok", "post-share control waiter resolved ok, got " + post11Outcome);
        check(Atomics.notify(i32a11, 0) === 0, "pre-share waiter unregistered after resolution");
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
