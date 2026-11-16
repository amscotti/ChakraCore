//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused JIT bounds, vtable/type-table alignment, and element access tests for
// Float16Array, BigInt64Array, BigUint64Array, and neighboring typed arrays.

function assert(cond, msg) {
    if (!cond) {
        print("float16-jit-bounds: FAIL: " + msg);
        WScript.Quit(1);
    }
}

function assertSameValue(a, b, msg) {
    if (a !== b) {
        if (a !== a && b !== b) {
            return;
        }
        assert(false, msg + " (expected " + b + ", got " + a + ")");
    } else if (a === 0 && (1 / a) !== (1 / b)) {
        assert(false, msg + " (+0 vs -0 mismatch)");
    }
}

function readF16(arr, idx) {
    return arr[idx];
}

function writeF16(arr, idx, val) {
    arr[idx] = val;
}

function lenF16(arr) {
    return arr.length;
}

function testFloat16MonomorphicJit() {
    var f16 = new Float16Array([1.5, -0, NaN, Math.pow(2, -24), 65504]);
    var sum = 0;
    for (var i = 0; i < 20000; i++) {
        var idx = i % 5;
        var v = readF16(f16, idx);
        if (idx === 0) {
            sum += v;
        }
        assert(lenF16(f16) === 5, "f16 JIT length");
    }
    assert(sum === 6000, "f16 JIT sum");

    assertSameValue(readF16(f16, 1), -0, "f16 -0 preserved");
    assert(readF16(f16, 2) !== readF16(f16, 2), "f16 NaN preserved");
    assertSameValue(readF16(f16, 3), Math.pow(2, -24), "f16 subnormal preserved");
    assertSameValue(readF16(f16, 4), 65504, "f16 max normal preserved");

    // In-bounds write + overflow rounding in JIT
    for (var j = 0; j < 20000; j++) {
        writeF16(f16, 0, (j & 1) ? 65520 : 2.25);
    }
    assertSameValue(f16[0], Infinity, "f16 JIT overflow rounds to Infinity");
    writeF16(f16, 0, 1.5);

    // Out-of-bounds reads and writes must not read past end or alter buffer
    for (var k = 0; k < 20000; k++) {
        assert(readF16(f16, 5) === undefined, "f16 OOB len");
        assert(readF16(f16, 100) === undefined, "f16 OOB 100");
        assert(readF16(f16, -1) === undefined, "f16 negative index");
        assert(readF16(f16, "1.5") === undefined, "f16 canonical non-integer index");
        writeF16(f16, 5, 9.5);
        writeF16(f16, -1, 9.5);
        writeF16(f16, "1.5", 9.5);
    }
    assertSameValue(f16[0], 1.5, "f16 OOB writes do not corrupt index 0");
    assertSameValue(f16[4], 65504, "f16 OOB writes do not corrupt last element");
    assert(f16["1.5"] === undefined, "f16 canonical non-integer write ignored");
}

function readI64(arr, idx) {
    return arr[idx];
}

function writeI64(arr, idx, val) {
    arr[idx] = val;
}

function readU64(arr, idx) {
    return arr[idx];
}

function testBigInt64Jit() {
    var i64 = new BigInt64Array([0n, -1n, 0x7fffffffffffffffn, -0x8000000000000000n]);
    var u64 = new BigUint64Array([0n, 1n, 0xffffffffffffffffn, 0x8000000000000000n]);

    for (var i = 0; i < 20000; i++) {
        var idx = i & 3;
        var a = readI64(i64, idx);
        var b = readU64(u64, idx);
        if (idx === 1) {
            assert(a === -1n && b === 1n, "64-bit JIT element mismatch");
        }
        assert(i64.length === 4 && u64.length === 4, "64-bit length");
        assert(readI64(i64, 4) === undefined, "i64 OOB len");
        assert(readU64(u64, 4) === undefined, "u64 OOB len");
    }

    for (var j = 0; j < 20000; j++) {
        writeI64(i64, 0, BigInt(j));
    }
    assert(i64[0] === 19999n, "i64 JIT store");
    writeI64(i64, 4, 12345n);
    assert(i64[4] === undefined, "i64 OOB store ignored");
}

function polyRead(arr, idx) {
    return arr[idx];
}

function polyLen(arr) {
    return arr.length;
}

function polyWrite(arr, idx, val) {
    arr[idx] = val;
}

function testPolymorphicTypedArrays() {
    var arrays = [
        new Uint16Array([10, 20, 30]),
        new Float16Array([1.5, 2.5, 3.5]),
        new Int32Array([-5, 6, 7]),
        new Float32Array([0.5, 1.5, 2.5]),
        new Float64Array([100.25, 200.5, 300.75]),
        new BigInt64Array([11n, -22n, 33n]),
        new BigUint64Array([111n, 222n, 333n])
    ];
    var expected0 = [10, 1.5, -5, 0.5, 100.25, 11n, 111n];
    var expected2 = [30, 3.5, 7, 2.5, 300.75, 33n, 333n];

    for (var iter = 0; iter < 5000; iter++) {
        for (var k = 0; k < arrays.length; k++) {
            var a = arrays[k];
            assert(polyLen(a) === 3, "polyLen mismatch at " + k);
            assert(polyRead(a, 0) === expected0[k], "polyRead[0] mismatch at " + k);
            assert(polyRead(a, 2) === expected2[k], "polyRead[2] mismatch at " + k);
            assert(polyRead(a, 3) === undefined, "polyRead OOB at " + k);
        }
    }

    // Exercise polymorphic writes on numeric vs bigint views separately
    var numArrays = arrays.slice(0, 5);
    for (var iter2 = 0; iter2 < 5000; iter2++) {
        for (var m = 0; m < numArrays.length; m++) {
            polyWrite(numArrays[m], 1, 4);
            assert(polyRead(numArrays[m], 1) === 4, "polyWrite numeric mismatch at " + m);
        }
    }
}

function readTrack(arr, idx) {
    return arr[idx];
}

function lenTrack(arr) {
    return arr.length;
}

function testLengthTrackingFloat16Jit() {
    if (typeof ArrayBuffer.prototype.resize !== "function") {
        return;
    }
    var ab = new ArrayBuffer(8, { maxByteLength: 16 });
    var ta = new Float16Array(ab, 4);
    ta[0] = 3.25;
    ta[1] = 4.5;

    for (var i = 0; i < 20000; i++) {
        assert(lenTrack(ta) === 2, "initial length-tracking len");
        assertSameValue(readTrack(ta, 1), 4.5, "initial length-tracking read");
    }

    ab.resize(12);
    assert(lenTrack(ta) === 4, "grown length-tracking len");
    assertSameValue(readTrack(ta, 3), 0, "grown length-tracking zero-filled");

    // Shrink below byteOffset (4): length must clamp to 0 and elements become undefined
    ab.resize(2);
    for (var j = 0; j < 20000; j++) {
        assert(lenTrack(ta) === 0, "shrunk below byteOffset must be 0");
        assert(readTrack(ta, 0) === undefined, "shrunk below byteOffset read[0] undefined");
    }
}

testFloat16MonomorphicJit();
testBigInt64Jit();
testPolymorphicTypedArrays();
testLengthTrackingFloat16Jit();

print("pass");
