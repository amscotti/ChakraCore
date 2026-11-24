//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// R-7 regression: BigInt exponentiation supports large results such as
// 3n ** 1000000n (up to BigIntMaxResultBitCount(), 2**30 bits on 64-bit /
// 2**29 bits on 32-bit) while rejecting overflowing exponents and products
// cleanly with RangeError without uint32/digit_t length wrap.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "3n ** 1000000n computes exact value, bit length, hex edges, and mod check",
        body() {
            var r = 3n ** 1000000n;
            assert.strictEqual("bigint", typeof r);
            assert.strictEqual(1n, r >> 1584962n, "exact MSB at bit 1584962");
            assert.strictEqual(0n, r >> 1584963n, "no bits above 1584962");

            var hex = r.toString(16);
            assert.strictEqual(396241, hex.length, "hex digit count for 1,584,963 bits");
            assert.strictEqual("5a8e0f6076b7d71b", hex.slice(0, 16), "3n**1000000n top hex digits");
            assert.strictEqual("6a9d5f40e641a501", hex.slice(-16), "3n**1000000n low hex digits");

            // Cross-check factorization and decimal tail without quadratic full decimal print.
            var half = 3n ** 500000n;
            assert.strictEqual(r, half * half, "squaring identity");
            assert.strictEqual(0n, r % 3n, "divisible by 3");
            assert.strictEqual(20000001n, r % 100000000n, "low 8 decimal digits");
        }
    },
    {
        name: "Power-of-two bases use exact left-shift fast path up to large exponents",
        body() {
            assert.strictEqual(1n << 1048576n, 2n ** 1048576n);
            assert.strictEqual(1n << 1048576n, (-2n) ** 1048576n);
            assert.strictEqual(-(1n << 1048575n), (-2n) ** 1048575n);
            assert.strictEqual(1n << 1048576n, 4n ** 524288n);
            assert.strictEqual(1n << 10000000n, (1n << 100n) ** 100000n);
            assert.strictEqual(1n << 10000000n, (-(1n << 100n)) ** 100000n);
            assert.strictEqual(-(1n << 300n), (-(1n << 100n)) ** 3n);
        }
    },
    {
        name: "Overflow boundaries throw RangeError in O(1) without OOM or wrap",
        body() {
            assert.throws(function () { return 2n ** 1073741824n; }, RangeError);
            assert.throws(function () { return (-2n) ** 1073741824n; }, RangeError);
            assert.throws(function () { return 2n ** (1n << 100n); }, RangeError);
            assert.throws(function () { return 3n ** 715827883n; }, RangeError);
            assert.throws(function () { return 3n ** (1n << 60n); }, RangeError);
            assert.throws(function () { return (1n << 536870912n) ** 2n; }, RangeError);
            assert.throws(function () { return 1n << 1073741824n; }, RangeError);

            // Exact upper boundary of BigIntMaxResultBitCount(): 2**30 bits on 64-bit
            // (2**29 bits on 32-bit). Compute exact-cap values outside assert.throws
            // so ShiftLeftMagnitude is proved not to throw 1 limb early.
            var has64BitCap = false;
            try {
                var probe = 1n << 536870912n;
                assert.strictEqual(1n, probe >> 536870912n);
                probe = null;
                has64BitCap = true;
            } catch (e) {
                assert.strictEqual(true, e instanceof RangeError);
            }

            var maxExp = has64BitCap ? 1073741823n : 536870911n;
            var maxShiftLow = 1n << (maxExp - 62n);
            assert.strictEqual(1n, maxShiftLow >> (maxExp - 62n));
            maxShiftLow = null;

            var carryShift = 7n << (maxExp - 64n);
            assert.strictEqual(7n, carryShift >> (maxExp - 64n));
            carryShift = null;

            var threeShift = 3n << (maxExp - 1n);
            assert.strictEqual(3n, threeShift >> (maxExp - 1n));
            assert.throws(function () { return 3n << maxExp; }, RangeError);
            threeShift = null;

            var maxShift = 1n << maxExp;
            assert.strictEqual(1n, maxShift >> maxExp);
            assert.throws(function () { return maxShift * 2n; }, RangeError);
            assert.throws(function () { return maxShift << 1n; }, RangeError);
            maxShift = null;

            var maxPow = 2n ** maxExp;
            assert.strictEqual(1n, maxPow >> maxExp);
            assert.throws(function () { return maxPow * 2n; }, RangeError);
            assert.throws(function () { return maxPow << 1n; }, RangeError);
            maxPow = null;

            var maxNegPow = (-2n) ** maxExp;
            assert.strictEqual(-1n, maxNegPow >> maxExp);
            maxNegPow = null;
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
