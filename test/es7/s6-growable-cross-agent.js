//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Cross-agent growable SharedArrayBuffer coherence test: foreign runtime
// creates length-tracking TypedArray / DataView objects, JIT-warms reads and
// length queries before grow(), observes main-agent grow(), then reads newly
// exposed elements and executes Atomics.waitAsync on the grown region.

function fail(msg) {
    print("s6-growable-cross-agent: FAIL: " + msg);
    WScript.Quit(1);
}

function check(cond, msg) {
    if (!cond) {
        fail(msg);
    }
}

function getReportWithTimeout(maxTries) {
    for (var i = 0; i < maxTries; i++) {
        var r = $262.agent.getReport();
        if (r !== null && r !== undefined) {
            return r;
        }
        $262.agent.sleep(5);
    }
    fail("timed out waiting for agent report");
}

function waitUntil(ctrl, idx, target) {
    for (var i = 0; i < 4000; i++) {
        if (Atomics.load(ctrl, idx) === target) {
            return;
        }
        $262.agent.sleep(1);
    }
    fail("timed out waiting for Atomics state at index " + idx + " to reach " + target);
}

if (typeof SharedArrayBuffer !== "function" ||
    typeof Atomics !== "object" ||
    typeof Atomics.waitAsync !== "function" ||
    typeof $262 === "undefined" ||
    !$262.agent) {
    fail("missing SharedArrayBuffer / Atomics.waitAsync / $262.agent harness");
}

// ctrl[0]: main -> agent phase flags; ctrl[1]: agent -> main ack
var ctrlSab = new SharedArrayBuffer(16);
var ctrl = new Int32Array(ctrlSab);

// Growable backing buffer: start at 16 bytes (4 int32s), max 64 bytes (16 int32s).
var gsab = new SharedArrayBuffer(16, { maxByteLength: 64 });
var mainI32 = new Int32Array(gsab);
mainI32[0] = 10;
mainI32[1] = 20;
mainI32[2] = 30;
mainI32[3] = 40;

