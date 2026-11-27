//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused regressions for GR-FE3 (WScript.SerializeObject / Deserialize),
// GR-FE4 (WScript.SetTimeout / ClearTimeout conversions), and
// GR-FE5 (MessageQueue microtask/timer ordering and scaling).

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertThrows(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
    }
    assert(threw, "Expected throw: " + msg);
}

// --- GR-FE3: WScript.SerializeObject / WScript.Deserialize ---
if (typeof WScript !== "undefined" && typeof WScript.SerializeObject === "function") {
    // 1. Basic round-trip.
    var serialized = WScript.SerializeObject({ a: 42, b: "hello", c: [1, 2, 3] });
    assert(serialized instanceof ArrayBuffer, "SerializeObject returns ArrayBuffer");
    var restored = WScript.Deserialize(serialized);
    assert(restored.a === 42 && restored.b === "hello" && restored.c.length === 3 && restored.c[2] === 3,
        "Basic round-trip value preserved");

    // 2. Second Deserialize on same buffer must throw, never UAF.
    assertThrows(function () { WScript.Deserialize(serialized); }, "Second Deserialize must fail");

    // 3. Round-trip with transferred ArrayBuffer + double Deserialize rejected.
    var srcBuf = new ArrayBuffer(16);
    var srcView = new Uint8Array(srcBuf);
    for (var i = 0; i < 16; i++) srcView[i] = i + 10;
    var sWithTransfer = WScript.SerializeObject({ buf: srcBuf }, [srcBuf]);
    assert(srcBuf.byteLength === 0, "Transferred source buffer is detached");
    // Mutating the opaque token buffer bytes must not corrupt host pointers.
    new Uint8Array(sWithTransfer).fill(0x41);
    var rWithTransfer = WScript.Deserialize(sWithTransfer);
    var rView = new Uint8Array(rWithTransfer.buf);
    assert(rView.length === 16 && rView[0] === 10 && rView[15] === 25, "Transferred bytes preserved");
    assertThrows(function () { WScript.Deserialize(sWithTransfer); }, "Second Deserialize with transfer must fail");

    // 4. Forged / arbitrary ArrayBuffers must throw cleanly, never SIGSEGV.
    var forged64 = new ArrayBuffer(64);
    var dv = new DataView(forged64);
    dv.setUint32(0, 0x41414141, true);
    dv.setUint32(4, 0x41414141, true);
    dv.setUint32(8, 16, true);
    assertThrows(function () { WScript.Deserialize(forged64); }, "Forged 64-byte buffer rejected");

    var forged8 = new ArrayBuffer(8);
    assertThrows(function () { WScript.Deserialize(forged8); }, "Forged 8-byte buffer rejected");

    var emptyBuf = new ArrayBuffer(0);
    assertThrows(function () { WScript.Deserialize(emptyBuf); }, "Empty buffer rejected");
    assertThrows(function () { WScript.Deserialize({}); }, "Non-ArrayBuffer rejected");
    assertThrows(function () { WScript.Deserialize(); }, "Missing argument rejected");

    // 5. Duplicate ArrayBuffer in transfer list and oversized transfer list rejected.
    var dupBuf = new ArrayBuffer(8);
    assertThrows(function () { WScript.SerializeObject(dupBuf, [dupBuf, dupBuf]); },
        "Duplicate transferable rejected");
    assert(dupBuf.byteLength === 8, "Duplicate rejection leaves buffer attached");

    assertThrows(function () { WScript.SerializeObject({}, new Array(1000000)); },
        "Oversized transfer list rejected");

    // 6. Failed serialization (throwing getter, already-detached or immutable
    // transferable) cleans up native buffer/handle without leaking.
    assertThrows(function () {
        WScript.SerializeObject({ get boom() { throw new Error("boom"); } });
    }, "Throwing getter during SerializeObject rejected");

    var detBuf = new ArrayBuffer(16);
    var detSer = WScript.SerializeObject(detBuf, [detBuf]);
    assertThrows(function () { WScript.SerializeObject(detBuf, [detBuf]); },
        "Already-detached transferable rejected");
    WScript.Deserialize(detSer);

    if (typeof ArrayBuffer.prototype.transferToImmutable === "function") {
        var immBuf = (new ArrayBuffer(8)).transferToImmutable();
        assertThrows(function () { WScript.SerializeObject(immBuf, [immBuf]); },
            "Immutable transferable rejected");
    }

    // 7. Unconsumed serialized blobs (with and without transferables) are freed on GC/teardown.
    for (var k = 0; k < 20; k++) {
        var tmp = new ArrayBuffer(32);
        new Uint8Array(tmp).fill(k);
        WScript.SerializeObject({ k: k, tmp: tmp }, [tmp]);
    }
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// --- GR-FE4: WScript.SetTimeout / ClearTimeout edge delays ---
var fe4Fired = [];
var edgeDelays = [-1, -5, -0.5, -Infinity, NaN, Infinity, 1e20, 2147483648, 4294967296, 0];
for (var d = 0; d < edgeDelays.length; d++) {
    (function (idx) {
        WScript.SetTimeout(function () { fe4Fired.push(idx); }, edgeDelays[idx]);
    })(d);
}

// ClearTimeout with non-finite / negative / out-of-range values must not trigger UB.
WScript.ClearTimeout(NaN);
WScript.ClearTimeout(-1);
WScript.ClearTimeout(Infinity);
WScript.ClearTimeout(-Infinity);
WScript.ClearTimeout(1e20);
WScript.ClearTimeout(4294967296);

// ClearTimeout on a real pending timer behind other timers.
var cancelledFired = false;
var tEarly = WScript.SetTimeout(function () {}, 1);
var tCancel = WScript.SetTimeout(function () { cancelledFired = true; }, 5);
WScript.ClearTimeout(tCancel);

// --- GR-FE5: Microtask FIFO + timer ordering + linear scaling ---
var orderLog = [];
WScript.SetTimeout(function () { orderLog.push("timer-20"); }, 20);
WScript.SetTimeout(function () { orderLog.push("timer-10a"); }, 10);
WScript.SetTimeout(function () { orderLog.push("timer-10b"); }, 10);
Promise.resolve().then(function () { orderLog.push("micro-1"); });
Promise.resolve().then(function () { orderLog.push("micro-2"); });

// Scale check: 20,000 promise microtasks + 5,000 timers queued before drain.
var microCount = 0;
var t0 = Date.now();
for (var m = 0; m < 20000; m++) {
    Promise.resolve().then(function () { microCount++; });
}
var timerCount = 0;
for (var ti = 0; ti < 5000; ti++) {
    WScript.SetTimeout(function () { timerCount++; }, 0);
}
var enqueueMs = Date.now() - t0;

WScript.SetTimeout(function () {
    assert(fe4Fired.length === edgeDelays.length,
        "All GR-FE4 edge timers fired: got " + fe4Fired.length);
    assert(!cancelledFired, "Cancelled timer did not fire");
    assert(microCount === 20000, "All 20000 microtasks ran: got " + microCount);
    assert(timerCount === 5000, "All 5000 timers ran: got " + timerCount);
    assert(orderLog.join(",") === "micro-1,micro-2,timer-10a,timer-10b,timer-20",
        "Ordering preserved: got " + orderLog.join(","));
    print("PASS");
}, 40);
