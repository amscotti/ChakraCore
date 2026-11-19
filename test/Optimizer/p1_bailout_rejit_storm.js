//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEqual(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(msg + ": expected " + String(expected) + " got " + String(actual));
    }
}

function assertThrowsTypeError(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        if (e instanceof TypeError) {
            threw = true;
        } else {
            throw new Error(msg + ": wrong error " + e);
        }
    }
    if (!threw) {
        throw new Error(msg + ": expected TypeError");
    }
}

// 1. Direct calls: warmup on small ints then >= 0x80000000, floats, -0, INT32_MIN, undefined, objects, BigInt.
function directBitOps(a, b) {
    var x = (a[0] ^ b[0]) | 0;
    var y = ((a[1] & 0xff) << 24) ^ (b[1] >>> 8);
    var z = ~(a[2] | b[2]);
    return [x, y, z];
}

function preserveNegZero(x) {
    var truncated = x | 0;
    return [truncated, x, 1 / x];
}

function directLossyBigInt(x, y) {
    return x ^ y;
}

// 2. Hot loops: multiple loops in one function, loop-carried and loop-invariant vars.
function hotMultiLoop(arr, key, mask) {
    var acc = 0;
    for (var i = 0; i < arr.length; i++) {
        acc = (acc ^ (arr[i] ^ mask)) | 0;
    }
    var out = new Array(key.length);
    for (var j = 0; j < key.length; j++) {
        var v = key[j];
        out[j] = ((v >>> 16) ^ ((v & 0xffff) << 16)) | 0;
    }
    return [acc, out[0], out[key.length - 1]];
}

function hotAggressiveArithmetic(a, b, mode) {
    var sum = 0;
    for (var i = 0; i < 40; i++) {
        if (mode === 0) {
            sum += (a * b) | 0;
        } else if (mode === 1) {
            sum += (a / b) | 0;
        } else {
            sum += a + b;
        }
    }
    return sum;
}

// 3. Inlined callees (2 levels + standalone + precompiled stale caller + post-compiled caller).
function innerBitMix(a, b) {
    if (a.length !== 4) {
        return -1;
    }
    var t = a[0] ^ b[0];
    t ^= (a[1] >>> 4) ^ (b[1] << 3);
    t ^= a[2] | b[2];
    t ^= a[3] & b[3];
    return t | 0;
}

function midCallerWarmBeforeFail(a, b) {
    return innerBitMix(a, b);
}

function topCallerWarmBeforeFail(a, b) {
    return midCallerWarmBeforeFail(a, b);
}

function callerCompiledAfterFail(a, b) {
    return innerBitMix(a, b);
}

// Inlined inside a hot loop.
function inlineLeaf(x, y) {
    return (x ^ y) >>> 0;
}

function loopCallerInlinee(xs, ys) {
    var acc = 0;
    for (var i = 0; i < xs.length; i++) {
        acc = (acc + inlineLeaf(xs[i], ys[i])) | 0;
    }
    return acc;
}

// 4. TypedArray and irregular/mixed array length transitions.
function typedStoreMix(ta, vals) {
    for (var i = 0; i < vals.length; i++) {
        ta[i & 3] = vals[i];
    }
    return [ta[0], ta[1], ta[2], ta[3]];
}

// Warmup phase: pure small tagged ints everywhere.
var smallA = [1, 2, 3, 4];
var smallB = [5, 6, 7, 8];
var u32 = new Uint32Array(4);
for (var i = 0; i < 400; i++) {
    var d = directBitOps(smallA, smallB);
    assertEqual(d[0], 4, "warmup directBitOps[0]");
    assertEqual(d[1], 33554432, "warmup directBitOps[1]");
    assertEqual(d[2], -8, "warmup directBitOps[2]");

    var nz = preserveNegZero(i & 7);
    assertEqual(nz[0], i & 7, "warmup preserveNegZero[0]");
    assertEqual(nz[1], i & 7, "warmup preserveNegZero[1]");

    assertEqual(directLossyBigInt(i, 3), i ^ 3, "warmup directLossyBigInt");

    var ml = hotMultiLoop(smallA, smallB, 0x15);
    assertEqual(ml[0], 4, "warmup hotMultiLoop[0]");
    assertEqual(ml[1], 5 << 16, "warmup hotMultiLoop[1]");

    assertEqual(hotAggressiveArithmetic(3, 7, 0), 840, "warmup mul");
    assertEqual(hotAggressiveArithmetic(21, 7, 1), 120, "warmup div");
    assertEqual(hotAggressiveArithmetic(10, 20, 2), 1200, "warmup add");

    assertEqual(innerBitMix(smallA, smallB), 51, "warmup innerBitMix");
    assertEqual(midCallerWarmBeforeFail(smallA, smallB), 51, "warmup midCaller");
    assertEqual(topCallerWarmBeforeFail(smallA, smallB), 51, "warmup topCaller");
    assertEqual(loopCallerInlinee(smallA, smallB), 24, "warmup loopCallerInlinee");

    var ts = typedStoreMix(u32, smallA);
    assertEqual(ts[0], 1, "warmup typedStoreMix[0]");
}

