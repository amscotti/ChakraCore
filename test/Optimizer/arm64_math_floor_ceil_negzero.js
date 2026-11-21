//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for ARM64 LowererMD::GenerateFastInlineBuiltInMathFloorCeil
// (-0 preservation across (-1, -0], ±0, ±NaN, ±Infinity, ±0x80000000, ±2^32,
// subnormals, ignoreNegativeZero, shared bailout, SimpleJIT, FullJIT, and LoopBody JIT).

function formatNum(v) {
    if (Object.is(v, -0)) {
        return "-0";
    }
    if (v === 0) {
        return "+0";
    }
    return String(v);
}

function assertSameValue(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(
            "Assertion failed (" + msg + "): expected " +
            formatNum(expected) + ", got " + formatNum(actual)
        );
    }
}

var dv = new DataView(new ArrayBuffer(8));
dv.setUint32(0, 0x7ff80000, false);
dv.setUint32(4, 0x00000000, false);
var posNaN = dv.getFloat64(0, false);

dv.setUint32(0, 0xfff80000, false);
dv.setUint32(4, 0x00000000, false);
var negNaN = dv.getFloat64(0, false);

// [input, expectedCeil, expectedFloor]
var cases = [
    [-0, -0, -0],
    [-5e-324, -0, -1],
    [-1e-308, -0, -1],
    [-1e-100, -0, -1],
    [-0.1, -0, -1],
    [-0.5, -0, -1],
    [-0.9, -0, -1],
    [-(1 - Number.EPSILON / 2), -0, -1],

    [0, 0, 0],
    [Number.MIN_VALUE, 1, 0],
    [1e-308, 1, 0],
    [0.1, 1, 0],
    [0.5, 1, 0],
    [0.9, 1, 0],
    [1 - Number.EPSILON / 2, 1, 0],

    [-1, -1, -1],
    [-1.5, -1, -2],
    [1, 1, 1],
    [1.5, 2, 1],

    [NaN, NaN, NaN],
    [posNaN, NaN, NaN],
    [negNaN, NaN, NaN],

    [Infinity, Infinity, Infinity],
    [-Infinity, -Infinity, -Infinity],

    [0x7fffffff, 0x7fffffff, 0x7fffffff],
    [0x7fffffff + 0.5, 0x80000000, 0x7fffffff],
    [0x80000000, 0x80000000, 0x80000000],
    [0x80000000 + 0.5, 0x80000001, 0x80000000],

    [-0x80000000, -0x80000000, -0x80000000],
    [-0x80000000 + 0.5, -0x7fffffff, -0x80000000],
    [-0x80000000 - 0.5, -0x80000000, -0x80000001],
    [-0x80000001, -0x80000001, -0x80000001],

    [4294967296, 4294967296, 4294967296],
    [4294967296.5, 4294967297, 4294967296],
    [-4294967296, -4294967296, -4294967296],
    [-4294967296.5, -4294967296, -4294967297]
];

function jitCeil(x) {
    return Math.ceil(x);
}

function jitFloor(x) {
    return Math.floor(x);
}

function jitCeilInt(x) {
    return Math.ceil(x) | 0;
}

function jitFloorInt(x) {
    return Math.floor(x) | 0;
}

function jitCeilFloorPair(x, y) {
    var a = Math.ceil(x);
    var b = Math.floor(y);
    return [a, b];
}

// 1. Warm functions on positive fractional values so SimpleJIT and FullJIT
//    compile the Float64->Int32 fast path cleanly before edge-case inputs.
for (var w = 0; w < 5000; w++) {
    var warmX = 0.5 + (w & 7);
    assertSameValue(jitCeil(warmX), 1 + (w & 7), "warm jitCeil");
    assertSameValue(jitFloor(warmX), w & 7, "warm jitFloor");
    assertSameValue(jitCeilInt(warmX), 1 + (w & 7), "warm jitCeilInt");
    assertSameValue(jitFloorInt(warmX), w & 7, "warm jitFloorInt");
    var pair = jitCeilFloorPair(warmX, warmX);
    assertSameValue(pair[0], 1 + (w & 7), "warm pair ceil");
    assertSameValue(pair[1], w & 7, "warm pair floor");
}

// 2. Run multiple rounds across all edge cases (covering pre/post bailout).
for (var round = 0; round < 5; round++) {
    for (var i = 0; i < cases.length; i++) {
        var input = cases[i][0];
        var expCeil = cases[i][1];
        var expFloor = cases[i][2];
        var label = "round=" + round + " input=" + formatNum(input);

        assertSameValue(jitCeil(input), expCeil, "jitCeil " + label);
        assertSameValue(jitFloor(input), expFloor, "jitFloor " + label);
        assertSameValue(jitCeilInt(input), expCeil | 0, "jitCeilInt " + label);
        assertSameValue(jitFloorInt(input), expFloor | 0, "jitFloorInt " + label);

        var resPair = jitCeilFloorPair(input, input);
        assertSameValue(resPair[0], expCeil, "pair ceil " + label);
        assertSameValue(resPair[1], expFloor, "pair floor " + label);
    }
}

// 3. Dedicated LoopBody JIT test: warm the loop body on positive inputs, then
//    feed negative-zero, subnormal, NaN, Inf, and 32-bit boundary values in-loop.
function testLoopBody() {
    var n = 6000;
    var inputs = new Array(n);
    var expCeils = new Array(n);
    var expFloors = new Array(n);

    for (var i = 0; i < n; i++) {
        if (i < 5000) {
            var v = (i & 7) + 0.25;
            inputs[i] = v;
            expCeils[i] = (i & 7) + 1;
            expFloors[i] = (i & 7);
        } else {
            var entry = cases[(i - 5000) % cases.length];
            inputs[i] = entry[0];
            expCeils[i] = entry[1];
            expFloors[i] = entry[2];
        }
    }

    var outCeil = new Array(n);
    var outFloor = new Array(n);
    var outCeilInt = new Array(n);
    var outFloorInt = new Array(n);

    for (var k = 0; k < n; k++) {
        var x = inputs[k];
        outCeil[k] = Math.ceil(x);
        outFloor[k] = Math.floor(x);
        outCeilInt[k] = Math.ceil(x) | 0;
        outFloorInt[k] = Math.floor(x) | 0;
    }

    for (var j = 0; j < n; j++) {
        assertSameValue(outCeil[j], expCeils[j], "loopBody ceil k=" + j + " x=" + formatNum(inputs[j]));
        assertSameValue(outFloor[j], expFloors[j], "loopBody floor k=" + j + " x=" + formatNum(inputs[j]));
        assertSameValue(outCeilInt[j], expCeils[j] | 0, "loopBody ceilInt k=" + j);
        assertSameValue(outFloorInt[j], expFloors[j] | 0, "loopBody floorInt k=" + j);
    }
}
testLoopBody();

// 4. Mirror Test262 S15.8.2.6_A7 / S15.8.2.9_A7 identity in a warmed loop.
function testNegatedIdentityLoop() {
    for (var iter = 0; iter < 10; iter++) {
        for (var i = -1000; i < 1000; i++) {
            var x = i / 10.0;
            assertSameValue(Math.ceil(x), -Math.floor(-x), "ceil vs -floor(-x) i=" + i);
            assertSameValue(Math.floor(x), -Math.ceil(-x), "floor vs -ceil(-x) i=" + i);
        }
    }
}
testNegatedIdentityLoop();

print("PASS");
