//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for ARM64 GenerateFastInlineBuiltInMathRound handling
// values in [-0.5, 0.5), especially 0.49999999999999994 (0.5 - 2^-54),
// where adding 0.5 rounds up to 1.0 in IEEE-754 double precision.

function roundVal(x) {
    return Math.round(x);
}

function roundInt(x) {
    return Math.round(x) | 0;
}

function sameNumber(a, b) {
    if (Number.isNaN(b)) {
        return Number.isNaN(a);
    }
    if (a === 0 && b === 0) {
        return (1 / a) === (1 / b);
    }
    return a === b;
}

function runChecks() {
    var cases = [
        [0.49999999999999994, 0, 0],
        [0.4999999999999999, 0, 0],
        [0.5, 1, 1],
        [0.0, 0, 0],
        [-0.0, -0, 0],
        [-0.25, -0, 0],
        [-0.49999999999999994, -0, 0],
        [-0.5, -0, 0],
        [-0.5000000000000001, -1, -1],
        [1.4999999999999998, 1, 1],
        [1.5, 2, 2],
        [-1.5, -1, -1],
        [-1.5000000000000002, -2, -2],
        [2147483647.4999998, 2147483647, 2147483647],
        [2147483647.5, 2147483648, -2147483648],
        [-2147483648.5, -2147483648, -2147483648],
        [-2147483648.5000005, -2147483649, 2147483647],
        [NaN, NaN, 0],
        [-NaN, NaN, 0],
        [Infinity, Infinity, 0],
        [-Infinity, -Infinity, 0]
    ];

    for (var i = 0; i < cases.length; i++) {
        var x = cases[i][0];
        var expVal = cases[i][1];
        var expInt = cases[i][2];
        var gotVal = roundVal(x);
        var gotInt = roundInt(x);
        if (!sameNumber(gotVal, expVal)) {
            throw new Error("roundVal(" + x + ") expected " + expVal + " got " + gotVal);
        }
        if (gotInt !== expInt) {
            throw new Error("roundInt(" + x + ") expected " + expInt + " got " + gotInt);
        }
    }
}

// Warm up with normal floats first so int32 specialization stays intact, then check boundary cases.
for (var w = 0; w < 2000; w++) {
    if (roundVal((w % 7) + 0.25) !== (w % 7)) {
        throw new Error("warmup roundVal failed");
    }
    if (roundInt((w % 7) + 0.75) !== ((w % 7) + 1)) {
        throw new Error("warmup roundInt failed");
    }
}

for (var iter = 0; iter < 50; iter++) {
    runChecks();
}

print("PASS");
