//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// PE-5 regression: views over fixed-size SharedArrayBuffers must not allocate
// parent weak-reference nodes, while growable SharedArrayBuffers and
// resizable/detachable ArrayBuffers continue tracking views accurately across
// GC, grow, resize, and transfer/detach.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function maybeGC() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

var tests = [
    {
        name: "Fixed-size SharedArrayBuffer views stay functional under heavy view churn and GC",
        body() {
            var sab = new SharedArrayBuffer(64);
            var writer = new Int32Array(sab);
            writer[0] = 0x12345678;
            writer[15] = -42;

            var retained = [
                new Uint8Array(sab),
                new Int32Array(sab, 0, 16),
                new BigInt64Array(sab),
                new DataView(sab, 4, 8)
            ];

            for (var i = 0; i < 5000; i++) {
                var u8 = new Uint8Array(sab, (i & 7) * 4, 16);
                var sub = u8.subarray(4, 8);
                var dv = new DataView(sab, 0, 16);
                if (i % 1000 === 0) {
                    maybeGC();
                }
                assert.areEqual(16, u8.length);
                assert.areEqual(4, sub.length);
                assert.areEqual(16, dv.byteLength);
            }

            maybeGC();
            maybeGC();

            assert.areEqual(64, retained[0].length);
            assert.areEqual(0x78, retained[0][0]);
            assert.areEqual(0x12345678, retained[1][0]);
            assert.areEqual(-42, retained[1][15]);
            assert.areEqual(8, retained[2].length);
            assert.areEqual(8, retained[3].byteLength);
        }
    },
    {
        name: "Growable SharedArrayBuffer still tracks live auto-length views across GC",
        body() {
            var gsab = new SharedArrayBuffer(8, { maxByteLength: 32 });
            var trackI32 = new Int32Array(gsab);
            var trackU8 = new Uint8Array(gsab, 4);
            var fixedI32 = new Int32Array(gsab, 0, 1);
            var trackDv = new DataView(gsab, 4);

            // Churn temporary views that become collectible before grow().
            for (var i = 0; i < 1500; i++) {
                new Uint8Array(gsab);
                new DataView(gsab);
            }
            maybeGC();
            maybeGC();

            assert.areEqual(2, trackI32.length);
            assert.areEqual(4, trackU8.length);
            assert.areEqual(1, fixedI32.length);
            assert.areEqual(4, trackDv.byteLength);

            gsab.grow(24);
            assert.areEqual(6, trackI32.length);
            assert.areEqual(20, trackU8.length);
            assert.areEqual(1, fixedI32.length);
            assert.areEqual(20, trackDv.byteLength);

            trackI32[5] = 999;
            assert.areEqual(999, trackDv.getInt32(16, true));
        }
    },
    {
        name: "Resizable and transferable ArrayBuffers still track resize and detach across GC",
        body() {
            var rab = new ArrayBuffer(8, { maxByteLength: 32 });
            var track = new Int32Array(rab);
            var dv = new DataView(rab);
            for (var i = 0; i < 1500; i++) {
                new Uint8Array(rab);
            }
            maybeGC();

            rab.resize(16);
            assert.areEqual(4, track.length);
            assert.areEqual(16, dv.byteLength);
            track[3] = 77;

            rab.resize(4);
            assert.areEqual(1, track.length);
            assert.areEqual(4, dv.byteLength);

            var transferred = rab.transfer();
            assert.isTrue(rab.detached);
            assert.areEqual(0, track.length);
            assert.areEqual(0, track.byteLength);
            assert.throws(function () { dv.byteLength; }, TypeError);
            assert.areEqual(4, transferred.byteLength);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
