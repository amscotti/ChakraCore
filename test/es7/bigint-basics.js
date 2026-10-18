//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "BigInt constructor converts Number and Boolean values",
        body() {
            assert.strictEqual(42n, BigInt(42));
            assert.strictEqual(-42n, BigInt(-42));
            assert.strictEqual(0n, BigInt(0));
            assert.strictEqual(0n, BigInt(-0));
            assert.strictEqual(1n, BigInt(1.0));
            assert.strictEqual(1n, BigInt(true));
            assert.strictEqual(0n, BigInt(false));
            assert.strictEqual(9007199254740991n, BigInt(Number.MAX_SAFE_INTEGER));
            assert.strictEqual(-9007199254740991n, BigInt(-Number.MAX_SAFE_INTEGER));
            assert.strictEqual(9007199254740992n, BigInt(Number.MAX_SAFE_INTEGER + 1));
            // Multi-digit NumberToBigInt: 2^64 and 2^80 cross the old single-limb stub.
            assert.strictEqual(18446744073709551616n, BigInt(18446744073709551616));
            assert.strictEqual(1208925819614629174706176n, BigInt(Math.pow(2, 80)));
            assert.strictEqual("bigint", typeof BigInt(Number.MAX_VALUE));
        }
    },
    {
        name: "BigInt constructor rejects non-integral and non-finite Numbers",
        body() {
            [NaN, Infinity, -Infinity, 0.00005, -0.00005, 0.1, -0.1, 1.1, -1.1, Number.MIN_VALUE].forEach(function (value) {
                assert.throws(function () { BigInt(value); }, RangeError, "BigInt(" + value + ")");
            });
        }
    },
    {
        name: "BigInt constructor parses StringToBigInt grammar",
        body() {
            assert.strictEqual(0n, BigInt(""));
            assert.strictEqual(0n, BigInt(" "));
            assert.strictEqual(0n, BigInt("\t\n\r\u00a0"));
            assert.strictEqual(10n, BigInt("10"));
            assert.strictEqual(-10n, BigInt("-10"));
            assert.strictEqual(10n, BigInt("+10"));
            assert.strictEqual(7n, BigInt("0007"));
            assert.strictEqual(0n, BigInt("-0"));
            assert.strictEqual(15n, BigInt("0b1111"));
            assert.strictEqual(15n, BigInt("0B1111"));
            assert.strictEqual(255n, BigInt("0xff"));
            assert.strictEqual(64188n, BigInt("0Xfabc"));
            assert.strictEqual(8n, BigInt("0o10"));
            assert.strictEqual(16n, BigInt("0O20"));
            assert.strictEqual(15n, BigInt("   0b1111"));
            assert.strictEqual(-197n, BigInt("   -197   "));
            assert.strictEqual(18446744073709551616n, BigInt("18446744073709551616   "));
            assert.strictEqual(75557863725914323419135n, BigInt("0xfffffffffffffffffff"));
            assert.strictEqual(340282366920938463463374607431768211456n,
                BigInt("0b1" + "0".repeat(128)));
        }
    },
    {
        name: "BigInt constructor rejects invalid StringToBigInt input",
        body() {
            ["10n", "10x", "10b", "10.5", "1e3", "Infinity", "-Infinity", "0b", "0o", "0x",
                "-0x1", "-0XFFab", "+0o1", "0oa", "000 12", "00o", "00b", "00x", "0b2", "\u0085"]
                .forEach(function (value) {
                    assert.throws(function () { BigInt(value); }, SyntaxError, "BigInt(" + JSON.stringify(value) + ")");
                });
        }
    },
    {
        name: "BigInt constructor uses ToPrimitive once and honors valueOf/toString/@@toPrimitive",
        body() {
            var first = true;
            var once = {
                [Symbol.toPrimitive]: function () {
                    if (first) {
                        first = false;
                        return "42";
                    }
                    throw new Error("Symbol.toPrimitive should only be invoked once");
                }
            };
            assert.strictEqual(42n, BigInt(once));

            var calls = 0;
            assert.strictEqual(44n, BigInt({
                valueOf: function () { calls++; return 44; },
                toString: function () { throw new Error("toString should not be reached"); }
            }));
            assert.strictEqual(1, calls, "valueOf called once");

            assert.strictEqual(45n, BigInt({ toString: function () { return "45"; } }));
            assert.strictEqual(7n, BigInt([7]));
            assert.strictEqual(1n, BigInt(Object(1n)));

            var noCoercion = false;
            assert.throws(function () {
                new BigInt({ valueOf: function () { noCoercion = true; return 1; } });
            }, TypeError, "new BigInt");
            assert.isFalse(noCoercion, "new BigInt throws before argument coercion");
        }
    },
    {
        name: "BigInt constructor rejects null, undefined, Symbol and no argument",
        body() {
            [null, undefined, Symbol("x")].forEach(function (value) {
                assert.throws(function () { BigInt(value); }, TypeError);
            });
            assert.throws(function () { BigInt(); }, TypeError, "BigInt() with no argument");
        }
    },
    {
        name: "typeof, ToBoolean and Object.prototype.toString identify BigInt",
        body() {
            assert.strictEqual("bigint", typeof 10n);
            assert.strictEqual("bigint", typeof 0n);
            assert.strictEqual("bigint", typeof BigInt("1"));
            assert.strictEqual("object", typeof Object(1n));
            assert.strictEqual("[object BigInt]", Object.prototype.toString.call(1n));
            assert.isFalse(Boolean(0n), "0n is false");
            assert.isTrue(Boolean(1n), "1n is true");
            assert.isTrue(Boolean(-1n), "-1n is true");
        }
    },
    {
        name: "Number(BigInt) is correctly rounded",
        body() {
            // Values past 2^53 where a limb-wise Horner accumulation rounds
            // one ulp away from the spec result.
            assert.strictEqual(35527847360849916000, Number(BigInt("35527847360849914690")));
            assert.strictEqual(-35527847360849916000, Number(BigInt("-35527847360849914690")));
            assert.strictEqual(4.6839495363032805e+30, Number(BigInt("4683949536303280496474565056423")));
            assert.strictEqual(8.812566036649335e+62,
                Number(BigInt("881256603664933473312330616694815128847873848356244872831692965")));
            assert.strictEqual(18446744073709551616, Number(BigInt("18446744073709551616")));
            assert.strictEqual(Infinity, Number(BigInt("1" + "0".repeat(400))));

            // Number() applies ToPrimitive before its explicit BigInt
            // conversion; Number(Object(1n)) must not reach ToNumber(BigInt).
            assert.strictEqual(1, Number(Object(1n)));
            assert.strictEqual(-1, Number(Object(-1n)));
            assert.strictEqual(1, Number({ valueOf: function () { return 1n; } }));
            assert.strictEqual(1, new Number(Object(1n)).valueOf());
        }
    },
    {
        name: "BigInt.prototype.toString uses the requested radix",
        body() {
            assert.strictEqual("255", (255n).toString());
            assert.strictEqual("-100", (-100n).toString());
            assert.strictEqual("0", (0n).toString());
            assert.strictEqual("ff", (255n).toString(16));
            assert.strictEqual("-ff", (-255n).toString(16));
            assert.strictEqual("11111111", (255n).toString(2));
            assert.strictEqual("377", (255n).toString(8));
            assert.strictEqual("73", (255n).toString(36));
            assert.strictEqual("z", (35n).toString(36));
            assert.strictEqual("0", (0n).toString(2));
            assert.strictEqual("-1", (-1n).toString(36));
            assert.strictEqual("ff", (255n).toString(16.9), "ToIntegerOrInfinity truncates");
            assert.strictEqual("ff", (255n).toString("16"));
            assert.strictEqual("-100", (-100n).toString(undefined));

            for (var radix = 2; radix <= 36; radix++) {
                assert.strictEqual("0", (0n).toString(radix));
                assert.strictEqual("1", (1n).toString(radix));
                assert.strictEqual("-1", (-1n).toString(radix));
            }
            for (var digit = 10; digit < 36; digit++) {
                assert.strictEqual(String.fromCharCode(digit + 87),
                    BigInt(digit).toString(36));
            }
        }
    },
    {
        name: "BigInt.prototype.toString validates this and radix",
        body() {
            var toString = BigInt.prototype.toString;
            assert.throws(function () { toString.call(0); }, TypeError);
            assert.throws(function () { toString.call(true); }, TypeError);
            assert.throws(function () { toString.call({ x: 1n }); }, TypeError);
            assert.throws(function () { toString.call(BigInt.prototype); }, TypeError);
            assert.strictEqual("1", toString.call(1n));
            assert.strictEqual("1", toString.call(Object(1n)));

            [-1, 0, 1, 37, null, NaN, Infinity, -Infinity].forEach(function (radix) {
                assert.throws(function () { (0n).toString(radix); }, RangeError, "radix " + radix);
            });
            assert.throws(function () { (0n).toString(Symbol()); }, TypeError);
            assert.throws(function () { (0n).toString(0n); }, TypeError);
            assert.throws(function () {
                (0n).toString({ valueOf: undefined, toString: undefined });
            }, TypeError);

            assert.strictEqual(0, toString.length);
            assert.strictEqual("toString", toString.name);
            assert.throws(function () { new toString(); }, TypeError);
        }
    },
    {
        name: "BigInt.prototype.valueOf returns the primitive",
        body() {
            var valueOf = BigInt.prototype.valueOf;
            assert.strictEqual(0n, valueOf.call(0n));
            assert.strictEqual(0n, valueOf.call(Object(0n)));
            assert.strictEqual(-1n, valueOf.call(-1n));
            assert.strictEqual(1n, valueOf.call(BigInt(1)));
            assert.throws(function () { valueOf.call({}); }, TypeError);
            assert.throws(function () { valueOf.call(Object(1)); }, TypeError);
            assert.throws(function () { valueOf.call(1); }, TypeError);
            assert.throws(function () { valueOf.call("1"); }, TypeError);
            assert.throws(function () { valueOf.call(false); }, TypeError);
            assert.throws(function () { valueOf.call(BigInt.prototype); }, TypeError);
            assert.strictEqual(0, valueOf.length);
            assert.strictEqual("valueOf", valueOf.name);
            assert.throws(function () { new valueOf(); }, TypeError);
        }
    },
    {
        name: "String, template and concatenation stringify BigInt",
        body() {
            assert.strictEqual("1", String(1n));
            assert.strictEqual("-1", String(-1n));
            assert.strictEqual("0", String(0n));
            assert.strictEqual("255", String(BigInt(255)));
            assert.strictEqual("1", `${1n}`);
            assert.strictEqual("x1", "x" + 1n);
            assert.strictEqual("1x", 1n + "x");
            assert.strictEqual("1", "".concat(1n));
            assert.strictEqual("1", String(Object(1n)));
        }
    },
    {
        name: "JSON.stringify rejects BigInt unless toJSON or a replacer converts it",
        body() {
            assert.throws(function () { JSON.stringify(1n); }, TypeError);
            assert.throws(function () { JSON.stringify({ x: 1n }); }, TypeError);
            assert.throws(function () { JSON.stringify(Object(1n)); }, TypeError);

            var replacer = function (key, value) {
                return typeof value === "bigint" ? value.toString() : value;
            };
            assert.strictEqual('"1"', JSON.stringify(1n, replacer));
            assert.strictEqual('{"x":"1"}', JSON.stringify({ x: 1n }, replacer));

            var oldToJSON = BigInt.prototype.toJSON;
            BigInt.prototype.toJSON = function () { return this.toString(); };
            try {
                assert.strictEqual('"1"', JSON.stringify(1n));
                assert.strictEqual('"1"', JSON.stringify(Object(1n)));
            } finally {
                if (oldToJSON === undefined) {
                    delete BigInt.prototype.toJSON;
                } else {
                    BigInt.prototype.toJSON = oldToJSON;
                }
            }
        }
    },
    {
        name: "unsupported BigInt operations fail closed instead of aborting",
        body() {
            assert.strictEqual(-1n, -1n);
            var increment = 1n;
            increment++;
            assert.strictEqual(2n, increment);
            increment--;
            assert.strictEqual(1n, increment);
            assert.strictEqual(-2n, ~1n);
            assert.strictEqual(1, Number(1n));
            assert.isTrue(1n == 1);

            // Same-BigInt arithmetic and shifts are implemented; the mixed
            // forms below must throw a controlled TypeError, not abort.
            assert.strictEqual(1n, 1n ** 2n);
            assert.strictEqual(2n, 1n << 1n);
            assert.strictEqual(1n, BigInt(1) / BigInt(1));

            [
                function () { return 1n + 1; },
                function () { return 1n * 1; },
                function () { return 1n / 1; },
                function () { return 1n % 1; },
                function () { return 1n ** 1; },
                function () { return +1n; },
                function () { return 1n & 1; },
                function () { return 1n << 1; },
                function () { return 1n >>> 1; },
                function () { return BigInt(1) + 1; },
                function () { return BigInt(1) >>> 1; },
                function () { return 1n + Symbol("x"); }
            ].forEach(function (operation, index) {
                assert.throws(operation, TypeError, "operation " + index);
            });

            // A throwing coercion is observed once, and the exception class is
            // preserved instead of being replaced by an internal error.
            var calls = 0;
            assert.throws(function () {
                BigInt({ valueOf: function () { calls++; throw new RangeError("valueOf"); } });
            }, RangeError);
            assert.strictEqual(1, calls);
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
