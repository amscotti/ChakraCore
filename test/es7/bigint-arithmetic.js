//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function truncatingQuotient(a, b) {
    var quotient = a / b;
    return quotient < 0 ? Math.ceil(quotient) : Math.floor(quotient);
}

var tests = [
    {
        name: "Small exhaustive + - * / % matrix against Number arithmetic",
        body() {
            for (var left = -12; left <= 12; left++) {
                for (var right = -12; right <= 12; right++) {
                    var a = BigInt(left);
                    var b = BigInt(right);
                    var label = "(" + left + ", " + right + ")";

                    assert.strictEqual(a + b, BigInt(left + right), "add " + label);
                    assert.strictEqual(a - b, BigInt(left - right), "sub " + label);
                    assert.strictEqual(a * b, BigInt(left * right), "mul " + label);
                    if (right !== 0) {
                        assert.strictEqual(a / b, BigInt(truncatingQuotient(left, right)), "div " + label);
                        assert.strictEqual(a % b, BigInt(left % right), "mod " + label);
                    } else {
                        assert.throws(function () { return a / b; }, RangeError, "div zero " + label);
                        assert.throws(function () { return a % b; }, RangeError, "mod zero " + label);
                    }
                }
            }
        }
    },
    {
        name: "Division truncates toward zero and remainder keeps the dividend sign",
        body() {
            assert.strictEqual(7n / 2n, 3n);
            assert.strictEqual(-7n / 2n, -3n);
            assert.strictEqual(7n / -2n, -3n);
            assert.strictEqual(-7n / -2n, 3n);
            assert.strictEqual(7n % 2n, 1n);
            assert.strictEqual(-7n % 2n, -1n);
            assert.strictEqual(7n % -2n, 1n);
            assert.strictEqual(-7n % -2n, -1n);

            assert.strictEqual(1n / 2n, 0n);
            assert.strictEqual(-1n / 2n, 0n);
            assert.strictEqual(1n % 2n, 1n);
            assert.strictEqual(-1n % 2n, -1n);
            assert.strictEqual(0n / -5n, 0n);
            assert.strictEqual(0n % -5n, 0n);

            assert.throws(function () { return 0n / 0n; }, RangeError);
            assert.throws(function () { return 1n / 0n; }, RangeError);
            assert.throws(function () { return -1n / 0n; }, RangeError);
            assert.throws(function () { return 0n % 0n; }, RangeError);
            assert.throws(function () { return 1n % 0n; }, RangeError);
            assert.throws(function () { return -1n % 0n; }, RangeError);
        }
    },
    {
        name: "Multi-limb division identity and sign invariants",
        body() {
            var values = [
                0xFEDCBA9876543210n,
                0x10000000000000001n,
                0xFEDCBA9876543210FEDCBA9876543210n,
                0x100000000000000000000000000000001n,
                -0x100000000000000000000000000000001n,
                -0x123456789ABCDEF0123456789ABCDEF0n,
                (1n << 256n) - 1n,
                (1n << 512n) - 1n
            ];
            var divisors = [
                3n, 0x1234n, 0x100000001n, 0x123456789ABCDEF0n,
                0xFEDCBA987654320Fn, (1n << 64n) + 1n, (1n << 127n) + 12345n
            ];

            for (var i = 0; i < values.length; i++) {
                for (var j = 0; j < divisors.length; j++) {
                    var a = values[i];
                    var b = divisors[j];
                    var quotient = a / b;
                    var remainder = a % b;
                    var label = "(" + a + ", " + b + ")";

                    assert.strictEqual(quotient * b + remainder, a, "q*b+r " + label);
                    assert.strictEqual((a - quotient * b), remainder, "a-q*b " + label);
                    assert.isTrue(remainder === 0n || (remainder < 0n) === (a < 0n), "remainder sign " + label);

                    var absRemainder = remainder < 0n ? -remainder : remainder;
                    var absDivisor = b < 0n ? -b : b;
                    assert.isTrue(absRemainder < absDivisor, "remainder bound " + label);

                    if (a < 0n) {
                        assert.isTrue(quotient <= 0n || b < 0n, "truncation direction " + label);
                    } else {
                        assert.isTrue(quotient >= 0n || b < 0n, "truncation direction " + label);
                    }
                }
            }
        }
    },
    {
        name: "Multi-limb division expected values",
        body() {
            assert.strictEqual(0xFEDCBA9876543210n / 0x1234n, 0xE0042813BE5DCn);
            assert.strictEqual(0xFEDCBA9876543210n % 0x1234n, 0x960n);
            assert.strictEqual(0xFEDCBA9876543210FEDCBA9876543210n / 0x123456789ABCDEF0n,
                0xE0000000000000D3Dn);
            assert.strictEqual(0xFEDCBA9876543210FEDCBA9876543210n % 0x123456789ABCDEF0n, 0xE2E0n);

            var dividend = 0x10000000000000000000000000000000000000000000000000000000000000001n;
            var divisor = 0x19275E82B7F203C37F28A0759B796E359n;
            assert.strictEqual(dividend / divisor, 0xA2D69B88C344991B4F3EE0A98D66B490n);
            assert.strictEqual(dividend % divisor, 0xA5E19C8FF04893FE6D3B915B08CF89F1n);

            var m = -((1n << 200n) + 987654321n);
            var n = (1n << 127n) + 12345n;
            assert.strictEqual(m / n, -0x1FFFFFFFFFFFFFFFFFFn);
            assert.strictEqual(m % n, -0x7FFFFFFFFF9F8E00000000003ADE98EAn);

            var k = (1n << 80n) + 123456789n;
            assert.strictEqual(k / 12345n, 0x54F077C718E7C48FDn);
            assert.strictEqual(k % 12345n, 0x1CC0n);
        }
    },
    {
        name: "Exponentiation values, signs and RangeError bounds",
        body() {
            assert.strictEqual(0n ** 0n, 1n);
            assert.strictEqual(0n ** 1n, 0n);
            assert.strictEqual(0n ** 9n, 0n);
            assert.strictEqual(1n ** 0n, 1n);
            assert.strictEqual(1n ** 100000000000000000n, 1n);
            assert.strictEqual((-1n) ** 0n, 1n);
            assert.strictEqual((-1n) ** 1n, -1n);
            assert.strictEqual((-1n) ** 2n, 1n);
            assert.strictEqual((-1n) ** 100000000000000001n, -1n);

            assert.strictEqual(2n ** 64n, 0x10000000000000000n);
            assert.strictEqual(10n ** 50n, 0x446C3B15F9926687D2C40534FDB564000000000000n);
            assert.strictEqual(3n ** 100n, 0x5A4653CA673768565B41F775D6947D55CF3813D1n);
            assert.strictEqual(0x123n ** 0x23n,
                0x58AFA7E21DD6D1814C20A0696DE9BDBFE52DAB947F3A09AB47569043B6941C3C29C80DFBn);
            assert.strictEqual((-2n) ** 65n, -0x20000000000000000n);
            assert.strictEqual((-2n) ** 64n, 0x10000000000000000n);
            assert.strictEqual(2n ** 2n ** 0n, 2n);

            assert.strictEqual(2n ** 100000n, 1n << 100000n);

            assert.throws(function () { return 1n ** -1n; }, RangeError);
            assert.throws(function () { return 0n ** -1n; }, RangeError);
            assert.throws(function () { return (-1n) ** -1n; }, RangeError);
            assert.throws(function () { return 1n ** -100000000000000000n; }, RangeError);
            assert.throws(function () { return 2n ** 1000000n; }, RangeError);
            assert.throws(function () { return 2n ** 100000000000000000n; }, RangeError);
            assert.throws(function () { return 2n ** Object(-1n); }, RangeError);
        }
    },
    {
        name: "Small exhaustive bitwise & | ^ matrix against Number arithmetic",
        body() {
            for (var left = -16; left <= 16; left++) {
                for (var right = -16; right <= 16; right++) {
                    var a = BigInt(left);
                    var b = BigInt(right);
                    var label = "(" + left + ", " + right + ")";
                    assert.strictEqual(a & b, BigInt(left & right), "and " + label);
                    assert.strictEqual(a | b, BigInt(left | right), "or " + label);
                    assert.strictEqual(a ^ b, BigInt(left ^ right), "xor " + label);
                }
            }

            var values = [0n, 1n, -1n, 5n, -5n, 0xFEDCBA9876543210n, -0xFEDCBA9876543210n];
            for (var i = 0; i < values.length; i++) {
                var value = values[i];
                assert.strictEqual(value & -1n, value);
                assert.strictEqual(value | 0n, value);
                assert.strictEqual(value ^ 0n, value);
                assert.strictEqual(value ^ value, 0n);
                assert.strictEqual(value & ~value, 0n);
                assert.strictEqual(value | ~value, -1n);
                assert.strictEqual(~value, -value - 1n);
            }
        }
    },
    {
        name: "Multi-limb bitwise expected values",
        body() {
            var a = 0xFEDCBA9876543210FEDCBA9876543210n;
            var b = 0x123456789ABCDEF0123456789ABCDEF0n;
            assert.strictEqual(a & b, 0x12141218121412101214121812141210n);
            assert.strictEqual(a | b, 0xFEFCFEF8FEFCFEF0FEFCFEF8FEFCFEF0n);
            assert.strictEqual(a ^ b, 0xECE8ECE0ECE8ECE0ECE8ECE0ECE8ECE0n);

            var negativeA = -a;
            assert.strictEqual(negativeA & b, 0x20446088A8CCE00020446088A8CCF0n);
            assert.strictEqual(negativeA | b, -0xECC8A88064402000ECC8A88064402010n);
            assert.strictEqual(negativeA ^ b, -0xECE8ECE0ECE8ECE0ECE8ECE0ECE8ED00n);

            assert.strictEqual((a ^ b) ^ b, a);
            assert.strictEqual((a & b) | (a ^ b), a | b);
        }
    },
    {
        name: "Left and right shift behavior including negative counts",
        body() {
            assert.strictEqual(0n << 0n, 0n);
            assert.strictEqual(0b101n << 1n, 0b1010n);
            assert.strictEqual(0b101n << -1n, 0b10n);
            assert.strictEqual(0b101n << -2n, 1n);
            assert.strictEqual(0b101n << -3n, 0n);
            assert.strictEqual(0n << 128n, 0n);
            assert.strictEqual(0n << -128n, 0n);

            assert.strictEqual(0x246n << 127n, 0x12300000000000000000000000000000000n);
            assert.strictEqual(0x246n << 128n, 0x24600000000000000000000000000000000n);
            assert.strictEqual(0x246n << -128n, 0n);

            var wide = 0x123456789ABCDEF0FEDCBA9876543212345678n;
            assert.strictEqual(wide << 64n, 0x123456789ABCDEF0FEDCBA98765432123456780000000000000000n);
            assert.strictEqual(wide << -127n, 0x2468ACn);
            assert.strictEqual(-wide << -127n, -0x2468ADn);
            assert.strictEqual(-wide << 0n, -wide);

            assert.strictEqual(-5n << 1n, -0xAn);
            assert.strictEqual(-5n << -1n, -3n);
            assert.strictEqual(-5n << -2n, -2n);
            assert.strictEqual(-5n << -3n, -1n);
            assert.strictEqual(-1n << 128n, -0x100000000000000000000000000000000n);
            assert.strictEqual(-1n << -128n, -1n);

            assert.strictEqual(5n >> 1n, 2n);
            assert.strictEqual(-5n >> 1n, -3n);
            assert.strictEqual(-5n >> 2n, -2n);
            assert.strictEqual(-1n >> 128n, -1n);
            assert.strictEqual(1n >> 128n, 0n);
            assert.strictEqual(1n >> -128n, 1n << 128n);
            assert.strictEqual(-5n >> -3n, -5n << 3n);

            for (var shift = 0; shift <= 130; shift++) {
                var value = (1n << 60n) + 12345n;
                assert.strictEqual((value << BigInt(shift)) >> BigInt(shift), value, "round trip " + shift);
            }

            assert.strictEqual(0n << (1n << 40n), 0n);
            assert.strictEqual(0n >> (1n << 40n), 0n);
            assert.strictEqual(1n >> (1n << 40n), 0n);
            assert.strictEqual(-1n >> (1n << 40n), -1n);
            assert.strictEqual(-1n << -(1n << 40n), -1n);
            assert.throws(function () { return 1n << (1n << 40n); }, RangeError);
            assert.throws(function () { return -1n << (1n << 40n); }, RangeError);
            assert.throws(function () { return 1n >> -(1n << 40n); }, RangeError);
        }
    },
    {
        name: "Mixed-type operators throw TypeError after coercing both operands",
        body() {
            var operations = [
                function (a, b) { return a - b; },
                function (a, b) { return a * b; },
                function (a, b) { return a / b; },
                function (a, b) { return a % b; },
                function (a, b) { return a ** b; },
                function (a, b) { return a & b; },
                function (a, b) { return a | b; },
                function (a, b) { return a ^ b; },
                function (a, b) { return a << b; },
                function (a, b) { return a >> b; }
            ];
            var nonBigInts = [1, 1.5, NaN, Infinity, "1", true, null, undefined, Symbol("s"), {}];

            var plusValues = [1, 1.5, NaN, Infinity, true, null, undefined, Symbol("s")];
            for (var i = 0; i < plusValues.length; i++) {
                var plusOther = plusValues[i];
                assert.throws(function () { return 1n + plusOther; }, TypeError, "plus bigint left " + i);
                assert.throws(function () { return plusOther + 1n; }, TypeError, "plus bigint right " + i);
            }
            assert.strictEqual(1n + "1", "11");
            assert.strictEqual("1" + 1n, "11");
            assert.strictEqual(1n + Object(1n), 2n);
            assert.strictEqual(Object(1n) + 1n, 2n);

            for (var i = 0; i < operations.length; i++) {
                for (var j = 0; j < nonBigInts.length; j++) {
                    var operation = operations[i];
                    var other = nonBigInts[j];
                    assert.throws(function () { return operation(1n, other); }, TypeError,
                        "op " + i + " bigint left, operand " + j);
                    assert.throws(function () { return operation(other, 1n); }, TypeError,
                        "op " + i + " bigint right, operand " + j);
                }
            }

            assert.throws(function () { return 1n >>> 1n; }, TypeError);
            assert.throws(function () { return 1 >>> 1n; }, TypeError);
            assert.throws(function () { return 1n >>> 1; }, TypeError);
        }
    },
    {
        name: "Wrapper and @@toPrimitive operands work and observe call order",
        body() {
            assert.strictEqual(Object(6n) + 6n, 12n);
            assert.strictEqual(Object(6n) - 2n, 4n);
            assert.strictEqual(Object(6n) * 2n, 12n);
            assert.strictEqual(Object(6n) / 2n, 3n);
            assert.strictEqual(Object(6n) % 4n, 2n);
            assert.strictEqual(Object(2n) ** 3n, 8n);
            assert.strictEqual(Object(6n) & 3n, 2n);
            assert.strictEqual(Object(6n) | 3n, 7n);
            assert.strictEqual(Object(6n) ^ 3n, 5n);
            assert.strictEqual(Object(6n) << 2n, 24n);
            assert.strictEqual(Object(6n) >> 1n, 3n);
            assert.strictEqual(-Object(3n), -3n);
            assert.strictEqual(~Object(3n), -4n);
            assert.throws(function () { return +Object(3n); }, TypeError);
            assert.throws(function () { return Object(3n) >>> 1n; }, TypeError);

            var wrapper = Object(1n);
            assert.strictEqual(wrapper++, 1n);
            assert.strictEqual(wrapper, 2n);
            assert.strictEqual(--wrapper, 1n);

            var prefixWrapper = Object(1n);
            assert.strictEqual(++prefixWrapper, 2n);
            assert.strictEqual(prefixWrapper, 2n);

            function makePrimitive(value, tag, log) {
                return {
                    [Symbol.toPrimitive](hint) {
                        log.push(tag + ":" + hint);
                        return value;
                    }
                };
            }

            var operations = [
                function (a, b) { return a - b; },
                function (a, b) { return a * b; },
                function (a, b) { return a / b; },
                function (a, b) { return a % b; },
                function (a, b) { return a ** b; },
                function (a, b) { return a & b; },
                function (a, b) { return a | b; },
                function (a, b) { return a ^ b; },
                function (a, b) { return a << b; },
                function (a, b) { return a >> b; }
            ];
            for (var i = 0; i < operations.length; i++) {
                var log = [];
                var result = operations[i](makePrimitive(8n, "left", log), makePrimitive(2n, "right", log));
                assert.isTrue(result === 6n || result === 4n || result === 16n || result === 10n ||
                    result === 0n || result === 32n || result === 2n || result === 64n,
                    "coerced result " + result);
                assert.strictEqual(log.join(","), "left:number,right:number", "order " + i);
            }

            function MyError() {}
            var throwingOps = operations.slice(0);
            throwingOps.push(function (a, b) { return a + b; });
            for (var i = 0; i < throwingOps.length; i++) {
                assert.throws(function () {
                    return throwingOps[i](1n, {
                        [Symbol.toPrimitive]() { throw new MyError(); }
                    });
                }, MyError, "abrupt right primitive " + i);
                assert.throws(function () {
                    return throwingOps[i]({
                        [Symbol.toPrimitive]() { throw new MyError(); }
                    }, 1n);
                }, MyError, "abrupt left primitive " + i);
            }

            var zeroLog = [];
            assert.throws(function () {
                return 1n / makePrimitive(0n, "divisor", zeroLog);
            }, RangeError);
            assert.strictEqual(zeroLog.join(","), "divisor:number");
        }
    },
    {
        name: "Canonical zero, unary minus and decrement results compare equal to 0n",
        body() {
            assert.strictEqual(0n - 0n, 0n);
            assert.strictEqual(1n - 1n, 0n);
            assert.strictEqual(-0n, 0n);
            assert.strictEqual(-(-1n), 1n);
            assert.strictEqual(-(-(2n)), 2n);
            assert.strictEqual(- - 1n, 1n);
            assert.strictEqual(~(-1n), 0n);
            assert.strictEqual(~(~0n), 0n);

            var value = 1n;
            value--;
            assert.strictEqual(value, 0n);
            assert.isTrue(value === 0n);

            value = 0n;
            value--;
            assert.strictEqual(value, -1n);
            value++;
            assert.strictEqual(value, 0n);

            var wrapper = Object(1n);
            wrapper--;
            assert.strictEqual(wrapper, 0n);
            assert.isTrue(wrapper === 0n);
        }
    },
    {
        name: "Arithmetic results interoperate with BigInt.asIntN/asUintN",
        body() {
            assert.strictEqual(BigInt.asUintN(64, -1n), (1n << 64n) - 1n);
            assert.strictEqual(BigInt.asIntN(64, 1n << 63n), -(1n << 63n));
            assert.strictEqual(BigInt.asIntN(64, (1n << 63n) - 1n), (1n << 63n) - 1n);
            assert.strictEqual(BigInt.asIntN(64, (1n << 63n) - 1n + 1n), -(1n << 63n));
            assert.strictEqual(BigInt.asUintN(32, (1n << 40n) + 5n), 5n);
            assert.strictEqual(BigInt.asIntN(16, 0x8FFF5n), -11n);
            assert.strictEqual(BigInt.asUintN(8, (0xFFn * 0x101n)), 0xFFn);
            assert.strictEqual(BigInt.asIntN(128, ((-1n) << 127n) - 1n), (1n << 127n) - 1n);
        }
    },
    {
        name: "Boundary and crash matrix completes with controlled results",
        body() {
            var values = [
                0n, 1n, -1n, 2n, -2n,
                0x7FFFFFFFn, -0x80000000n,
                0xFFFFFFFFFFFFFFFFn, -0x8000000000000000n,
                0x10000000000000000n, -0x10000000000000001n,
                (1n << 127n) + 1n, -((1n << 128n) - 1n)
            ];
            var operations = [
                function (a) { return a / 2n; },
                function (a) { return a % 2n; },
                function (a) { return a / -1n; },
                function (a) { return a % -1n; },
                function (a) { return a ** 2n; },
                function (a) { return a & -1n; },
                function (a) { return a | 1n; },
                function (a) { return a ^ (a >> 3n); },
                function (a) { return a << 7n; },
                function (a) { return a >> 7n; }
            ];
            for (var i = 0; i < values.length; i++) {
                for (var j = 0; j < operations.length; j++) {
                    var result = operations[j](values[i]);
                    assert.strictEqual(typeof result, "bigint", "bigint result " + i + "/" + j);
                }
                assert.strictEqual(values[i] / -1n, -values[i], "divide by -1 " + i);
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
