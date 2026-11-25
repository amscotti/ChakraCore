//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Unaligned DataView read/write regression test across offsets 0..7 in both
// big-endian and little-endian modes (Int16, Uint16, Int32, Uint32, Float32,
// Float64, Float16, BigInt64, BigUint64), including NaN, -0, Infinity,
// subnormals, and unaligned view byteOffsets.

function assertEq(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error("FAIL (" + msg + "): expected " + String(expected) + ", got " + String(actual));
    }
}

function assertTrue(cond, msg) {
    if (!cond) {
        throw new Error("FAIL (" + msg + ")");
    }
}

function fillCanary(bytes) {
    for (var i = 0; i < bytes.length; i++) {
        bytes[i] = 0xa5;
    }
}

function verifyCanaries(bytes, offset, width, label) {
    for (var i = 0; i < bytes.length; i++) {
        if (i < offset || i >= offset + width) {
            assertEq(bytes[i], 0xa5, label + " canary clobbered at byte " + i);
        }
    }
}

function checkIntType(getter, setter, width, values) {
    var buf = new ArrayBuffer(32);
    var bytes = new Uint8Array(buf);
    var dv = new DataView(buf);

    for (var offset = 0; offset <= 7; offset++) {
        for (var vi = 0; vi < values.length; vi++) {
            var val = values[vi];
            for (var leIdx = 0; leIdx < 2; leIdx++) {
                var le = leIdx === 1;
                fillCanary(bytes);
                dv[setter](offset, val, le);
                verifyCanaries(bytes, offset, width, setter + " le=" + le + " off=" + offset);

                var got = dv[getter](offset, le);
                assertEq(got, val, setter + "/" + getter + " roundtrip le=" + le + " off=" + offset);

                // Cross-check exact big-endian vs little-endian byte order.
                var opposite = dv[getter](offset, !le);
                var manual = 0n;
                if (!le) {
                    for (var b = 0; b < width; b++) {
                        manual = (manual << 8n) | BigInt(bytes[offset + b]);
                    }
                } else {
                    for (var b = width - 1; b >= 0; b--) {
                        manual = (manual << 8n) | BigInt(bytes[offset + b]);
                    }
                }
                if (typeof val === "bigint") {
                    if (getter === "getBigInt64") {
                        var mask = (1n << 64n) - 1n;
                        var expectedBits = val & mask;
                        assertEq(manual, expectedBits, getter + " raw bytes le=" + le + " off=" + offset);
                    } else {
                        assertEq(manual, val, getter + " raw bytes le=" + le + " off=" + offset);
                    }
                } else {
                    var masked = BigInt(val >>> 0);
                    if (width === 2) {
                        masked = BigInt(val & 0xffff);
                    }
                    assertEq(manual, masked, getter + " raw bytes le=" + le + " off=" + offset);
                }
                void opposite;
            }
        }
    }
}

function checkFloatType(getter, setter, width, values) {
    var buf = new ArrayBuffer(32);
    var bytes = new Uint8Array(buf);
    var dv = new DataView(buf);

    for (var offset = 0; offset <= 7; offset++) {
        for (var vi = 0; vi < values.length; vi++) {
            var val = values[vi];
            for (var leIdx = 0; leIdx < 2; leIdx++) {
                var le = leIdx === 1;
                fillCanary(bytes);
                dv[setter](offset, val, le);
                verifyCanaries(bytes, offset, width, setter + " le=" + le + " off=" + offset);

                var got = dv[getter](offset, le);
                if (Number.isNaN(val)) {
                    assertTrue(Number.isNaN(got), getter + " NaN le=" + le + " off=" + offset);
                } else {
                    assertEq(got, val, getter + " roundtrip le=" + le + " off=" + offset);
                }

                // Verify reversed byte order matches a canonically aligned reference buffer.
                var refBuf = new ArrayBuffer(8);
                var refDv = new DataView(refBuf);
                var refBytes = new Uint8Array(refBuf);
                refDv[setter](0, val, le);
                for (var b = 0; b < width; b++) {
                    assertEq(bytes[offset + b], refBytes[b], setter + " byte[" + b + "] le=" + le + " off=" + offset);
                }
            }
        }

        // Non-canonical / signaling-like NaN bit patterns at every unaligned offset.
        var sNaNBytesBE = width === 2
            ? [0x7c, 0x01]
            : (width === 4
                ? [0x7f, 0x80, 0x00, 0x01]
                : [0x7f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01]);
        fillCanary(bytes);
        for (var k = 0; k < width; k++) {
            bytes[offset + k] = sNaNBytesBE[k];
        }
        assertTrue(Number.isNaN(dv[getter](offset, false)), getter + " BE signaling-NaN bits off=" + offset);

        for (var k = 0; k < width; k++) {
            bytes[offset + k] = sNaNBytesBE[width - 1 - k];
        }
        assertTrue(Number.isNaN(dv[getter](offset, true)), getter + " LE signaling-NaN bits off=" + offset);
    }
}

