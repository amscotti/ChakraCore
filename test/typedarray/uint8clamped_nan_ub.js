//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, label)
{
    if (actual !== expected)
    {
        throw new Error(label + ": expected " + expected + ", got " + actual);
    }
}

// Non-finite, zero, and negative inputs must clamp to 0 without UB in ToUInt8Clamped.
var clamped = new Uint8ClampedArray([
    NaN,
    -NaN,
    0 / 0,
    undefined,
    "not-a-number",
    -0,
    +0,
    -0.5,
    -1,
    -1e20,
    -Infinity,
    Infinity,
    1e20,
    255.5
]);

var expectedInitial = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255];
assertEq(clamped.length, expectedInitial.length, "initial length");
for (var i = 0; i < expectedInitial.length; i++)
{
    assertEq(clamped[i], expectedInitial[i], "constructor index " + i);
}

// IEEE-754 ties-to-even rounding for [0, 255].
var ties = new Uint8ClampedArray([
    0.4999999999999999,
    0.5,
    0.5000000000000001,
    1.5,
    2.5,
    3.5,
    253.5,
    254.49999999999997,
    254.5,
    254.50000000000003
]);

var expectedTies = [0, 0, 1, 2, 2, 4, 254, 254, 254, 255];
assertEq(ties.length, expectedTies.length, "ties length");
for (var i = 0; i < expectedTies.length; i++)
{
    assertEq(ties[i], expectedTies[i], "ties index " + i);
}

// Direct indexing, .set, .fill, Uint8ClampedArray.from, and valueOf returning NaN.
var ops = new Uint8ClampedArray(6);
ops.fill(255);
ops[0] = NaN;
ops[1] = -0;
ops[2] = { valueOf: function () { return NaN; } };
ops[3] = 2.5;
ops.set([NaN, 254.5], 4);

var expectedOps = [0, 0, 0, 2, 0, 254];
for (var i = 0; i < expectedOps.length; i++)
{
    assertEq(ops[i], expectedOps[i], "ops index " + i);
}

ops.fill(NaN);
for (var i = 0; i < ops.length; i++)
{
    assertEq(ops[i], 0, "fill(NaN) index " + i);
}

var fromNaN = Uint8ClampedArray.from([NaN, -Infinity, 1.5, 2.5, 254.5, 254.50000000000003]);
var expectedFrom = [0, 0, 2, 2, 254, 255];
for (var i = 0; i < expectedFrom.length; i++)
{
    assertEq(fromNaN[i], expectedFrom[i], "from index " + i);
}

print("pass");
