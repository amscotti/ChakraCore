//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "Hex, octal and binary BigInt literals with both prefix cases",
        body() {
            assert.strictEqual(0x0n, 0n);
            assert.strictEqual(0X0n, 0n);
            assert.strictEqual(0o0n, 0n);
            assert.strictEqual(0O0n, 0n);
            assert.strictEqual(0b0n, 0n);
            assert.strictEqual(0B0n, 0n);

            assert.strictEqual(0x1n, 1n);
            assert.strictEqual(0X1n, 1n);
            assert.strictEqual(0xan, 10n);
            assert.strictEqual(0xAn, 10n);
            assert.strictEqual(0xABn, 171n);
            assert.strictEqual(0xfn, 15n);
            assert.strictEqual(0o7n, 7n);
            assert.strictEqual(0O10n, 8n);
            assert.strictEqual(0o777n, 511n);
            assert.strictEqual(0b1n, 1n);
            assert.strictEqual(0B1010n, 10n);
            assert.strictEqual(0b11111111n, 255n);
            assert.strictEqual("bigint", typeof 0x10n);
        }
    },
    {
        name: "Hex digit e is a digit, not an exponent",
        body() {
            // The scanner consumes e/E as hexadecimal digits; Node agrees
            // that these are integer literals, not exponent forms.
            assert.strictEqual(0x1e2n, 482n);
            assert.strictEqual(0x1En, 30n);
            assert.strictEqual(0xen, 14n);
            assert.strictEqual(0xEn, 14n);
        }
    },
    {
        name: "Numeric separators between digits",
        body() {
            assert.strictEqual(1_2n, 12n);
            assert.strictEqual(1_000n, 1000n);
            assert.strictEqual(0x1_2n, 18n);
            assert.strictEqual(0X1_2n, 18n);
            assert.strictEqual(0xdead_beefn, 3735928559n);
            assert.strictEqual(0b1_1n, 3n);
            assert.strictEqual(0B1_0n, 2n);
            assert.strictEqual(0o7_7n, 63n);
            assert.strictEqual(0O7_0n, 56n);
            assert.strictEqual(0x0_1n, 1n);
            assert.strictEqual(0b0_1n, 1n);
            assert.strictEqual(0o0_1n, 1n);
            assert.strictEqual(0x00_00_01n, 1n);
        }
    },
    {
        name: "Multi-limb values keep every digit",
        body() {
            assert.strictEqual(0xffffffffffffffffn, 18446744073709551615n);
            assert.strictEqual(0x10000000000000000n, 18446744073709551616n);
            assert.strictEqual(0xffffffffffffffffffffffffffffffffn,
                340282366920938463463374607431768211455n);
            assert.strictEqual(0x123456789abcdef0123456789abcdef0n,
                24197857203266734864793317670504947440n);
            assert.strictEqual(0o1234567012345670123456701234567n,
                1616895878810725189668911479n);
            assert.strictEqual(0b101010101010101010101010101010101010101010101010101010101010101010101n,
                393530540239137101141n);
            assert.strictEqual(0xFFFFFFFFFFFFFFFFn, 0xffffffffffffffffn);
        }
    },
    {
        name: "Negative non-decimal literals and negative zero",
        body() {
            assert.strictEqual(-0x10n, -16n);
            assert.strictEqual(-0XFFn, -255n);
            assert.strictEqual(-0o10n, -8n);
            assert.strictEqual(-0O777n, -511n);
            assert.strictEqual(-0b101n, -5n);
            assert.strictEqual(-0B1111n, -15n);
            assert.strictEqual(-0x0n, 0n);
            assert.strictEqual(-0o0n, 0n);
            assert.strictEqual(-0b0n, 0n);
            assert.strictEqual(1 / Number(0x0n), Infinity);
            assert.strictEqual(1 / Number(-0x0n), Infinity);
        }
    },
    {
        name: "Literals in expressions, keys, calls and returns",
        body() {
            var value = 0x10n;
            var object = { property: 0o10n };
            object[0x10n] = 0b11n;
            assert.strictEqual(value, 16n);
            assert.strictEqual(object.property, 8n);
            assert.strictEqual(object[16n], 3n);
            assert.strictEqual(object["16"], 3n);

            function identity(x) { return x; }
            assert.strictEqual(identity(0x10n), 16n);
            assert.strictEqual([0x10n, 0o10n, 0b10n][1], 8n);
            assert.strictEqual((function () { return 0b101n; })(), 5n);
            assert.strictEqual((function (x) { return x + 1n; })(0x10n), 17n);
            assert.strictEqual(`${0x10n}`, "16");
            assert.strictEqual(String(0o10n), "8");
            assert.strictEqual((0x10n).toString(), "16");
            assert.strictEqual((0x10n).toString(16), "10");
            assert.strictEqual((0x10n).valueOf(), 16n);
            assert.strictEqual(Boolean(0x0n), false);
            assert.strictEqual(Boolean(0x1n), true);
        }
    },
    {
        name: "Equality and asIntN/asUintN use non-decimal literals",
        body() {
            assert.isTrue(0x10n === 16n);
            assert.isTrue(0o10n === 8n);
            assert.isTrue(0b10n === 2n);
            assert.isTrue(0xdead_beefn === 0xDEADBEEFn);
            assert.strictEqual(BigInt.asIntN(8, 0xFFn), -1n);
            assert.strictEqual(BigInt.asIntN(8, 0x7Fn), 127n);
            assert.strictEqual(BigInt.asIntN(8, 0x80n), -128n);
            assert.strictEqual(BigInt.asUintN(8, -0x1n), 255n);
            assert.strictEqual(BigInt.asUintN(8, -0o1n), 255n);
            assert.strictEqual(BigInt.asUintN(8, -0b1n), 255n);
            assert.strictEqual(BigInt.asIntN(128, 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFn), -1n);
            assert.strictEqual(BigInt.asUintN(128, -0x1n),
                340282366920938463463374607431768211455n);
        }
    },
    {
        name: "Sloppy and strict code, eval and new Function",
        body() {
            // Sloppy function
            function sloppy() { return 0x10n; }
            // Strict function
            function strict() { "use strict"; return 0o10n; }
            // Strict class method
            class StrictClass {
                method() { "use strict"; return 0b10n; }
            }
            assert.strictEqual(sloppy(), 16n);
            assert.strictEqual(strict(), 8n);
            assert.strictEqual(new StrictClass().method(), 2n);
            assert.strictEqual(eval("0x10n"), 16n);
            assert.strictEqual(eval("'use strict'; 0o10n"), 8n);
            assert.strictEqual(new Function("return 0x10n")(), 16n);
            assert.strictEqual(new Function("'use strict'; return 0b10n")(), 2n);

            // A strict directive must not change literal validation.
            assert.throws(function () { eval("'use strict'; 0x"); }, SyntaxError);
            assert.throws(function () { new Function("'use strict'; return 0x1_n"); }, SyntaxError);
        }
    },
    {
        name: "Invalid non-decimal BigInt forms throw SyntaxError",
        body() {
            [
                "0x", "0X", "0o", "0O", "0b", "0B",
                "0xn", "0Xn", "0on", "0On", "0bn", "0Bn",
                "0x_n", "0x_1n", "0x1_n", "0x1__2n", "0x1n_",
                "0b_n", "0b_1n", "0b1_n", "0b1__0n",
                "0o_n", "0o_1n", "0o1_n", "0o1__7n",
                "0x1.2n", "0x1pn", "0x1p2n",
                "0b2", "0B2", "0o8", "0O8",
                "0X1N", "0x1N",
                "0x1nfoo", "0x1n0", "0b1nfoo", "0o1nfoo",
                "1nfoo", "08n", "09n", "081n", "08_1n", "00n", "0_0n"
            ].forEach(function (source) {
                assert.throws(function () { eval(source); }, SyntaxError, source);
            });
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
