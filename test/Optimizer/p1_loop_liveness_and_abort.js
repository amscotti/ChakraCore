//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify JITLoopBody upward-exposed liveness, compound element key
// conversion (BrOnObject_A + Conv_Prop + Ld_A_ReuseLoc) in loops/FullJIT,
// bailout value restoration, with-scope call probes (464c2eb73 shapes),
// GC pressure, and bounded permanent loop-body aborts without retry storms.

function assertEqual(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + ": expected " + expected + ", got " + actual);
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

// 1. Bitwise sieve (0xffffffff) exercising loop-body compound &= and upward-exposed locals.
function runBitwiseSieve(m) {
    var size = (m >> 5) + 1;
    var isPrime = new Array(size);
    for (var k = 0; k < size; k++) {
        isPrime[k] = 0xffffffff;
    }
    var limit = m;
    for (var i = 2; i <= limit; i++) {
        if ((isPrime[i >> 5] & (1 << (i & 31))) !== 0) {
            for (var j = i + i; j <= limit; j += i) {
                isPrime[j >> 5] &= ~(1 << (j & 31));
            }
        }
    }
    var count = 0;
    for (var p = 2; p <= limit; p++) {
        if ((isPrime[p >> 5] & (1 << (p & 31))) !== 0) {
            count++;
        }
    }
    return count;
}

for (var r = 0; r < 4; r++) {
    assertEqual(runBitwiseSieve(4096), 564, "sieve 4096 iter " + r);
}

// Bailout mid-loop during sieve via late polymorphic key / array hole transition
// while pre-loop upward-exposed locals remain intact.
function sieveWithLateBailout(arr, n, preGuard, bailAt) {
    var savedA = preGuard + 10;
    var savedB = preGuard * 3;
    var sum = 0;
    for (var i = 2; i < n; i++) {
        var idx = i === bailAt ? { toString: function () { sum += 1000; return "1"; } } : (i >> 2);
        arr[idx] &= ~(1 << (i & 7));
        sum += 1;
    }
    return sum + savedA + savedB;
}

var bailArr = new Array(1024);
for (var bi = 0; bi < bailArr.length; bi++) {
    bailArr[bi] = 0xffffffff;
}
assertEqual(sieveWithLateBailout(bailArr, 2500, 7, -1), 2498 + 17 + 21, "sieve bailout warmup");
assertEqual(sieveWithLateBailout(bailArr, 2500, 7, 2400), 3498 + 17 + 21, "sieve bailout trigger");

// 2. Float-array FFT butterfly math (Array & Float64Array) with += compound ops.
function fftButterfly(real, imag, n) {
    var halfSize = 1;
    while (halfSize < n) {
        var step = (halfSize << 1);
        var theta = -Math.PI / halfSize;
        var wStepReal = Math.cos(theta);
        var wStepImag = Math.sin(theta);
        var wReal = 1.0;
        var wImag = 0.0;
        for (var fftStep = 0; fftStep < halfSize; fftStep++) {
            var i = fftStep;
            while (i < n) {
                var off = i + halfSize;
                var tr = wReal * real[off] - wImag * imag[off];
                var ti = wReal * imag[off] + wImag * real[off];
                real[off] = real[i] - tr;
                imag[off] = imag[i] - ti;
                real[i] += tr;
                imag[i] += ti;
                i += step;
            }
            var tmpReal = wReal;
            wReal = tmpReal * wStepReal - wImag * wStepImag;
            wImag = tmpReal * wStepImag + wImag * wStepReal;
        }
        halfSize = step;
    }
}

for (var iter = 0; iter < 5; iter++) {
    var N = 256;
    var re = new Array(N);
    var im = new Array(N);
    var fRe = new Float64Array(N);
    var fIm = new Float64Array(N);
    for (var idx = 0; idx < N; idx++) {
        var v = Math.sin(idx * 0.25) + 0.5 * Math.cos(idx * 0.5);
        re[idx] = v;
        im[idx] = 0.0;
        fRe[idx] = v;
        fIm[idx] = 0.0;
    }
    fftButterfly(re, im, N);
    fftButterfly(fRe, fIm, N);
    var energyJSArr = 0.0;
    var energyTArr = 0.0;
    for (var idx2 = 0; idx2 < N; idx2++) {
        assertClose(re[idx2], fRe[idx2], 1e-9, "FFT real match at " + idx2);
        assertClose(im[idx2], fIm[idx2], 1e-9, "FFT imag match at " + idx2);
        energyJSArr += re[idx2] * re[idx2] + im[idx2] * im[idx2];
        energyTArr += fRe[idx2] * fRe[idx2] + fIm[idx2] * fIm[idx2];
    }
    assertClose(energyJSArr, energyTArr, 1e-6, "FFT Parseval energy");
}

