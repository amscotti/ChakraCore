//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Exact decimal spellings for the 999/1000-bit boundary values (BigInt
// arithmetic and non-decimal literals are C3, so the test parses strings).
var two999 = "5357543035931336604742125245300009052807024058527668037218751941851755255624680612465991894078479290637973364587765734125935726428461570217992288787349287401967283887412115492710537302531185570938977091076523237491790970633699383779582771973038531457285598238843271083830214915826312193418602834034688";
var two1000Minus1 = "10715086071862673209484250490600018105614048117055336074437503883703510511249361224931983788156958581275946729175531468251871452856923140435984577574698574803934567774824230985421074605062371141877954182153046474983581941267398767559165543946077062914571196477686542167660429831652624386837205668069375";

var tests = [
    {
        name: "BigInt.asIntN/asUintN zero bits produce 0n",
        body() {
            [0n, 1n, -1n, 2n, -2n, 123456789012345678901n, -123456789012345678901n].forEach(function (value) {
                assert.strictEqual(0n, BigInt.asIntN(0, value), "asIntN(0, " + value + ")");
                assert.strictEqual(0n, BigInt.asUintN(0, value), "asUintN(0, " + value + ")");
            });
            [undefined, NaN, 0.9, -0.9, null, false, "", "foo", {}, []].forEach(function (bits) {
                assert.strictEqual(0n, BigInt.asIntN(bits, 1n), "asIntN bits=" + String(bits));
                assert.strictEqual(0n, BigInt.asUintN(bits, 1n), "asUintN bits=" + String(bits));
            });
        }
    },
    {
        name: "Small-bit signed and unsigned two's-complement behavior",
        body() {
            assert.strictEqual(-1n, BigInt.asIntN(1, 1n));
            assert.strictEqual(-1n, BigInt.asIntN(1, -1n));
            assert.strictEqual(0n, BigInt.asIntN(1, -2n));
            assert.strictEqual(1n, BigInt.asUintN(1, -1n));
            assert.strictEqual(0n, BigInt.asUintN(1, -2n));

            assert.strictEqual(-85n, BigInt.asIntN(8, 171n));
            assert.strictEqual(85n, BigInt.asUintN(8, 85n));
            assert.strictEqual(255n, BigInt.asUintN(8, -1n));
            assert.strictEqual(1n, BigInt.asUintN(8, -255n));
            assert.strictEqual(0n, BigInt.asUintN(8, -256n));
            assert.strictEqual(255n, BigInt.asUintN(8, -257n));
            assert.strictEqual(-1n, BigInt.asIntN(8, -257n));
            assert.strictEqual(127n, BigInt.asIntN(8, -129n));
            assert.strictEqual(-128n, BigInt.asIntN(8, -128n));
            assert.strictEqual(-127n, BigInt.asIntN(8, 129n));
            assert.strictEqual(0n, BigInt.asIntN(8, 256n));
            assert.strictEqual(1n, BigInt.asIntN(8, 257n));
        }
    },
    {
        name: "32-, 64-, 65- and 128-bit boundaries",
        body() {
            assert.strictEqual(-1n, BigInt.asIntN(32, 4294967295n));
            assert.strictEqual(-2147483648n, BigInt.asIntN(32, 2147483648n));
            assert.strictEqual(2147483647n, BigInt.asIntN(32, 2147483647n));
            assert.strictEqual(-2147483648n, BigInt.asIntN(32, -2147483648n));
            assert.strictEqual(2147483647n, BigInt.asIntN(32, -2147483649n));
            assert.strictEqual(4294967295n, BigInt.asUintN(32, -1n));
            assert.strictEqual(0n, BigInt.asUintN(32, 4294967296n));

            assert.strictEqual(-1n, BigInt.asIntN(64, 18446744073709551615n));
            assert.strictEqual(-9223372036854775808n, BigInt.asIntN(64, 9223372036854775808n));
            assert.strictEqual(9223372036854775807n, BigInt.asIntN(64, 9223372036854775807n));
            assert.strictEqual(-9223372036854775808n, BigInt.asIntN(64, -9223372036854775808n));
            assert.strictEqual(9223372036854775807n, BigInt.asIntN(64, -9223372036854775809n));
            assert.strictEqual(18446744073709551615n, BigInt.asUintN(64, -1n));
            assert.strictEqual(0n, BigInt.asUintN(64, 18446744073709551616n));
            // 65 bits uses a partial top limb on 64-bit digit_t builds.
            assert.strictEqual(-18446744073709551616n, BigInt.asIntN(65, 18446744073709551616n));
            assert.strictEqual(18446744073709551615n, BigInt.asIntN(65, -18446744073709551617n));
            assert.strictEqual(18446744073709551616n, BigInt.asUintN(65, 18446744073709551616n));

            assert.strictEqual(-170141183460469231731687303715884105728n,
                BigInt.asIntN(128, 170141183460469231731687303715884105728n));
            assert.strictEqual(170141183460469231731687303715884105727n,
                BigInt.asIntN(128, 170141183460469231731687303715884105727n));
            assert.strictEqual(-123456789012345678901234567890n,
                BigInt.asIntN(128, -123456789012345678901234567890n));
            assert.strictEqual(340282366920938463463374607431768211455n,
                BigInt.asUintN(128, -1n));
            assert.strictEqual(0n, BigInt.asUintN(128, 340282366920938463463374607431768211456n));
        }
    },
    {
        name: "Operations keep only the low ceil(bits/limbBits) limbs",
        body() {
            var huge = 123456789012345678901234567890123456789012345678901234567890n;
            assert.strictEqual(-8300149958212908334n, BigInt.asIntN(64, huge));
            assert.strictEqual(10146594115496643282n, BigInt.asUintN(64, huge));
            assert.strictEqual(84445654526471084090152257172637944530n,
                BigInt.asUintN(128, huge));
            assert.strictEqual(84445654526471084090152257172637944530n,
                BigInt.asIntN(128, huge));
            assert.strictEqual(0n, BigInt.asIntN(1, -123456789012345678900n));
            assert.strictEqual(-1n, BigInt.asIntN(1, 123456789012345678901n));
        }
    },
    {
        name: "1000-bit results are exact",
        body() {
            var two999Value = BigInt(two999);
            assert.strictEqual(two999Value, BigInt.asUintN(1000, two999Value));
            assert.strictEqual(-two999Value, BigInt.asIntN(1000, two999Value));
            assert.strictEqual(BigInt(two1000Minus1), BigInt.asUintN(1000, -1n));
            assert.strictEqual(BigInt(two1000Minus1), BigInt.asUintN(1000, BigInt(two1000Minus1)));
            assert.strictEqual(-1n, BigInt.asIntN(1000, BigInt(two1000Minus1)));
            assert.strictEqual(-1n, BigInt.asIntN(1000, -1n));
            assert.strictEqual(-two999Value, BigInt.asIntN(1000, -two999Value));

            assert.strictEqual(two999Value, BigInt.asUintN(1000, -two999Value));
        }
    },
    {
        name: "ToIndex coercion, errors and order",
        body() {
            assert.strictEqual(-1n, BigInt.asIntN(true, 1n));
            assert.strictEqual(-1n, BigInt.asIntN("1", 1n));
            assert.strictEqual(-1n, BigInt.asIntN(["1"], 1n));
            assert.strictEqual(0n, BigInt.asIntN([0], 1n));
            assert.strictEqual(2n, BigInt.asIntN(3.9, 10n));
            assert.strictEqual(2n, BigInt.asIntN("3.9", 10n));
            assert.strictEqual(2n, BigInt.asIntN({ valueOf: function () { return 3; } }, 10n));
            assert.strictEqual(2n, BigInt.asIntN({
                [Symbol.toPrimitive]: function () { return 3; },
                valueOf: function () { throw new Error("wrong"); }
            }, 10n));

            [-1, -2.5, "-2.5", -Infinity, 9007199254740992, Infinity].forEach(function (bits) {
                assert.throws(function () { BigInt.asIntN(bits, 0n); }, RangeError, "asIntN bits=" + String(bits));
                assert.throws(function () { BigInt.asUintN(bits, 0n); }, RangeError, "asUintN bits=" + String(bits));
            });
            assert.strictEqual(1n, BigInt.asIntN(9007199254740991, 1n));
            assert.strictEqual(-1n, BigInt.asIntN(9007199254740991, -1n));
            assert.strictEqual(1n, BigInt.asUintN(9007199254740991, 1n));
            assert.throws(function () { BigInt.asUintN(1073741823, -1n); }, RangeError,
                "negative asUintN at the result-limb bound");
            assert.throws(function () { BigInt.asUintN(1073741824, -1n); }, RangeError,
                "negative asUintN at the exact result-limb bound");

            [0n, Object(0n), Symbol("1"), Object(Symbol("1"))].forEach(function (bits) {
                assert.throws(function () { BigInt.asIntN(bits, 0n); }, TypeError);
                assert.throws(function () { BigInt.asUintN(bits, 0n); }, TypeError);
            });
            assert.throws(function () {
                BigInt.asIntN({ valueOf: function () { return 0n; } }, 0n);
            }, TypeError);
            assert.throws(function () {
                BigInt.asIntN({ [Symbol.toPrimitive]: function () { return 0n; } }, 0n);
            }, TypeError);

            var calls = 0;
            assert.throws(function () {
                BigInt.asIntN({
                    valueOf: function () { calls++; return 0n; }
                }, {
                    valueOf: function () { calls += 100; return 0n; }
                });
            }, TypeError);
            assert.strictEqual(1, calls, "ToIndex runs before ToBigInt");

            assert.throws(function () { BigInt.asIntN(); }, TypeError);
            assert.throws(function () { BigInt.asIntN(0); }, TypeError);
            assert.throws(function () { BigInt.asUintN(0); }, TypeError);
            [undefined, null, 0, NaN, Infinity, Symbol("1"), Object(Symbol("1"))].forEach(function (value) {
                assert.throws(function () { BigInt.asIntN(1, value); }, TypeError, "bigint=" + String(value));
            });
            assert.throws(function () { BigInt.asIntN(1, "a"); }, SyntaxError);
            assert.throws(function () { BigInt.asIntN(1, "0b2"); }, SyntaxError);
            assert.throws(function () { BigInt.asIntN(1, Object("0b2")); }, SyntaxError);
            assert.strictEqual(1n, BigInt.asIntN(2, Object(1n)));
            assert.strictEqual(1n, BigInt.asIntN(2, { valueOf: function () { return "1"; } }));
            assert.strictEqual(1n, BigInt.asUintN(2, {
                [Symbol.toPrimitive]: function () { return true; }
            }));
        }
    },
    {
        name: "Round-trip between asUintN and asIntN",
        body() {
            var values = [-2147483648n, -2147483647n, -1n, 0n, 1n,
                2147483647n];
            values.forEach(function (value) {
                assert.strictEqual(value, BigInt.asIntN(32, BigInt.asUintN(32, value)), "signed " + value);
                assert.strictEqual(value, BigInt.asIntN(64, BigInt.asUintN(64, value)), "signed64 " + value);
            });
            var unsigned = [0n, 1n, 2147483647n, 2147483648n, 4294967295n];
            unsigned.forEach(function (value) {
                assert.strictEqual(value, BigInt.asUintN(32, BigInt.asIntN(32, value)), "unsigned " + value);
                assert.strictEqual(value, BigInt.asUintN(128, BigInt.asIntN(128, value)), "unsigned128 " + value);
            });
        }
    },
    {
        name: "Function properties, descriptors and constructability",
        body() {
            assert.strictEqual("function", typeof BigInt.asIntN);
            assert.strictEqual("function", typeof BigInt.asUintN);
            assert.strictEqual(2, BigInt.asIntN.length);
            assert.strictEqual(2, BigInt.asUintN.length);
            assert.strictEqual("asIntN", BigInt.asIntN.name);
            assert.strictEqual("asUintN", BigInt.asUintN.name);
            assert.strictEqual(undefined, BigInt.asIntN.prototype);
            assert.strictEqual(undefined, BigInt.asUintN.prototype);

            ["asIntN", "asUintN"].forEach(function (name) {
                var descriptor = Object.getOwnPropertyDescriptor(BigInt, name);
                assert.isTrue(!!descriptor);
                assert.isFalse(descriptor.enumerable);
                assert.isTrue(descriptor.writable);
                assert.isTrue(descriptor.configurable);
                assert.strictEqual("function", typeof descriptor.value);
                assert.isTrue(Object.prototype.hasOwnProperty.call(BigInt, name));
            });

            assert.throws(function () { new BigInt.asIntN(64, 1n); }, TypeError);
            assert.throws(function () { new BigInt.asUintN(64, 1n); }, TypeError);
            if (typeof Reflect !== "undefined" && Reflect.construct) {
                assert.throws(function () { Reflect.construct(BigInt.asIntN, [64, 1n]); }, TypeError);
                assert.throws(function () { Reflect.construct(BigInt.asUintN, [64, 1n]); }, TypeError);
            }
            assert.strictEqual(-1n, BigInt.asIntN.call(undefined, 1, 1n));
            assert.strictEqual(1n, BigInt.asUintN.call(undefined, 1, 1n));
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
