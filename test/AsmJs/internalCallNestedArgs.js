//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression coverage for the ARM64 AsmJsCallI x0 convention: every internal
// asm.js call must present the callee function object in x0 (the callee homes
// it as its module/heap anchor). Exercises nested internal calls whose callers
// carry enough live locals that arguments are materialized from the frame
// under register pressure. Verified against Octane zlib (deterministic FullJIT
// SIGSEGV before the fix, SCORE 69085 after).

function asmModuleNestedCalls(stdlib, ffi, heap) {
    "use asm";
    var H = new stdlib.Int32Array(heap);
    function leaf(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9) {
        p0 = p0 | 0; p1 = p1 | 0; p2 = p2 | 0; p3 = p3 | 0; p4 = p4 | 0;
        p5 = p5 | 0; p6 = p6 | 0; p7 = p7 | 0; p8 = p8 | 0; p9 = p9 | 0;
        var s = 0;
        s = ((H[p0 >> 2] | 0) + (H[p1 >> 2] | 0)) | 0;
        s = (((s + (H[p2 >> 2] | 0)) | 0) + (H[p3 >> 2] | 0)) | 0;
        s = (((s + (H[p4 >> 2] | 0)) | 0) + (H[p5 >> 2] | 0)) | 0;
        s = (((s + (H[p6 >> 2] | 0)) | 0) + (H[p7 >> 2] | 0)) | 0;
        s = (((s + (H[p8 >> 2] | 0)) | 0) + (H[p9 >> 2] | 0)) | 0;
        return s | 0;
    }
    function mid(q0, q1, q2, q3, q4, q5) {
        q0 = q0 | 0; q1 = q1 | 0; q2 = q2 | 0; q3 = q3 | 0; q4 = q4 | 0; q5 = q5 | 0;
        return (leaf(q0 | 0, q1 | 0, q2 | 0, q3 | 0, q4 | 0, q5 | 0, q0 | 0, q1 | 0, q2 | 0, q3 | 0) | 0);
    }
    function outer(x) {
        x = x | 0;
        var l0 = 0, l1 = 0, l2 = 0, l3 = 0, l4 = 0, l5 = 0, l6 = 0, l7 = 0, l8 = 0, l9 = 0;
        var m0 = 0, m1 = 0, m2 = 0, m3 = 0, m4 = 0, m5 = 0, m6 = 0, m7 = 0, m8 = 0, m9 = 0;
        var s = 0, t = 0;
        l0 = (x + 0) | 0; l1 = (x + 4) | 0; l2 = (x + 8) | 0; l3 = (x + 12) | 0; l4 = (x + 16) | 0;
        l5 = (x + 20) | 0; l6 = (x + 24) | 0; l7 = (x + 28) | 0; l8 = (x + 32) | 0; l9 = (x + 36) | 0;
        m0 = (x + 40) | 0; m1 = (x + 44) | 0; m2 = (x + 48) | 0; m3 = (x + 52) | 0; m4 = (x + 56) | 0;
        m5 = (x + 60) | 0; m6 = (x + 64) | 0; m7 = (x + 68) | 0; m8 = (x + 72) | 0; m9 = (x + 76) | 0;
        s = (mid(l0 | 0, l1 | 0, l2 | 0, l3 | 0, l4 | 0, l5 | 0) | 0);
        t = (((s + l6) | 0) + l7) | 0;
        t = (((t + l8) | 0) + l9) | 0;
        t = (((t + m0) | 0) + m1) | 0;
        t = (((t + m2) | 0) + m3) | 0;
        t = (((t + m4) | 0) + m5) | 0;
        t = (((t + m6) | 0) + m7) | 0;
        t = (((t + m8) | 0) + m9) | 0;
        return t | 0;
    }
    return { run: outer };
}

var buf = new ArrayBuffer(65536);
var ia = new Int32Array(buf);
for (var i = 0; i < 60; i++) {
    ia[25 + i] = i + 1;
}
var mod = asmModuleNestedCalls(this, {}, buf);
function ref(x) {
    var leafIdx = [0, 1, 2, 3, 4, 5, 0, 1, 2, 3];
    var s = 0;
    for (var i = 0; i < 10; i++) {
        s += ia[((x + leafIdx[i] * 4) >> 2)];
    }
    for (var k = 24; k <= 76; k += 4) {
        s += x + k;
    }
    return s | 0;
}
var inputs = [100, 104, 200];
var pass = true;
for (var t = 0; t < inputs.length; t++) {
    var got = mod.run(inputs[t]);
    var want = ref(inputs[t]);
    print("run(" + inputs[t] + ") = " + got + " want " + want);
    if (got !== want) {
        pass = false;
    }
}
print(pass ? "PASS" : "FAIL");