$262.agent.start(`
    function getLen(arr) {
        return arr.length;
    }
    function getElem(arr, idx) {
        return arr[idx];
    }
    function getDvLen(dv) {
        return dv.byteLength;
    }
    function reportFail(msg) {
        $262.agent.report("FAIL:" + msg);
        $262.agent.leaving();
    }

    var sharedCtrl = null;
    var sharedGsab = null;
    $262.agent.receiveBroadcast(function(buf) {
        if (!sharedCtrl) {
            sharedCtrl = new Int32Array(buf);
            return;
        }
        sharedGsab = buf;

        var i32Track = new Int32Array(sharedGsab);
        var i32OffsetTrack = new Int32Array(sharedGsab, 8);
        var i32Fixed = new Int32Array(sharedGsab, 0, 2);
        var b64Track = new BigInt64Array(sharedGsab, 16);
        var f16Track = new Float16Array(sharedGsab, 16);
        var dvTrack = new DataView(sharedGsab, 8);

        // Warm JIT on pre-grow lengths and OOB accesses.
        for (var i = 0; i < 20000; i++) {
            if (getLen(i32Track) !== 4 || getLen(i32OffsetTrack) !== 2 ||
                getLen(i32Fixed) !== 2 || getLen(b64Track) !== 0 ||
                getLen(f16Track) !== 0 || getDvLen(dvTrack) !== 8) {
                reportFail("pre-grow JIT length mismatch");
                return;
            }
            if (getElem(i32Track, 4) !== undefined || getElem(b64Track, 0) !== undefined ||
                getElem(f16Track, 0) !== undefined) {
                reportFail("pre-grow OOB read returned value");
                return;
            }
        }

        // Notify main thread that foreign views are created & JIT-warmed.
        Atomics.store(sharedCtrl, 1, 1);

        // Wait for phase 1 (grow to 32 bytes).
        while (Atomics.load(sharedCtrl, 0) !== 1) {
            Atomics.pause();
        }

        // Re-enter JIT-warmed accessors after main-thread grow(32).
        for (var j = 0; j < 20000; j++) {
            if (getLen(i32Track) !== 8) {
                reportFail("post-grow i32Track.length expected 8, got " + getLen(i32Track));
                return;
            }
            if (getLen(i32OffsetTrack) !== 6) {
                reportFail("post-grow i32OffsetTrack.length expected 6, got " + getLen(i32OffsetTrack));
                return;
            }
            if (getLen(i32Fixed) !== 2) {
                reportFail("post-grow i32Fixed.length expected 2, got " + getLen(i32Fixed));
                return;
            }
            if (getLen(b64Track) !== 2) {
                reportFail("post-grow b64Track.length expected 2, got " + getLen(b64Track));
                return;
            }
            if (getLen(f16Track) !== 8) {
                reportFail("post-grow f16Track.length expected 8, got " + getLen(f16Track));
                return;
            }
            if (getDvLen(dvTrack) !== 24) {
                reportFail("post-grow dvTrack.byteLength expected 24, got " + getDvLen(dvTrack));
                return;
            }
        }

        // Newly grown bytes (index 4..7 in i32Track) must be zero-initialized before
        // any write, and index 4..5 are untouched by main before phase 1 except
        // i32Track[5] which waitAsync not-equal checks after grow.
        var elem4 = getElem(i32Track, 4);
        var elem5 = getElem(i32Track, 5);
        var b64_0 = getElem(b64Track, 0);
        var f16_0 = getElem(f16Track, 0);
        if (elem4 !== 0 || elem5 !== 0 || b64_0 !== 0n || f16_0 !== 0) {
            reportFail("grown region not zero-filled: elem4=" + elem4 +
                " elem5=" + elem5 + " b64_0=" + b64_0 + " f16_0=" + f16_0);
            return;
        }

        // Fast-path Atomics.waitAsync not-equal when value is 0 vs expected 99.
        var neq = Atomics.waitAsync(i32OffsetTrack, 3, 99);
        if (neq.async !== false || neq.value !== "not-equal") {
            reportFail("waitAsync expected not-equal on grown index, got async=" + neq.async + " value=" + neq.value);
            return;
        }

        // Register async waiter on grown index 4 of i32Track (index 2 of i32OffsetTrack).
        var waiter = Atomics.waitAsync(i32OffsetTrack, 2, 0);
        if (waiter.async !== true) {
            reportFail("waitAsync on grown index expected async=true");
            return;
        }
        Atomics.store(sharedCtrl, 1, 2);

        waiter.value.then(function(res) {
            // Wait for phase 2 (grow to 64 bytes while agent handles callbacks).
            while (Atomics.load(sharedCtrl, 0) !== 2) {
                Atomics.pause();
            }
            if (getLen(i32Track) !== 16 || getLen(b64Track) !== 6) {
                reportFail("phase-2 grow(64) lengths wrong: i32=" + getLen(i32Track));
                return;
            }
            var w64 = Atomics.load(b64Track, 4);
            if (w64 !== 0x123456789abcdef0n && w64 !== 0n) {
                reportFail("phase-2 torn read on b64Track[4]: " + w64);
                return;
            }
            $262.agent.report("OK:" + res);
            $262.agent.leaving();
        });
    });
`);

$262.agent.broadcast(ctrlSab);
$262.agent.broadcast(gsab);

// Wait until foreign agent created its tracking views and warmed JIT.
waitUntil(ctrl, 1, 1);
check(mainI32.length === 4, "main pre-grow length");

// Phase 1: grow GSAB to 32 bytes from main thread while foreign agent already holds views.
gsab.grow(32);
check(mainI32.length === 8, "main post-grow(32) length");
Atomics.store(ctrl, 0, 1);

// Wait until agent verified post-grow lengths & registered Atomics.waitAsync on byte offset 16.
waitUntil(ctrl, 1, 2);
check(Atomics.notify(mainI32, 4) === 1, "notify foreign waiter on grown byte offset 16");

// Phase 2: grow to 64 bytes and write a BigInt64 sentinel in newly grown storage.
gsab.grow(64);
var mainB64 = new BigInt64Array(gsab);
Atomics.store(mainB64, 6, 0x123456789abcdef0n);
Atomics.store(ctrl, 0, 2);

var report = getReportWithTimeout(1000);
check(report === "OK:ok", "expected OK:ok from agent, got " + report);

if (typeof CollectGarbage === "function") {
    CollectGarbage();
}

print("pass");