maybeGC();

// 3. Compound element assignment in JITLoopBody & FullJIT with object keys
//    vs primitive keys (verifies BrOnObject_A never inverts and Conv_Prop runs once).
function compoundMixedKey(o, k) {
    o[k] += 3;
    o[k]++;
    return o[k];
}

var keyHits = 0;
var objKey = {
    toString: function () {
        keyHits++;
        return "prop";
    }
};
var mixedObj = { prop: 0, "0": 0, "1": 0, "2": 0, "3": 0 };
for (var m = 0; m < 1000; m++) {
    if ((m & 3) === 0) {
        compoundMixedKey(mixedObj, objKey);
    } else {
        compoundMixedKey(mixedObj, m & 3);
    }
}
assertEqual(keyHits, 750, "ToPropertyKey called three times (once each for +=, ++, and return) when object key");
assertEqual(mixedObj.prop, 1000, "mixed object key sum");
assertEqual(mixedObj[1], 1000, "mixed primitive key 1 sum");
assertEqual(mixedObj[2], 1000, "mixed primitive key 2 sum");
assertEqual(mixedObj[3], 1000, "mixed primitive key 3 sum");

// 4. Inlined with-scope call targets (464c2eb73 shapes) inside & outside loops.
(function testWithScopeInLoop() {
    var x = 1;
    function f1(v) { return v + 1; }
    var sum = 0;
    for (var i = 0; i < 600; i++) {
        var scopeObj = i < 550 ? { f1: f1 } : { other: x };
        with (scopeObj) {
            if (typeof f1 === "function") {
                sum += f1(1);
            }
        }
    }
    assertEqual(sum, 1200, "with-scope call target in loop");

    var sumShadow = 0;
    for (var j = 0; j < 600; j++) {
        var shadowObj = j < 550 ? { f1: function (v) { return v + 2; } } : { f1: undefined };
        with (shadowObj) {
            if (typeof f1 === "function") {
                sumShadow += f1(1);
            }
        }
    }
    assertEqual(sumShadow, 1650, "with-scope shadowed call target in loop");

    // Direct 464c2eb73 nested with-recursion bounded stack shape inside a loop
    var caught = 0;
    for (var t = 0; t < 20; t++) {
        try {
            (function TestFunc() {
                var a = t;
                (function outer(depth) {
                    (function inner() { return a; })();
                    if (depth > 0) {
                        with ({}) { outer(depth - 1); }
                    }
                })(15);
            })();
        } catch (e) {
            caught++;
        }
    }
    assertEqual(caught, 0, "bounded with recursion stays crash-free");
})();

// 5. Lazy/loop bailout with pre-loop locals and loop-invariant calls.
function helperId(v) {
    return v + 1;
}

function loopUpwardExposedBailout(limit, trigger) {
    var pre1 = 100;
    var pre2 = 200;
    var acc = 0;
    for (var i = 0; i < limit; i++) {
        if (i === trigger) {
            acc += helperId(pre1) + pre2;
        } else {
            acc += 1;
        }
    }
    return acc + pre1 + pre2;
}

for (var b = 0; b < 10; b++) {
    assertEqual(loopUpwardExposedBailout(1500, -1), 1800, "loop upward-exposed warmup");
}
assertEqual(loopUpwardExposedBailout(1500, 1400), 1499 + 101 + 200 + 300, "loop upward-exposed bailout");

maybeGC();

// 6. Permanently-aborted JITLoopBody must mark the LoopHeader unjittable once
//    and never hang in a per-iteration GenerateLoopBody retry storm.
function abortedEvalLoop(n) {
    var acc = 0;
    for (var i = 0; i < n; i++) {
        if (i < 0) {
            eval("acc++");
        }
        acc += (i & 7);
    }
    return acc;
}
assertEqual(abortedEvalLoop(25000), 87500, "permanently-aborted eval loop completes cleanly");

class PrivateBrandLoop {
    #brand = 1;
    run(obj, n) {
        var acc = 0;
        for (var i = 0; i < n; i++) {
            if (#brand in obj) {
                acc += 1;
            }
        }
        return acc;
    }
}
var pbl = new PrivateBrandLoop();
assertEqual(pbl.run(pbl, 25000), 25000, "permanently-aborted private brand loop completes cleanly");

print("PASS");
