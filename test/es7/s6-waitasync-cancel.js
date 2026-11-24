//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// R-4 + R-6 regression: Atomics.waitAsync settled timer cancellation (Int32,
// BigInt64, cross-realm, cross-agent), pending short timeout preservation,
// and R-6 lock-order / GC cleanup stress. Runs with -ESSharedArrayBuffer
// -ESBigInt -Test262.

var startMs = Date.now();
var failures = [];
function check(cond, msg) {
    if (!cond) {
        failures.push(msg);
        print("s6-waitasync-cancel: FAIL: " + msg);
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

function waitUntil(view, index, expected) {
    for (var i = 0; i < 50000000; i++) {
        if (Atomics.load(view, index) === expected) {
            return true;
        }
    }
    check(false, "waitUntil(" + index + ", " + expected + ") timed out");
    return false;
}

// 1. Int32 same-thread settled 1500ms waitAsync cancels timer immediately.
var sab1 = new SharedArrayBuffer(16);
var i32 = new Int32Array(sab1);
var r1 = Atomics.waitAsync(i32, 0, 0, 1500);
check(r1.async === true, "1: async true");
var out1 = "";
r1.value.then(function (v) { out1 = v; });
check(Atomics.notify(i32, 0, 1) === 1, "1: notify returns 1");
check(Atomics.notify(i32, 0, 1) === 0, "1: second notify returns 0");

// 2. BigInt64 same-thread settled 1500ms waitAsync cancels timer immediately.
var sab2 = new SharedArrayBuffer(16);
var b64 = new BigInt64Array(sab2);
var r2 = Atomics.waitAsync(b64, 0, 0n, 1500);
check(r2.async === true, "2: async true");
var out2 = "";
r2.value.then(function (v) { out2 = v; });
check(Atomics.notify(b64, 0, 1) === 1, "2: notify returns 1");

// 3. Cross-realm waitAsync with 1500ms timeout notified from main realm.
var other = WScript.LoadScript("function startWait(sab) { var a = new Int32Array(sab); return Atomics.waitAsync(a, 0, 0, 1500); }", "samethread");
var sab3 = new SharedArrayBuffer(16);
var i32_3 = new Int32Array(sab3);
var r3 = other.startWait(sab3);
check(r3.async === true, "3: cross-realm async true");
var out3 = "";
r3.value.then(function (v) { out3 = v; });
check(Atomics.notify(i32_3, 0, 1) === 1, "3: cross-realm notify returns 1");

// 4. R-6: not-equal & 0ms returns + CollectGarbage finalization with other live SABs.
for (var k = 0; k < 200; k++) {
    var tmpSab = new SharedArrayBuffer(8);
    var tmpView = new Int32Array(tmpSab);
    Atomics.waitAsync(tmpView, 0, 0, Infinity); // unrooted once tmpSab overwritten
    var ne = Atomics.waitAsync(i32, 0, 99, 1500);
    check(ne.async === false && ne.value === "not-equal", "4: not-equal");
    var z = Atomics.waitAsync(i32, 0, 0, 0);
    check(z.async === false && z.value === "timed-out", "4: 0 timeout");
}
if (typeof CollectGarbage === "function") {
    CollectGarbage();
}

// 5. Cross-agent notify wakes parent's 1500ms waitAsync and cancels its 1500ms timer.
var sab5 = new SharedArrayBuffer(16);
var i32_5 = new Int32Array(sab5);
var out5 = "";
$262.agent.start(`
    $262.agent.receiveBroadcast(function (sab) {
        var v = new Int32Array(sab);
        while (Atomics.load(v, 1) !== 1) {}
        WScript.Sleep(5);
        $262.agent.report("5:notify=" + Atomics.notify(v, 0, 1));
        $262.agent.leaving();
    });
`);
$262.agent.broadcast(sab5);
var r5 = Atomics.waitAsync(i32_5, 0, 0, 1500);
check(r5.async === true, "5: async true");
r5.value.then(function (v) { out5 = v; });
Atomics.store(i32_5, 1, 1);
check(getReport() === "5:notify=1", "5: agent notified parent");

// 6. Pending short finite timeout still fires and resolves "timed-out" (preserving Test262).
var sab6 = new SharedArrayBuffer(16);
var i32_6 = new Int32Array(sab6);
var out6 = "pending";
var r6 = Atomics.waitAsync(i32_6, 0, 0, 1);
check(r6.async === true, "6: async true");
r6.value.then(function (v) { out6 = v; });

WScript.SetTimeout(function () {
    check(out1 === "ok", "1: resolved ok, got " + out1);
    check(out2 === "ok", "2: resolved ok, got " + out2);
    check(out3 === "ok", "3: cross-realm resolved ok, got " + out3);
    check(out5 === "ok", "5: cross-agent woken resolved ok, got " + out5);
    check(out6 === "timed-out", "6: 1ms pending resolved timed-out, got " + out6);

    var elapsed = Date.now() - startMs;
    check(elapsed < 750, "settled 1500ms timers must cancel without waiting 1500ms (elapsed=" + elapsed + "ms)");

    // Drop references to settled SABs and trigger GC to prove host timer roots were released.
    sab1 = null; i32 = null; r1 = null;
    sab2 = null; b64 = null; r2 = null;
    sab3 = null; i32_3 = null; r3 = null;
    sab5 = null; i32_5 = null; r5 = null;
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }

    if (failures.length === 0) {
        print("PASS");
    } else {
        throw new Error("Failures: " + failures.join("; "));
    }
}, 40);
