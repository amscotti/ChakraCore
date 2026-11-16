//-------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for ARM64 ENABLE_ASMJS inlining without ASMJS_PLAT:
// Inline::BuildIRForInlinee and Inline::InlineFunctionCommon must gate
// IRBuilderAsmJs and GetAsmJsInfo()->GetArgCount() on ENABLE_ASMJS (not
// ASMJS_PLAT) so inlining asm.js helpers with constant-table entries does not
// fall through to JS IRBuilder::BuildConstantLoads() and dereference null
// constTableContent.

function assert(cond, msg) {
    if (!cond) {
        print("asmjs-inline-const: FAIL: " + msg);
        WScript.Quit(1);
    }
}

function asmModule(stdlib, foreign, heap) {
    "use asm";
    var imul = stdlib.Math.imul;
    var I32 = new stdlib.Int32Array(heap);
    var F64 = new stdlib.Float64Array(heap);

    function helperInt(x, y) {
        x = x | 0;
        y = y | 0;
        var a = 0;
        a = imul(x ^ 0x13579bdf, 0x00010003) | 0;
        return (a + ((y + 1048573) | 0)) | 0;
    }

    function helperDouble(v) {
        v = +v;
        var scale = 1.625;
        var bias = 3.375;
        return +(v * scale + bias);
    }

    function helperMem(idx, val) {
        idx = idx | 0;
        val = +val;
        var cur = 0.0;
        var byteIdx = 0;
        byteIdx = (idx & 63) << 3;
        cur = +F64[byteIdx >> 3];
        cur = +(cur + val * 0.25 + 0.125);
        F64[byteIdx >> 3] = cur;
        byteIdx = (512 + ((idx & 63) << 2)) | 0;
        I32[byteIdx >> 2] = (((I32[byteIdx >> 2] | 0) + 7717) | 0);
        return +cur;
    }

    function outerLoop(iters, seed) {
        iters = iters | 0;
        seed = seed | 0;
        var i = 0;
        var acc = 0;
        var dacc = 0.0;
        acc = seed;
        for (i = 0; (i | 0) < (iters | 0); i = (i + 1) | 0) {
            acc = helperInt(acc | 0, i | 0) | 0;
            dacc = +helperDouble(+((acc & 1023) | 0));
            dacc = +helperMem(i | 0, +dacc);
        }
        return (acc + (~~dacc)) | 0;
    }

    return {
        helperInt: helperInt,
        helperDouble: helperDouble,
        outerLoop: outerLoop
    };
}

function jsCaller(mod, n) {
    var sum = 0;
    for (var i = 0; i < n; i++) {
        sum = (sum + mod.helperInt(i, 7)) | 0;
        if (i % 17 === 0) {
            sum = (sum + ~~mod.helperDouble(i * 0.5)) | 0;
        }
    }
    return sum | 0;
}

var heap = new ArrayBuffer(65536);
var mod = asmModule(this, {}, heap);

function refOuter(iters, seed) {
    var i32 = new Int32Array(new ArrayBuffer(65536));
    var f64 = new Float64Array(i32.buffer);
    var acc = seed | 0;
    var dacc = 0.0;
    for (var i = 0; i < iters; i++) {
        var a = Math.imul(acc ^ 0x13579bdf, 0x00010003) | 0;
        acc = (a + ((i + 1048573) | 0)) | 0;
        dacc = +((acc & 1023) * 1.625 + 3.375);
        var idx = i & 63;
        var cur = f64[idx];
        cur = +(cur + dacc * 0.25 + 0.125);
        f64[idx] = cur;
        i32[idx + 128] = (i32[idx + 128] + 7717) | 0;
        dacc = cur;
    }
    return (acc + (~~dacc)) | 0;
}

var expected = refOuter(2000, 42);
var actual = mod.outerLoop(2000, 42);
assert(actual === expected, "outerLoop mismatch: expected " + expected + ", got " + actual);

// Warm up multiple invocations and JS->asmjs calls to exercise FullJIT + inlining.
var expectedJs = 0;
for (var k = 0; k < 200; k++) {
    var a = Math.imul(k ^ 0x13579bdf, 0x00010003) | 0;
    expectedJs = (expectedJs + ((a + (7 + 1048573 | 0)) | 0)) | 0;
    if (k % 17 === 0) {
        expectedJs = (expectedJs + ~~(+((k * 0.5) * 1.625 + 3.375))) | 0;
    }
}
for (var r = 0; r < 40; r++) {
    var res = jsCaller(mod, 200);
    assert(res === expectedJs, "jsCaller mismatch at r=" + r + ": expected " + expectedJs + ", got " + res);
    var out = mod.outerLoop(250, r);
    assert(typeof out === "number" && (out | 0) === out, "outerLoop int result");
}

print("pass");
