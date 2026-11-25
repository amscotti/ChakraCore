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

// Keep return type non-int-specialized so Div_A uses GenerateFastDivByPow2.
function divBy2(x) {
    return x / 2;
}
function divBy4(x) {
    return x / 4;
}
function divBy8(x) {
    return x / 8;
}
function divBy1(x) {
    return x / 1;
}
function divBy2Pow30(x) {
    return x / 1073741824;
}
function divBy2InPlace(x) {
    var a = x / 2;
    var b = x / 4;
    return a + b;
}

// Warm up with both even and odd positive/negative values so Div_A stays Div_A (Float/Var).
var warmInputs = [0, 1, 2, 3, 4, -1, -2, -3, -4, 5, -5, 6, -6, 7, -7, 8, -8];
for (var w = 0; w < 600; w++) {
    var v = warmInputs[w % warmInputs.length];
    assertEqual(divBy2(v), v * 0.5, "warmup divBy2");
    assertEqual(divBy4(v), v * 0.25, "warmup divBy4");
    assertEqual(divBy8(v), v * 0.125, "warmup divBy8");
    assertEqual(divBy1(v), v, "warmup divBy1");
    assertEqual(divBy2Pow30(v), v / 1073741824, "warmup divBy2Pow30");
    assertEqual(divBy2InPlace(v), v * 0.5 + v * 0.25, "warmup divBy2InPlace");
}

var exactCases = [
    [0, 0, 0, 0, 0],
    [1, 0.5, 0.25, 0.125, 0.75],
    [-1, -0.5, -0.25, -0.125, -0.75],
    [2, 1, 0.5, 0.25, 1.5],
    [-2, -1, -0.5, -0.25, -1.5],
    [3, 1.5, 0.75, 0.375, 2.25],
    [-3, -1.5, -0.75, -0.375, -2.25],
    [4, 2, 1, 0.5, 3],
    [-4, -2, -1, -0.5, -3],
    [6, 3, 1.5, 0.75, 4.5],
    [-6, -3, -1.5, -0.75, -4.5],
    [12, 6, 3, 1.5, 9],
    [-12, -6, -3, -1.5, -9],
    [2147483647, 1073741823.5, 536870911.75, 268435455.875, 1610612735.25],
    [2147483646, 1073741823, 536870911.5, 268435455.75, 1610612734.5],
    [-2147483648, -1073741824, -536870912, -268435456, -1610612736],
    [-2147483647, -1073741823.5, -536870911.75, -268435455.875, -1610612735.25],
    [-2147483646, -1073741823, -536870911.5, -268435455.75, -1610612734.5],
    [-0, -0, -0, -0, -0],
    [NaN, NaN, NaN, NaN, NaN],
    [Infinity, Infinity, Infinity, Infinity, Infinity],
    [-Infinity, -Infinity, -Infinity, -Infinity, -Infinity],
    [1.5, 0.75, 0.375, 0.1875, 1.125],
    ["8", 4, 2, 1, 6],
];

for (var r = 0; r < 200; r++) {
    for (var i = 0; i < exactCases.length; i++) {
        var c = exactCases[i];
        assertEqual(divBy2(c[0]), c[1], "divBy2(" + c[0] + ")");
        assertEqual(divBy4(c[0]), c[2], "divBy4(" + c[0] + ")");
        assertEqual(divBy8(c[0]), c[3], "divBy8(" + c[0] + ")");
        assertEqual(divBy2InPlace(c[0]), c[4], "divBy2InPlace(" + c[0] + ")");
    }
    assertEqual(divBy2Pow30(-2147483648), -2, "INT32_MIN / 2^30");
    assertEqual(divBy2Pow30(1610612736), 1.5, "1.5 * 2^30 / 2^30");
    assertEqual(divBy2Pow30(-536870912), -0.5, "-0.5 * 2^30 / 2^30");
    assertEqual(divBy2Pow30(-1610612736), -1.5, "-1.5 * 2^30 / 2^30");
    assertEqual(divBy1(-0), -0, "-0 / 1");
    assertEqual(divBy1(-2147483648), -2147483648, "INT32_MIN / 1");
    assertThrowsTypeError(function () { return divBy2(1n); }, "1n / 2");
    assertThrowsTypeError(function () { return divBy4(4n); }, "4n / 4");
}

print("PASS");
