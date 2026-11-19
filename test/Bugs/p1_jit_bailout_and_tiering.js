//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Cross-tier smoke test for Track P1 JIT bailout and tiering fixes:
// 1. Compound element key liveness & branch inversion safety + bounded loop-body abort.
// 2. BailOutIntOnly mixed int/double/uint32/object/BigInt storm & inlinee ReJIT convergence.
// 3. asm.js external (AsmJsExternalEntryPoint) + internal (TryCallAsmJsJit) BGJIT tier-up.

function assertIs(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(msg + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertClose(actual, expected, eps, msg) {
    if (!(Math.abs(actual - expected) <= eps)) {
        throw new Error(msg + ": expected ~" + expected + ", got " + actual);
    }
}

function maybeGC() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// 1. Compound-key loop liveness & bounded loop-body abort.
function compoundSieveAndKey(n) {
    var size = (n >> 5) + 1;
    var bits = new Array(size);
    for (var k = 0; k < size; k++) {
        bits[k] = 0xffffffff;
    }
    for (var i = 2; i <= n; i++) {
        if ((bits[i >> 5] & (1 << (i & 31))) !== 0) {
            for (var j = i + i; j <= n; j += i) {
                bits[j >> 5] &= ~(1 << (j & 31));
            }
        }
    }
    var primes = 0;
    for (var p = 2; p <= n; p++) {
        if ((bits[p >> 5] & (1 << (p & 31))) !== 0) {
            primes++;
        }
    }
    return primes;
}

function compoundKeyAccess(o, k) {
    o[k] += 2;
    o[k]++;
    return o[k];
}

for (var s = 0; s < 4; s++) {
    assertIs(compoundSieveAndKey(1024), 172, "compoundSieveAndKey iter " + s);
}

var toPropHits = 0;
var objKey = {
    toString: function () {
        toPropHits++;
        return "hit";
    }
};
var keyHolder = { hit: 0, "0": 0, "1": 0, "2": 0, "3": 0 };
for (var m = 0; m < 400; m++) {
    if ((m & 3) === 0) {
        compoundKeyAccess(keyHolder, objKey);
    } else {
        compoundKeyAccess(keyHolder, m & 3);
    }
}
assertIs(toPropHits, 300, "compound key converted once per operator");
assertIs(keyHolder.hit, 300, "compound object-key sum");
assertIs(keyHolder[1], 300, "compound int-key sum");

function abortedLoop(n) {
    var acc = 0;
    for (var i = 0; i < n; i++) {
        if (i < 0) {
            eval("acc++");
        }
        acc += (i & 3);
    }
    return acc;
}
assertIs(abortedLoop(10000), 15000, "aborted eval loop bounded");

maybeGC();

// 2. BailOutIntOnly mixed int/double/uint32/object/BigInt storm & inlinee convergence.
function mixBitwise(a, b) {
    var x = (a[0] ^ b[0]) | 0;
    var y = ((a[1] & 0xff) << 24) ^ (b[1] >>> 8);
    var z = ~(a[2] | b[2]);
    return [x, y, z];
}

function preserveNegZero(x) {
    return [x | 0, x, 1 / x];
}

function inlineBitHelper(a, b) {
    if (a.length !== 3) {
        return -1;
    }
    return ((a[0] ^ b[0]) + (a[1] | b[1]) + (a[2] & b[2])) | 0;
}

function warmCaller(a, b) {
    return inlineBitHelper(a, b);
}

function xorBigIntOrInt(x, y) {
    return x ^ y;
}

var smallA = [1, 2, 3];
var smallB = [4, 5, 6];
var u32 = new Uint32Array(4);
for (var w = 0; w < 300; w++) {
    var r0 = mixBitwise(smallA, smallB);
    assertIs(r0[0], 5, "warm mixBitwise[0]");
    assertIs(r0[1], 33554432, "warm mixBitwise[1]");
    assertIs(r0[2], -8, "warm mixBitwise[2]");
    var nz0 = preserveNegZero(w & 7);
    assertIs(nz0[0], w & 7, "warm preserveNegZero[0]");
    assertIs(warmCaller(smallA, smallB), 14, "warmCaller");
    assertIs(xorBigIntOrInt(w, 3), w ^ 3, "warm xor");
    u32[w & 3] = w;
}

var stormA = [0xf34481ec, 0x80000000, 0xffffffff];
var stormB = [0x12345678, 3.9, -0];
var stormObj = [undefined, { valueOf: function () { return 0x80000005; } }, -3.9];
var stormMisc = [{ valueOf: function () { return 0xf0f0f0f0; } }, null, NaN];

for (var t = 0; t < 300; t++) {
    var m1 = mixBitwise(stormA, stormB);
    assertIs(m1[0], -512698476, "storm mixBitwise[0]");
    assertIs(m1[1], 0, "storm mixBitwise[1]");
    assertIs(m1[2], 0, "storm mixBitwise[2]");

    var m2 = mixBitwise(stormObj, stormMisc);
    assertIs(m2[0], -252645136, "stormObj mixBitwise[0]");
    assertIs(m2[1], 83886080, "stormObj mixBitwise[1]");
    assertIs(m2[2], 2, "stormObj mixBitwise[2]");

    var pnz = preserveNegZero(-0);
    assertIs(pnz[0], 0, "negZero bitor");
    assertIs(pnz[1], -0, "negZero kept");
    assertIs(pnz[2], -Infinity, "negZero 1/-0");

    assertIs(warmCaller(stormA, stormB), 1634785175, "storm warmCaller");
    assertIs(warmCaller({ length: 3, 0: 0xf34481ec, 1: 0x80000000, 2: 0xffffffff }, stormB), 1634785175, "non-array caller");
    assertIs(warmCaller([1, 2], stormB), -1, "irregular length");

    u32[0] = 0x80000000;
    u32[1] = 3.9;
    assertIs(u32[0], 0x80000000, "u32[0]");
    assertIs(u32[1], 3, "u32[1]");
}

for (var bi = 0; bi < 40; bi++) {
    assertIs(xorBigIntOrInt(0x1ffffffffn, 0x0f0f0f0fn), 0x1f0f0f0f0n, "BigInt xor");
    var threw = false;
    try {
        xorBigIntOrInt(1n, 2);
    } catch (e) {
        threw = (e instanceof TypeError);
    }
    assertIs(threw, true, "mixed BigInt xor throws TypeError");
}

maybeGC();

// 3. asm.js external + internal tier-up correctness.
function AsmSmoke(stdlib, foreign, buffer) {
    "use asm";
    var HEAP32 = new stdlib.Int32Array(buffer);
    var fround = stdlib.Math.fround;
    var imul = stdlib.Math.imul;
    var ffi = foreign.ffi;

    function addSigned(a, b) {
        a = a | 0;
        b = b | 0;
        return (a + b) | 0;
    }

    function addDouble(x, y) {
        x = +x;
        y = +y;
        return +(x * 1.5 + y);
    }

    function addFloat(x, y) {
        x = fround(x);
        y = fround(y);
        return fround(fround(x + y) * fround(1.5));
    }

    function sum10(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9) {
        a0 = a0 | 0;
        a1 = a1 | 0;
        a2 = a2 | 0;
        a3 = a3 | 0;
        a4 = a4 | 0;
        a5 = a5 | 0;
        a6 = a6 | 0;
        a7 = a7 | 0;
        a8 = a8 | 0;
        a9 = a9 | 0;
        return (((((a0 + a1) | 0) + ((a2 + a3) | 0)) | 0) +
                (((((a4 + a5) | 0) + ((a6 + a7) | 0)) | 0) + ((a8 + a9) | 0)) | 0) | 0;
    }

    function opAdd(a, b) { a = a | 0; b = b | 0; return (a + b) | 0; }
    function opMul(a, b) { a = a | 0; b = b | 0; return imul(a, b) | 0; }
    var tbl = [opAdd, opMul];

    function indirect(sel, a, b) {
        sel = sel | 0;
        a = a | 0;
        b = b | 0;
        return tbl[sel & 1](a, b) | 0;
    }

    function bumpHeap(idx, v) {
        idx = idx | 0;
        v = v | 0;
        var off = 0;
        off = (idx & 255) << 2;
        HEAP32[off >> 2] = ((HEAP32[off >> 2] | 0) + v) | 0;
        return HEAP32[off >> 2] | 0;
    }

    function innerStep(x) {
        x = x | 0;
        return ((x * 3) | 0) + 1 | 0;
    }

    function outerOnce(rounds) {
        rounds = rounds | 0;
        var i = 0;
        var acc = 0;
        for (i = 0; (i | 0) < (rounds | 0); i = (i + 1) | 0) {
            acc = (acc + (innerStep(i) | 0)) | 0;
        }
        return acc | 0;
    }

    function callFfi(n) {
        n = n | 0;
        return ffi(n | 0) | 0;
    }

    return {
        addSigned: addSigned,
        addDouble: addDouble,
        addFloat: addFloat,
        sum10: sum10,
        indirect: indirect,
        bumpHeap: bumpHeap,
        outerOnce: outerOnce,
        callFfi: callFfi
    };
}

var buf = new ArrayBuffer(0x1000);
var view = new Int32Array(buf);
var mod = AsmSmoke(
    { Math: Math, Int32Array: Int32Array },
    { ffi: function (n) { if ((n % 50) === 0) maybeGC(); return (n * 2 + 1) | 0; } },
    buf
);

for (var k = 0; k < 250; k++) {
    assertIs(mod.addSigned(k, 7), (k + 7) | 0, "asm addSigned");
    assertClose(mod.addDouble(k, 0.25), k * 1.5 + 0.25, 1e-12, "asm addDouble");
    assertIs(mod.addFloat(k, 1.5), Math.fround(Math.fround(k + 1.5) * Math.fround(1.5)), "asm addFloat");
    assertIs(mod.sum10(k, 1, 2, 3, 4, 5, 6, 7, 8, 9), (k + 45) | 0, "asm sum10");
    assertIs(mod.indirect(k & 1, k, 3), (k & 1) ? Math.imul(k, 3) : ((k + 3) | 0), "asm indirect");
    assertIs(mod.bumpHeap(3, 2), (k + 1) * 2, "asm bumpHeap");
    assertIs(view[3], (k + 1) * 2, "asm heap view");
    assertIs(mod.callFfi(k), (k * 2 + 1) | 0, "asm callFfi");
}

var expectedOuter = 0;
for (var o = 0; o < 60; o++) {
    expectedOuter = (expectedOuter + ((o * 3 + 1) | 0)) | 0;
}
assertIs(mod.outerOnce(60), expectedOuter, "asm outerOnce internal tier-up");

print("PASS");
