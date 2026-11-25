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
            throw new Error(msg + ": unexpected error " + e);
        }
    }
    if (!threw) {
        throw new Error(msg + ": expected TypeError");
    }
}

function bitOrZero(x) {
    var t = x | 0;
    return [t, x];
}

function bitNot(x) {
    return ~x;
}

function bitNotNot(x) {
    return ~(~x);
}

function bitNotSub(a, b) {
    return ~(a - b);
}

function bitNotAdd(a, b) {
    return ~(a + b);
}

function bitXor(a, b) {
    return a ^ b;
}

function bitOr(a, b) {
    return a | b;
}

function bitAnd(a, b) {
    return a & b;
}

function bitShl(a, b) {
    return a << b;
}

function bitShr(a, b) {
    return a >> b;
}

function bitShrUZero(x) {
    return x >>> 0;
}

function relLt(a, b) {
    return a < b;
}

function relUnsignedLoop(arr) {
    var s = 0;
    for (var i = 0; i < arr.length; i++) {
        if ((arr[i] >>> 0) < 0x80000010) {
            s++;
        }
    }
    return s;
}

function loopInvariantBit(x, n) {
    var s = 0;
    for (var i = 0; i < n; i++) {
        s = (x ^ i) | 0;
    }
    return s;
}

// 1. Warm up with strings, undefined, floats, and large uint32 BEFORE ints,
//    verifying that lossy int32 ops still handle all non-BigInt values without crashing.
function preWarmedMask(a, b) {
    return ((a | 0) ^ (b >>> 0)) + (~(a & 0xff));
}
var preWarmVals = ["42", undefined, null, true, 3.9, 0x80000000, -0, NaN, Infinity, 1e20, 123456];
var preWarmExpected = [-43, -1, -1, -2, -4, -1, -1, -1, -1, -1, -65];
for (var p = 0; p < 500; p++) {
    var idx = p % preWarmVals.length;
    var pv = preWarmVals[idx];
    assertEqual(preWarmedMask(pv, pv), preWarmExpected[idx], "preWarmedMask[" + idx + "]");
    assertEqual(preWarmedMask("42", undefined), (42 ^ 0) + (~(42 & 0xff)), "preWarmedMask fixed");
    assertEqual(preWarmedMask(0x80000005, -0), (0x80000005 | 0) + (~(5)), "preWarmedMask uint32");
}

// 2. Warm up all bitwise & relational functions on small tagged ints.
for (var w = 0; w < 500; w++) {
    var bo = bitOrZero(w);
    assertEqual(bo[0], w, "warmup bitOrZero[0]");
    assertEqual(bo[1], w, "warmup bitOrZero[1]");
    assertEqual(bitNot(w), ~w, "warmup bitNot");
    assertEqual(bitNotNot(w), w, "warmup bitNotNot");
    assertEqual(bitNotSub(w + 5, w), -6, "warmup bitNotSub");
    assertEqual(bitNotAdd(w, 3), ~(w + 3), "warmup bitNotAdd");
    assertEqual(bitXor(w, 0x55), w ^ 0x55, "warmup bitXor");
    assertEqual(bitOr(w, 0xaa), w | 0xaa, "warmup bitOr");
    assertEqual(bitAnd(w, 0x3f), w & 0x3f, "warmup bitAnd");
    assertEqual(bitShl(w, 3), w << 3, "warmup bitShl");
    assertEqual(bitShr(w, 2), w >> 2, "warmup bitShr");
    assertEqual(bitShrUZero(w), w, "warmup bitShrUZero");
    assertEqual(relLt(w, 250), w < 250, "warmup relLt");
    assertEqual(relUnsignedLoop([1, 2, 0x80000020, 0x7fffffff]), 3, "warmup relUnsignedLoop");
    assertEqual(loopInvariantBit(w, 10), w ^ 9, "warmup loopInvariantBit");
}