// Post-warmup stress phase: >= 0x80000000, floats, -0, INT32_MIN, undefined, objects, mixed arrays.
var edgeCasesA = [0xf34481ec, 0x80000000, 0xffffffff, -2147483648];
var edgeCasesB = [0x12345678, 3.9, -0, 5 * 0x10000000000 + 7];
var mixedUndefined = [undefined, "42", -3.9, { valueOf: function () { return 0x80000005; } }];
var mixedObjects = [{ valueOf: function () { return 0xf0f0f0f0; } }, null, true, NaN];

for (var iter = 0; iter < 500; iter++) {
    var d1 = directBitOps(edgeCasesA, edgeCasesB);
    assertEqual(d1[0], -512698476, "edge directBitOps[0]");
    assertEqual(d1[1], 0, "edge directBitOps[1]");
    assertEqual(d1[2], 0, "edge directBitOps[2]");

    var d2 = directBitOps(mixedUndefined, mixedObjects);
    assertEqual(d2[0], -252645136, "obj directBitOps[0]");
    assertEqual(d2[1], 704643072, "obj directBitOps[1]");
    assertEqual(d2[2], 2, "obj directBitOps[2]");

    var pnz = preserveNegZero(-0);
    assertEqual(pnz[0], 0, "negZero bitor trunc");
    assertEqual(pnz[1], -0, "negZero preserved");
    assertEqual(pnz[2], -Infinity, "negZero 1/-0");

    var pinf = preserveNegZero(Infinity);
    assertEqual(pinf[0], 0, "Infinity bitor");
    assertEqual(pinf[1], Infinity, "Infinity preserved");

    var pmin = preserveNegZero(-2147483648);
    assertEqual(pmin[0], -2147483648, "INT32_MIN bitor");

    assertEqual(directLossyBigInt(0xf34481ec, 0x12345678), -512698476, "edge directLossyBigInt");
    assertEqual(directLossyBigInt(undefined, 0x80000001), -2147483647, "undef directLossyBigInt");
    assertEqual(directLossyBigInt(-0, 0), 0, "negzero directLossyBigInt");

    var ml1 = hotMultiLoop(edgeCasesA, edgeCasesB, 0x80000001);
    assertEqual(ml1[0], 213614099, "edge hotMultiLoop[0]");
    assertEqual(ml1[1], 1450709556, "edge hotMultiLoop[1]");
    assertEqual(ml1[2], 458752, "edge hotMultiLoop[2]");

    var ml2 = hotMultiLoop(mixedUndefined, mixedObjects, undefined);
    assertEqual(ml2[0], 2147483602, "mixed hotMultiLoop[0]");

    assertEqual(hotAggressiveArithmetic(0x10000, 0x10000, 0), 0, "mul overflow");
    assertEqual(hotAggressiveArithmetic(-2147483648, -1, 1), -2147483648 * 40, "div INT32_MIN/-1");
    assertEqual(hotAggressiveArithmetic(1, 0, 1), 0, "div by zero");
    assertEqual(hotAggressiveArithmetic(0x7fffffff, 1, 2), 2147483648 * 40, "add overflow");
    assertEqual(hotAggressiveArithmetic(-0, -0, 2), 0, "add -0");

    assertEqual(topCallerWarmBeforeFail(edgeCasesA, edgeCasesB), 378480755, "topCaller edge");
    assertEqual(midCallerWarmBeforeFail(edgeCasesA, edgeCasesB), 378480755, "midCaller edge");
    assertEqual(innerBitMix(edgeCasesA, edgeCasesB), 378480755, "direct innerBitMix edge");
    assertEqual(callerCompiledAfterFail(edgeCasesA, edgeCasesB), 378480755, "postFail caller edge");
    assertEqual(callerCompiledAfterFail(mixedUndefined, mixedObjects), 252645135, "postFail caller obj");
    assertEqual(innerBitMix([1, 2], smallB), -1, "irregular len check");
    assertEqual(topCallerWarmBeforeFail({ length: 4, 0: 0xf34481ec, 1: 0x80000000, 2: 0xffffffff, 3: -2147483648 }, edgeCasesB), 378480755, "non-array object len 4");

    assertEqual(loopCallerInlinee(edgeCasesA, edgeCasesB), -512698467, "loopCallerInlinee edge");
    assertEqual(loopCallerInlinee(mixedUndefined, mixedObjects), 1894838555, "loopCallerInlinee mixed");

    var ts1 = typedStoreMix(u32, [0x80000000, 0xf34481ec, -1, 3.9]);
    assertEqual(ts1[0], 0x80000000, "u32[0]");
    assertEqual(ts1[1], 0xf34481ec, "u32[1]");
    assertEqual(ts1[2], 0xffffffff, "u32[2]");
    assertEqual(ts1[3], 3, "u32[3]");
}

// BigInt semantic preservation after JIT warmup/bailout transitions.
for (var bi = 0; bi < 100; bi++) {
    assertEqual(directLossyBigInt(0x1ffffffffn, 0x0f0f0f0fn), 0x1f0f0f0f0n, "BigInt xor");
    assertThrowsTypeError(function () { return directLossyBigInt(1n, 2); }, "BigInt mixed xor");
    assertThrowsTypeError(function () { return preserveNegZero(1n); }, "BigInt bitOr");
}

print("pass");
