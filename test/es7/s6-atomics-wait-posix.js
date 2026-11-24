//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// R-3 + R-6 regression: POSIX Atomics.wait suspension, notify waking, finite
// timeouts (0, sub-ms, normal, >2^31ms huge doubles), FIFO count waking, and
// timeout-vs-notify linearizability conservation. Runs with -ESSharedArrayBuffer
// -ESBigInt -Test262.

var failures = [];
function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("s6-atomics-wait-posix: FAIL: " + msg);
    }
}

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

// 1. Immediate non-blocking paths (Int32 and BigInt64) + CollectGarbage stress (R-6)
var sab1 = new SharedArrayBuffer(16);
var i32 = new Int32Array(sab1);
var b64 = new BigInt64Array(sab1);
check(Atomics.wait(i32, 0, 1, Infinity) === "not-equal", "1: i32 not-equal");
check(Atomics.wait(b64, 0, 1n, Infinity) === "not-equal", "1: b64 not-equal");
check(Atomics.wait(i32, 0, 0, 0) === "timed-out", "1: 0ms timeout");
check(Atomics.wait(i32, 0, 0, -0) === "timed-out", "1: -0ms timeout");
check(Atomics.wait(i32, 0, 0, -10) === "timed-out", "1: negative timeout");
check(Atomics.wait(i32, 0, 0, -Infinity) === "timed-out", "1: -Infinity timeout");
check(Atomics.wait(i32, 0, 0, 0.1) === "timed-out", "1: sub-ms timeout 0.1");
check(Atomics.wait(i32, 0, 0, 0.5) === "timed-out", "1: sub-ms timeout 0.5");

// 2. Real finite timed suspension (40ms) actually waits rather than returning instantly.
var t0 = Date.now();
var rTimed = Atomics.wait(i32, 0, 0, 40);
var dt = Date.now() - t0;
check(rTimed === "timed-out", "2: 40ms wait returned timed-out, got " + rTimed);
check(dt >= 25, "2: 40ms wait suspended for >=25ms, elapsed=" + dt + "ms");

// 3. Cross-agent notify wakes worker blocked in infinite Atomics.wait (Int32 + BigInt64).
var sab3 = new SharedArrayBuffer(32);
var i32_3 = new Int32Array(sab3);
var b64_3 = new BigInt64Array(sab3);
$262.agent.start(`
    $262.agent.receiveBroadcast(function (sab) {
        var v = new Int32Array(sab);
        var b = new BigInt64Array(sab);
        if (Atomics.load(v, 7) !== 1) return;
        Atomics.store(v, 2, 1);
        var res1 = Atomics.wait(v, 0, 0, Infinity);
        $262.agent.report("3a:" + res1);
        Atomics.store(v, 3, 1);
        var res2 = Atomics.wait(b, 2, 0n); // undefined timeout => +Infinity (bytes 16..23)
        $262.agent.report("3b:" + res2);
        $262.agent.leaving();
    });
`);
Atomics.store(i32_3, 7, 1);
$262.agent.broadcast(sab3);
while (Atomics.load(i32_3, 2) !== 1) {}
while (Atomics.notify(i32_3, 0, 1) === 0) { $262.agent.sleep(1); }
check(getReport() === "3a:ok", "3a: worker i32 infinite wait woken");
while (Atomics.load(i32_3, 3) !== 1) {}
while (Atomics.notify(b64_3, 2, 1) === 0) { $262.agent.sleep(1); }
check(getReport() === "3b:ok", "3b: worker b64 undefined timeout wait woken");

// 4. Parent suspended with Infinity, 3e9 (>= 2^31 ms), and 1e15 woken by worker.
//    Worker waits >=15ms after parent sets flag to prove parent actually blocks on >2^31 timeouts.
var sab4 = new SharedArrayBuffer(24);
var i32_4 = new Int32Array(sab4);
$262.agent.start(`
    $262.agent.receiveBroadcast(function (sab) {
        var v = new Int32Array(sab);
        if (Atomics.load(v, 5) !== 2) return;
        for (var phase = 1; phase <= 3; phase++) {
            while (Atomics.load(v, 1) !== phase) {}
            WScript.Sleep(15);
            var n = Atomics.notify(v, 0, 1);
            $262.agent.report("4." + phase + ":notify=" + n);
        }
        $262.agent.leaving();
    });
`);
Atomics.store(i32_4, 5, 2);
$262.agent.broadcast(sab4);