// 3. Edge non-BigInt values after int warmup: floats, -0, NaN, Infinity, 1e20, strings, undefined, objects.
for (var iter = 0; iter < 300; iter++) {
    var nz = bitOrZero(-0);
    assertEqual(nz[0], 0, "-0 | 0 == 0");
    assertEqual(nz[1], -0, "original -0 preserved");
    assertEqual(bitOrZero(NaN)[0], 0, "NaN | 0 == 0");
    assertEqual(bitOrZero(Infinity)[0], 0, "Infinity | 0 == 0");
    assertEqual(bitOrZero(-Infinity)[0], 0, "-Infinity | 0 == 0");
    assertEqual(bitOrZero(1e20)[0], 1661992960, "1e20 | 0");
    assertEqual(bitOrZero("42")[0], 42, "'42' | 0");
    assertEqual(bitOrZero(undefined)[0], 0, "undefined | 0");
    assertEqual(bitOrZero(null)[0], 0, "null | 0");
    assertEqual(bitOrZero(true)[0], 1, "true | 0");
    assertEqual(bitOrZero({})[0], 0, "{} | 0");
    assertEqual(bitOrZero({ valueOf: function () { return 7; } })[0], 7, "{valueOf:7} | 0");

    assertEqual(bitNot("42"), -43, "~'42'");
    assertEqual(bitNot(undefined), -1, "~undefined");
    assertEqual(bitNot({ valueOf: function () { return 7; } }), -8, "~{valueOf:7}");
    assertEqual(bitShrUZero(-1), 4294967295, "-1 >>> 0");
    assertEqual(bitShrUZero(0x80000000), 2147483648, "0x80000000 >>> 0");
    assertEqual(relLt("2", "10"), false, "'2' < '10'");
    assertEqual(relLt(5, undefined), false, "5 < undefined");
    assertEqual(relUnsignedLoop([0x80000005, 0x80000020, undefined, "12"]), 3, "edge relUnsignedLoop");
}

// 4. BigInt inputs & { valueOf() { return 7n; } } after JIT warmup and bailout transitions.
for (var bi = 0; bi < 300; bi++) {
    assertEqual(bitNot(1n), -2n, "~1n");
    assertEqual(bitNot(-1n), 0n, "~(-1n)");
    assertEqual(bitNotNot(0n), 0n, "~(~0n)");
    assertEqual(bitNotNot(0x123456789abcdef0n), 0x123456789abcdef0n, "~(~wideBigInt)");
    assertEqual(bitNotSub(1n, 2n), 0n, "~(1n - 2n)");
    assertEqual(bitNotAdd(1n, 2n), -4n, "~(1n + 2n)");
    assertEqual(bitXor(0x1ffffffffn, 0x0f0f0f0fn), 0x1f0f0f0f0n, "BigInt ^");
    assertEqual(bitOr(0x100000000n, 3n), 0x100000003n, "BigInt |");
    assertEqual(bitAnd(0x1ffffffffn, 0x1000000ffn), 0x1000000ffn, "BigInt &");
    assertEqual(bitShl(1n, 40n), 1099511627776n, "BigInt <<");
    assertEqual(bitShr(1099511627776n, 40n), 1n, "BigInt >>");

    assertEqual(relLt(9007199254740992n, 9007199254740993n), true, "BigInt < precision");
    assertEqual(relLt(9007199254740993n, 9007199254740992), false, "BigInt < large double");

    var objBig = { valueOf: function () { return 7n; } };
    assertEqual(bitNot(objBig), -8n, "~{valueOf:7n}");
    assertEqual(bitXor(objBig, 1n), 6n, "{valueOf:7n} ^ 1n");
    assertEqual(relLt(objBig, 8n), true, "{valueOf:7n} < 8n");

    assertThrowsTypeError(function () { return bitOrZero(1n); }, "1n | 0");
    assertThrowsTypeError(function () { return bitOrZero(objBig); }, "{valueOf:7n} | 0");
    assertThrowsTypeError(function () { return bitXor(1n, 2); }, "1n ^ 2");
    assertThrowsTypeError(function () { return bitXor(objBig, 2); }, "{valueOf:7n} ^ 2");
    assertThrowsTypeError(function () { return bitShrUZero(1n); }, "1n >>> 0");
    assertThrowsTypeError(function () { return loopInvariantBit(1n, 5); }, "loopInvariantBit 1n");
}

// 5. Hoisted loop-invariant test with BigInt in first loop iteration vs later.
function loopHoistObjBig(x, n) {
    var acc = 0;
    for (var i = 0; i < n; i++) {
        acc = (x | 0) + i;
    }
    return acc;
}
for (var lh = 0; lh < 300; lh++) {
    assertEqual(loopHoistObjBig(10, 5), 14, "loopHoistObjBig warmup");
}
for (var lh2 = 0; lh2 < 100; lh2++) {
    assertThrowsTypeError(function () { return loopHoistObjBig(1n, 5); }, "loopHoistObjBig 1n");
    assertThrowsTypeError(function () { return loopHoistObjBig({ valueOf: function () { return 7n; } }, 5); }, "loopHoistObjBig obj(7n)");
    assertEqual(loopHoistObjBig("42", 5), 46, "loopHoistObjBig '42'");
    assertEqual(loopHoistObjBig(undefined, 5), 4, "loopHoistObjBig undefined");
}

print("PASS");
