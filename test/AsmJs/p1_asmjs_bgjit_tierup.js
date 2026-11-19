//-------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, msg) {
    if (actual !== expected && !(Number.isNaN(actual) && Number.isNaN(expected))) {
        throw new Error("FAIL (" + msg + "): expected " + expected + ", got " + actual);
    }
}

function AsmTierUpModule(stdlib, foreign, buffer) {
    "use asm";

    var HEAP32 = new stdlib.Int32Array(buffer);
    var fround = stdlib.Math.fround;
    var imul = stdlib.Math.imul;
    var ffiCallback = foreign.ffiCallback;

    function addSigned(a, b) {
        a = a | 0;
        b = b | 0;
        var sum = 0;
        var i = 0;
        sum = (a + b) | 0;
        for (i = 0; (i | 0) < 4; i = (i + 1) | 0) {
            sum = (sum + i) | 0;
        }
        return sum | 0;
    }

    function addDouble(x, y) {
        x = +x;
        y = +y;
        return +((x * 1.5) + y);
    }

    function addFloat(x, y) {
        x = fround(x);
        y = fround(y);
        return fround(fround(x + y) * fround(1.5));
    }

    function bumpHeapVoid(index, delta) {
        index = index | 0;
        delta = delta | 0;
        var byteIdx = 0;
        var cur = 0;
        byteIdx = (index & 1023) << 2;
        cur = HEAP32[byteIdx >> 2] | 0;
        HEAP32[byteIdx >> 2] = (cur + delta) | 0;
    }

    function readHeap(index) {
        index = index | 0;
        var byteIdx = 0;
        byteIdx = (index & 1023) << 2;
        return HEAP32[byteIdx >> 2] | 0;
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

    function opAdd(a, b) {
        a = a | 0;
        b = b | 0;
        return (a + b) | 0;
    }

    function opSub(a, b) {
        a = a | 0;
        b = b | 0;
        return (a - b) | 0;
    }

    function opMul(a, b) {
        a = a | 0;
        b = b | 0;
        return imul(a, b) | 0;
    }

    function opXor(a, b) {
        a = a | 0;
        b = b | 0;
        return (a ^ b) | 0;
    }

    function dblScale(x) {
        x = +x;
        return +(x * 2.5);
    }

    function dblShift(x) {
        x = +x;
        return +(x + 7.25);
    }

    function indirectSigned(sel, a, b) {
        sel = sel | 0;
        a = a | 0;
        b = b | 0;
        return intTable[sel & 3](a, b) | 0;
    }

    function indirectDouble(sel, x) {
        sel = sel | 0;
        x = +x;
        return +dblTable[sel & 1](x);
    }

    function callFfi(n) {
        n = n | 0;
        return ffiCallback(n | 0) | 0;
    }

    function internalBatch(rounds) {
        rounds = rounds | 0;
        var i = 0;
        var acc = 0;
        for (i = 0; (i | 0) < (rounds | 0); i = (i + 1) | 0) {
            acc = (acc + (intTable[i & 3](i, 3) | 0)) | 0;
        }
        return acc | 0;
    }

    function innerHotSigned(x) {
        x = x | 0;
        var k = 0;
        var s = 0;
        s = x;
        for (k = 0; (k | 0) < 5; k = (k + 1) | 0) {
            s = (s + ((k * 3) | 0)) | 0;
        }
        return s | 0;
    }

    function innerHotDouble(x) {
        x = +x;
        return +(x * 1.25 + 0.5);
    }

    // Called once from JS so outerOnce remains in AsmJsInterpreter while
    // inner callees exceed callcount 10 and tier up via TryCallAsmJsJit.
    function outerOnce(rounds) {
        rounds = rounds | 0;
        var i = 0;
        var acc = 0;
        var dacc = 0.0;
        for (i = 0; (i | 0) < (rounds | 0); i = (i + 1) | 0) {
            acc = (acc + (innerHotSigned(i) | 0)) | 0;
            dacc = +(dacc + +innerHotDouble(+(i | 0)));
        }
        return (acc + ~~dacc) | 0;
    }

    function recurseDepth(n) {
        n = n | 0;
        if ((n | 0) <= 0) {
            return 1;
        }
        return ((recurseDepth((n - 1) | 0) | 0) + 1) | 0;
    }

    var intTable = [opAdd, opSub, opMul, opXor];
    var dblTable = [dblScale, dblShift];

    return {
        addSigned: addSigned,
        addDouble: addDouble,
        addFloat: addFloat,
        bumpHeapVoid: bumpHeapVoid,
        readHeap: readHeap,
        sum10: sum10,
        indirectSigned: indirectSigned,
        indirectDouble: indirectDouble,
        callFfi: callFfi,
        internalBatch: internalBatch,
        outerOnce: outerOnce,
        recurseDepth: recurseDepth
    };
}

var gcCount = 0;
var trash = [];
function ffiCallback(n) {
    // Allocate short-lived garbage on each FFI callback and run explicit GC
    // when exposed (e.g., under -CollectGarbage) across tier-up transitions.
    var obj = { iter: n, payload: [n, n + 1, n + 2, n + 3] };
    trash.push(obj);
    if (trash.length > 32) {
        trash.shift();
    }
    if ((n % 5 === 0) && typeof CollectGarbage === "function") {
        CollectGarbage();
        gcCount++;
    }
    return (n * 3 + 1) | 0;
}

var heap = new ArrayBuffer(0x10000);
var heapView = new Int32Array(heap);
var stdlib = {
    Math: Math,
    Int8Array: Int8Array,
    Int16Array: Int16Array,
    Int32Array: Int32Array,
    Uint8Array: Uint8Array,
    Uint16Array: Uint16Array,
    Uint32Array: Uint32Array,
    Float32Array: Float32Array,
    Float64Array: Float64Array,
    Infinity: Infinity,
    NaN: NaN
};
var mod = AsmTierUpModule(stdlib, { ffiCallback: ffiCallback }, heap);

// Exercise 300+ external calls per export so background JIT is scheduled (~10),
// polled while pending/completed, and executed on native JIT code.
for (var i = 0; i < 300; i++) {
    assertEq(mod.addSigned(i, 10), (i + 16) | 0, "addSigned iter " + i);
    assertEq(mod.addDouble(i, 0.25), i * 1.5 + 0.25, "addDouble iter " + i);
    assertEq(mod.addFloat(i, 1.5), Math.fround(Math.fround(i + 1.5) * Math.fround(1.5)), "addFloat iter " + i);

    var voidRet = mod.bumpHeapVoid(7, 2);
    assertEq(voidRet, undefined, "bumpHeapVoid return iter " + i);
    assertEq(mod.readHeap(7), (i + 1) * 2, "readHeap iter " + i);
    assertEq(heapView[7], (i + 1) * 2, "heapView[7] iter " + i);

    assertEq(mod.sum10(i, 1, 2, 3, 4, 5, 6, 7, 8, 9), (i + 45) | 0, "sum10 iter " + i);

    var sel = i & 3;
    var expectedInd = sel === 0 ? ((i + 5) | 0) :
                      sel === 1 ? ((i - 5) | 0) :
                      sel === 2 ? Math.imul(i, 5) :
                                  ((i ^ 5) | 0);
    assertEq(mod.indirectSigned(sel, i, 5), expectedInd, "indirectSigned iter " + i);

    var dSel = i & 1;
    var expectedDbl = dSel === 0 ? (i * 2.5) : (i + 7.25);
    assertEq(mod.indirectDouble(dSel, i), expectedDbl, "indirectDouble iter " + i);

    assertEq(mod.callFfi(i), (i * 3 + 1) | 0, "callFfi iter " + i);
}

// Exercise internal calls inside a single external invocation as well as multiple batches.
var expectedBatch = 0;
for (var j = 0; j < 200; j++) {
    var bSel = j & 3;
    expectedBatch = (expectedBatch + (bSel === 0 ? (j + 3) : bSel === 1 ? (j - 3) : bSel === 2 ? Math.imul(j, 3) : (j ^ 3))) | 0;
}
for (var rep = 0; rep < 30; rep++) {
    assertEq(mod.internalBatch(200), expectedBatch, "internalBatch rep " + rep);
}

// Single external call to outerOnce keeps outerOnce interpreted while inner
// callees tier up across TryCallAsmJsJit -> CheckAsmJsCodeGen.
var expectedOuterInt = 0;
var expectedOuterDbl = 0.0;
for (var k = 0; k < 60; k++) {
    expectedOuterInt = (expectedOuterInt + k + 30) | 0;
    expectedOuterDbl += (k * 1.25 + 0.5);
}
assertEq(mod.outerOnce(60), (expectedOuterInt + (expectedOuterDbl | 0)) | 0, "outerOnce single call");

// Tier up recurseDepth via shallow calls, then verify deep recursion trips
// ThreadContext::WasmStackReserveFloorGuard cleanly with RangeError and recovers.
for (var r = 0; r < 250; r++) {
    assertEq(mod.recurseDepth(5), 6, "shallow recurseDepth iter " + r);
}

var caughtStackOverflow = false;
try {
    mod.recurseDepth(100000);
} catch (e) {
    caughtStackOverflow = (e instanceof RangeError) ||
                          ((e instanceof Error) && /Out of stack space/i.test(e.message));
    if (!caughtStackOverflow) {
        throw e;
    }
}
assertEq(caughtStackOverflow, true, "deep recursion must throw stack overflow (RangeError / Error: Out of stack space)");
assertEq(mod.recurseDepth(5), 6, "post-RangeError recovery call");
assertEq(mod.addSigned(40, 2), 48, "post-RangeError signed export call");

if (typeof CollectGarbage === "function" && gcCount === 0) {
    throw new Error("Expected CollectGarbage() to have run in FFI callback");
}

WScript.Echo("PASS");