Atomics.store(i32_4, 1, 1);
check(Atomics.wait(i32_4, 0, 0, Infinity) === "ok", "4.1: parent Infinity wait woken");
check(getReport() === "4.1:notify=1", "4.1: worker notified 1");

Atomics.store(i32_4, 1, 2);
check(Atomics.wait(i32_4, 0, 0, 3e9) === "ok", "4.2: parent 3e9ms (>= 2^31) wait woken");
check(getReport() === "4.2:notify=1", "4.2: worker notified 1 for 3e9ms");

Atomics.store(i32_4, 1, 3);
check(Atomics.wait(i32_4, 0, 0, 1e15) === "ok", "4.3: parent 1e15ms huge wait woken");
check(getReport() === "4.3:notify=1", "4.3: worker notified 1 for 1e15ms");

// 5. FIFO wake order & exact count across 3 blocked workers.
var sab5 = new SharedArrayBuffer(32);
var i32_5 = new Int32Array(sab5);
function fifoAgent(id) {
    return `
        $262.agent.receiveBroadcast(function (sab) {
            var v = new Int32Array(sab);
            if (Atomics.load(v, 7) !== 3) return;
            while (Atomics.load(v, 6) !== ${id}) {}
            Atomics.store(v, 1 + ${id}, 1);
            var r = Atomics.wait(v, 0, 0, Infinity);
            $262.agent.report("5:${id}:" + r);
            $262.agent.leaving();
        });
    `;
}
$262.agent.start(fifoAgent(0));
$262.agent.start(fifoAgent(1));
$262.agent.start(fifoAgent(2));
Atomics.store(i32_5, 7, 3);
$262.agent.broadcast(sab5);

for (var id = 0; id < 3; id++) {
    Atomics.store(i32_5, 6, id);
    while (Atomics.load(i32_5, 1 + id) !== 1) {}
    $262.agent.sleep(20); // allow worker `id` to enter WaiterList before launching next
}
var wokenFirstTwo = Atomics.notify(i32_5, 0, 2);
check(wokenFirstTwo === 2, "5: notify(2) wakes 2, got " + wokenFirstTwo);
var repA = getReport();
var repB = getReport();
var wokenSet = [repA, repB].sort().join(",");
check(wokenSet === "5:0:ok,5:1:ok", "5: FIFO woke workers 0 and 1 first, got " + wokenSet);
check($262.agent.getReport() === null, "5: worker 2 must still be blocked");
check(Atomics.notify(i32_5, 0, 1) === 1, "5: notify(1) wakes remaining worker");
check(getReport() === "5:2:ok", "5: worker 2 woke last");

// 6. Timeout-vs-notify linearizability conservation test: sum(notify) === count("ok").
var rounds = 40;
var sab6 = new SharedArrayBuffer(16);
var i32_6 = new Int32Array(sab6);
$262.agent.start(`
    $262.agent.receiveBroadcast(function (sab) {
        var v = new Int32Array(sab);
        if (Atomics.load(v, 3) !== 4) return;
        var sum = 0;
        for (var r = 1; r <= ${rounds}; r++) {
            while (Atomics.load(v, 1) !== r) {}
            WScript.Sleep(1);
            sum += Atomics.notify(v, 0, 1);
            Atomics.store(v, 2, r);
        }
        $262.agent.report("6:" + sum);
        $262.agent.leaving();
    });
`);
Atomics.store(i32_6, 3, 4);
$262.agent.broadcast(sab6);
var okCount = 0;
var toCount = 0;
for (var r = 1; r <= rounds; r++) {
    Atomics.store(i32_6, 1, r);
    var res = Atomics.wait(i32_6, 0, 0, 1);
    if (res === "ok") okCount++;
    else if (res === "timed-out") toCount++;
    else check(false, "6: unexpected result " + res);
    while (Atomics.load(i32_6, 2) !== r) {}
}
var notifyRep = getReport();
var workerSum = parseInt(notifyRep.split(":")[1], 10);
check(okCount + toCount === rounds, "6: all rounds settled");
check(workerSum === okCount, "6: conservation invariant notifySum=" + workerSum + " === okCount=" + okCount);

if (failures.length === 0) {
    print("PASS");
} else {
    throw new Error("Failures: " + failures.join("; "));
}