function runSuite() {
    checkIntType("getInt16", "setInt16", 2, [0, 1, -1, 0x1234, -0x1234, 0x7fff, -0x8000]);
    checkIntType("getUint16", "setUint16", 2, [0, 1, 0x1234, 0x7fff, 0x8000, 0xffff]);
    checkIntType("getInt32", "setInt32", 4, [0, 1, -1, 0x12345678, -0x12345678, 0x7fffffff, -0x80000000]);
    checkIntType("getUint32", "setUint32", 4, [0, 1, 0x12345678, 0x7fffffff, 0x80000000, 0xffffffff]);

    checkFloatType("getFloat32", "setFloat32", 4, [
        0, -0, 1.5, -123.625,
        Infinity, -Infinity, NaN,
        Math.pow(2, -149), // smallest Float32 subnormal
        Math.pow(2, -140),
        3.4028234663852886e+38
    ]);

    checkFloatType("getFloat64", "setFloat64", 8, [
        0, -0, Math.PI, -1e-300,
        Infinity, -Infinity, NaN,
        Number.MIN_VALUE, // 5e-324 subnormal
        Math.pow(2, -1050),
        Number.MAX_VALUE
    ]);

    if (typeof DataView.prototype.getFloat16 === "function") {
        checkFloatType("getFloat16", "setFloat16", 2, [
            0, -0, 1.5, -65504,
            Infinity, -Infinity, NaN,
            Math.pow(2, -24), // smallest Float16 subnormal
            Math.pow(2, -15)
        ]);
    }

    if (typeof DataView.prototype.getBigInt64 === "function") {
        checkIntType("getBigInt64", "setBigInt64", 8, [
            0n, 1n, -1n,
            0x0123456789abcdefn,
            -0x0123456789abcdefn,
            0x7fffffffffffffffn,
            -0x8000000000000000n
        ]);
        checkIntType("getBigUint64", "setBigUint64", 8, [
            0n, 1n,
            0x0123456789abcdefn,
            0x7fffffffffffffffn,
            0x8000000000000000n,
            0xffffffffffffffffn
        ]);
    }

    // Also test views constructed with unaligned DataView byteOffset (1, 3, 5, 7).
    var buf = new ArrayBuffer(32);
    for (var baseOff = 1; baseOff <= 7; baseOff += 2) {
        var sub = new DataView(buf, baseOff);
        for (var inner = 0; inner <= 7; inner++) {
            sub.setInt16(inner, -12345, false);
            assertEq(sub.getInt16(inner, false), -12345, "subview Int16 BE base=" + baseOff + " inner=" + inner);
            sub.setUint32(inner, 0xdeadbeef, true);
            assertEq(sub.getUint32(inner, true), 0xdeadbeef, "subview Uint32 LE base=" + baseOff + " inner=" + inner);
            sub.setFloat32(inner, -0, false);
            assertEq(sub.getFloat32(inner, false), -0, "subview Float32 -0 BE base=" + baseOff + " inner=" + inner);
            sub.setFloat64(inner, Infinity, true);
            assertEq(sub.getFloat64(inner, true), Infinity, "subview Float64 Inf LE base=" + baseOff + " inner=" + inner);
        }
    }

    // 0-byte ArrayBuffer DataView construction (null + 0 guard) + range error.
    var emptyView = new DataView(new ArrayBuffer(0));
    var threw = false;
    try {
        emptyView.getUint16(0);
    } catch (e) {
        threw = e instanceof RangeError;
    }
    assertTrue(threw, "0-byte DataView.getUint16(0) throws RangeError");
}

for (var iter = 0; iter < 50; iter++) {
    runSuite();
}
print("PASS");
