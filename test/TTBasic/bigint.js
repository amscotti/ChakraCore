//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertEq(actual, expected, label)
{
    if (actual !== expected)
    {
        throw new Error(label + ": expected " + String(expected) + ", got " + String(actual));
    }
}

var zero = 0n;
var one = 1n;
var negOne = -1n;
var maxI64 = 0x7fffffffffffffffn;
var minI64 = -0x8000000000000000n;
var maxU64 = 0xffffffffffffffffn;
var negMaxU64 = -0xffffffffffffffffn;
var pos64Plus1 = 1n << 64n;
var neg64Minus1 = -(1n << 64n) - 1n;
var hugePos = (1n << 200n) + 0x123456789abcdef0123456789abcdefn;
var hugeNeg = -(1n << 256n) - 9876543210123456789n;

var obj = {
    zero: zero,
    negOne: negOne,
    maxU64: maxU64,
    hugePos: hugePos,
    hugeNeg: hugeNeg
};

var arr = [zero, one, negOne, maxI64, minI64, maxU64, negMaxU64, pos64Plus1, neg64Minus1, hugePos, hugeNeg];

var biMap = new Map();
biMap.set(zero, "zero");
biMap.set(negOne, "negOne");
biMap.set(maxU64, hugePos);
biMap.set(hugePos, hugeNeg);
biMap.set(hugeNeg, 42n);

var biSet = new Set([zero, negOne, maxU64, negMaxU64, hugePos, hugeNeg]);

var i64ta = new BigInt64Array([0n, -1n, maxI64, minI64]);
var u64ta = new BigUint64Array([0n, 1n, maxU64]);

function makeAdder(base)
{
    return function (delta)
    {
        return base + delta;
    };
}

var addHuge = makeAdder(hugePos);

WScript.SetTimeout(step1, 20);

function step1()
{
    assertEq(typeof zero, "bigint", "typeof zero");
    assertEq(zero, 0n, "zero");
    assertEq(one, 1n, "one");
    assertEq(negOne, -1n, "negOne");
    assertEq(maxI64, 9223372036854775807n, "maxI64");
    assertEq(minI64, -9223372036854775808n, "minI64");
    assertEq(maxU64, 18446744073709551615n, "maxU64");
    assertEq(negMaxU64, -18446744073709551615n, "negMaxU64");
    assertEq(pos64Plus1, 1n << 64n, "pos64Plus1");
    assertEq(neg64Minus1, -(1n << 64n) - 1n, "neg64Minus1");
    assertEq(hugePos, (1n << 200n) + 0x123456789abcdef0123456789abcdefn, "hugePos");
    assertEq(hugeNeg, -(1n << 256n) - 9876543210123456789n, "hugeNeg");

    assertEq(obj.zero, 0n, "obj.zero");
    assertEq(obj.negOne, -1n, "obj.negOne");
    assertEq(obj.maxU64, maxU64, "obj.maxU64");
    assertEq(obj.hugePos, hugePos, "obj.hugePos");
    assertEq(obj.hugeNeg, hugeNeg, "obj.hugeNeg");

    assertEq(arr.length, 11, "arr.length");
    assertEq(arr[9], hugePos, "arr[9]");
    assertEq(arr[10], hugeNeg, "arr[10]");

    assertEq(biMap.get(0n), "zero", "map zero");
    assertEq(biMap.get(-1n), "negOne", "map negOne");
    assertEq(biMap.get(maxU64), hugePos, "map maxU64");
    assertEq(biMap.get((1n << 200n) + 0x123456789abcdef0123456789abcdefn), hugeNeg, "map hugePos");
    assertEq(biMap.get(hugeNeg), 42n, "map hugeNeg");

    assertEq(biSet.has(0n), true, "set zero");
    assertEq(biSet.has(-1n), true, "set negOne");
    assertEq(biSet.has(maxU64), true, "set maxU64");
    assertEq(biSet.has(negMaxU64), true, "set negMaxU64");
    assertEq(biSet.has(hugePos), true, "set hugePos");
    assertEq(biSet.has(hugeNeg), true, "set hugeNeg");
    assertEq(biSet.has(999n), false, "set missing");

    assertEq(i64ta[2], maxI64, "i64ta[2]");
    assertEq(i64ta[3], minI64, "i64ta[3]");
    assertEq(u64ta[2], maxU64, "u64ta[2]");

    assertEq(addHuge(1n), hugePos + 1n, "closure addHuge");

    if (typeof telemetryLog === "function")
    {
        telemetryLog("step1 ok", true);
    }
    WScript.SetTimeout(step2, 20);
}

function step2()
{
    assertEq(addHuge(negOne), hugePos - 1n, "step2 closure");
    assertEq(biMap.get(hugePos) + 1n, hugeNeg + 1n, "step2 map arithmetic");
    if (typeof telemetryLog === "function")
    {
        telemetryLog("step2 ok", true);
    }
    else
    {
        print("pass");
    }

    if (typeof emitTTDLog === "function" && typeof ttdLogURI !== "undefined")
    {
        emitTTDLog(ttdLogURI);
    }
}
