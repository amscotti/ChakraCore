//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "BigInt/Number abstract equality is exact",
        body() {
            assert.isTrue(1n == 1);
            assert.isTrue(1 == 1n);
            assert.isTrue(1n == 1.0);
            assert.isTrue(0n == 0);
            assert.isTrue(0n == -0);
            assert.isTrue(-1n == -1);
            assert.isTrue(-1n == -1.0);
            assert.isTrue(-2n == -2);
            assert.isTrue(18446744073709551616n == 18446744073709551616);

            assert.isFalse(1n == 2);
            assert.isFalse(1n == 1.5);
            assert.isFalse(1.5 == 1n);
            assert.isFalse(1n == NaN);
            assert.isFalse(NaN == 1n);
            assert.isFalse(1n == Infinity);
            assert.isFalse(1n == -Infinity);
            assert.isFalse(-1n == Infinity);
            assert.isFalse(1n == Number.MAX_VALUE);

            // Values above 2**53 must not be collapsed through a double.
            assert.isTrue(9007199254740992n == 9007199254740992);
            assert.isFalse(9007199254740993n == 9007199254740992);
            assert.isTrue(9007199254740993n > 9007199254740992);
            assert.isTrue(9007199254740991n < 9007199254740992);
            assert.isFalse(9007199254740992n < 9007199254740992);
            assert.isTrue(18446744073709551615n < 18446744073709551616);
            assert.isTrue(18446744073709551617n > 18446744073709551616);
            assert.isFalse(18446744073709551615n == 18446744073709551616);
        }
    },
    {
        name: "BigInt/String and BigInt/Boolean abstract equality",
        body() {
            assert.isTrue(1n == "1");
            assert.isTrue("1" == 1n);
            assert.isTrue(10n == "10");
            assert.isTrue(1n == "01");
            assert.isTrue(1n == "+1");
            assert.isTrue(-5n == "-5");
            assert.isTrue(16n == "0x10");
            assert.isTrue(8n == "0o10");
            assert.isTrue(2n == "0b10");
            assert.isTrue(1n == " 1 ");
            assert.isTrue(0n == "");
            assert.isTrue(0n == " ");
            assert.isTrue(0n == false);
            assert.isTrue(false == 0n);
            assert.isTrue(1n == true);
            assert.isTrue(true == 1n);
            assert.isTrue(2n == "2");
            assert.isFalse(1n == "");
            assert.isFalse(1n == "1.0");
            assert.isFalse(1n == "1e0");
            assert.isFalse(1n == "10n");
            assert.isFalse(1n == "0x");
            assert.isFalse(1n == "abc");
            assert.isFalse(2n == true);
            assert.isFalse(-1n == true);
            assert.isFalse(1n == null);
            assert.isFalse(1n == undefined);
            assert.isFalse(1n == Symbol());
        }
    },
    {
        name: "BigInt wrapper and object abstract equality",
        body() {
            assert.isTrue(Object(1n) == 1n);
            assert.isTrue(1n == Object(1n));
            assert.isTrue(Object(1n) == 1);
            assert.isTrue(Object(1n) == "1");
            assert.isTrue(Object(1n) == true);
            assert.isFalse(Object(1n) == 2n);
            assert.isFalse(Object(1n) == 2);
            assert.isFalse(1n == Object(2n));
            assert.isFalse(Object(1n) == Object(1n));

            var wrapper = Object(1n);
            assert.isTrue(wrapper == wrapper);

            assert.isTrue(({ valueOf() { return 2n; } }) == 2n);
            assert.isTrue(2n == ({ valueOf() { return 2n; } }));
            assert.isTrue(({ valueOf() { return 2n; } }) == 2);
            assert.isTrue(2 == ({ valueOf() { return 2n; } }));
            assert.isTrue(({ valueOf() { return 2; } }) == 2n);
            assert.isTrue(2n == ({ valueOf() { return 2; } }));
            assert.isFalse(({ valueOf() { return 3n; } }) == 2n);
        }
    },
    {
        name: "Wrapper ToPrimitive observes the default/number/string hints",
        body() {
            var savedValueOf = BigInt.prototype.valueOf;
            var savedToString = BigInt.prototype.toString;
            var calls = [];
            try {
                BigInt.prototype.valueOf = function () {
                    calls.push("valueOf");
                    return 2n;
                };
                BigInt.prototype.toString = function () {
                    calls.push("toString");
                    return "3";
                };

                // Default hint is number and OrdinaryToPrimitive tries
                // valueOf first.
                assert.isTrue(Object(1n) == 2n);
                assert.strictEqual(calls.join(","), "valueOf");

                calls = [];
                assert.isTrue(Object(1n) < 3);
                assert.strictEqual(calls.join(","), "valueOf");

                calls = [];
                assert.strictEqual(Object(1n) * 1n, 2n);
                assert.strictEqual(calls.join(","), "valueOf");

                calls = [];
                assert.strictEqual(String(Object(1n)), "3");
                assert.strictEqual(calls.join(","), "toString");

                calls = [];
                assert.strictEqual(`${Object(1n)}`, "3");
                assert.strictEqual(calls.join(","), "toString");

                // A wrapper compared with null/undefined or another object is
                // unequal without invoking user code.
                calls = [];
                assert.isFalse(Object(1n) == null);
                assert.isFalse(Object(1n) == undefined);
                assert.isFalse(Object(1n) == {});
                assert.strictEqual(calls.length, 0);
            }
            finally {
                BigInt.prototype.valueOf = savedValueOf;
                BigInt.prototype.toString = savedToString;
            }
        }
    },
    {
        name: "Relational comparison with mixed Number operands",
        body() {
            assert.isTrue(1n < 2);
            assert.isTrue(1 < 2n);
            assert.isTrue(1n <= 1);
            assert.isTrue(1 <= 1n);
            assert.isTrue(1n >= 1);
            assert.isTrue(1 >= 1n);
            assert.isTrue(2n > 1);
            assert.isTrue(2 > 1n);
            assert.isFalse(1n < 1);
            assert.isFalse(1n > 1);
            assert.isFalse(1n < 0);
            assert.isTrue(-2n < -1);
            assert.isTrue(-2n < -1.5);
            assert.isFalse(-1n < -1);
            assert.isTrue(-1n > -2);

            assert.isTrue(1n < 1.5);
            assert.isFalse(1n < 0.5);
            assert.isTrue(0n < 0.5);
            assert.isTrue(1n <= 1.5);
            assert.isFalse(1n >= 1.5);
            assert.isFalse(-1n < -1.5);
            assert.isTrue(-1n > -1.5);
            assert.isFalse(1n < -0.5);
            assert.isTrue(-1n < -0.5);
            assert.isTrue(1n > -0.5);

            assert.isFalse(1n < NaN);
            assert.isFalse(1n <= NaN);
            assert.isFalse(1n > NaN);
            assert.isFalse(1n >= NaN);
            assert.isFalse(NaN < 1n);
            assert.isFalse(NaN <= 1n);
            assert.isFalse(NaN > 1n);
            assert.isFalse(NaN >= 1n);

            assert.isTrue(1n < Infinity);
            assert.isTrue(1n <= Infinity);
            assert.isFalse(1n > Infinity);
            assert.isTrue(1n > -Infinity);
            assert.isTrue(-Infinity < 1n);
            assert.isFalse(Infinity < 1n);
            assert.isTrue(1n < Number.MAX_VALUE);
            assert.isTrue(-1n > -Number.MAX_VALUE);

            assert.isTrue(9007199254740993n > 9007199254740992);
            assert.isTrue(9007199254740991n < 9007199254740992);
            assert.isTrue(18446744073709551615n < 18446744073709551616);
            assert.isTrue(18446744073709551617n > 18446744073709551616);
        }
    },
    {
        name: "Relational comparison with String, Boolean and object operands",
        body() {
            assert.isTrue(1n < "2");
            assert.isTrue("2" > 1n);
            assert.isFalse(10n < "9");
            assert.isTrue("10" < "9"); // both strings: lexicographic
            assert.isTrue(2n < "10"); // BigInt/string: numeric
            // BigInt/String uses StringToBigInt: non-integers and invalid
            // forms are undefined (all comparisons false), and large values
            // stay exact rather than going through double precision.
            assert.isFalse(0n < "1e0");
            assert.isFalse(0n <= "1e0");
            assert.isFalse(0n >= "1e0");
            assert.isFalse(1n < "1.5");
            assert.isFalse(1n <= "1.5");
            assert.isFalse(1n >= "1.5");
            assert.isFalse("1e0" > 0n);
            assert.isTrue(9007199254740992n < "9007199254740993");
            assert.isTrue(18446744073709551616n < "18446744073709551617");
            assert.isTrue(0n < true);
            assert.isTrue(false < 1n);
            assert.isTrue(1n > false);
            assert.isTrue(true > 0n);
            assert.isTrue(1n < Object(2n));
            assert.isTrue(Object(1n) < 2);
            assert.isTrue(Object(2n) > 1);
            assert.isTrue(Object(1n) < Object(2n));
            assert.isFalse(1n < Object(1n));
            assert.isFalse(Object(1n) > Object(1n));

            assert.throws(() => 1n < Symbol(), TypeError);
            assert.throws(() => Symbol() < 1n, TypeError);
            assert.throws(() => 1n <= Symbol(), TypeError);
            assert.throws(() => Symbol() >= 1n, TypeError);
        }
    },
    {
        name: "Relational ToPrimitive evaluates the original left operand first",
        body() {
            function makePrimitive(value, tag, log) {
                return {
                    [Symbol.toPrimitive](hint) {
                        log.push(tag + ":" + hint);
                        return value;
                    }
                };
            }

            var log = [];
            var left = makePrimitive(1n, "left", log);
            var right = makePrimitive(2, "right", log);
            assert.isTrue(left < right);
            assert.strictEqual(log.join(","), "left:number,right:number");

            log = [];
            left = makePrimitive(1n, "left", log);
            right = makePrimitive(2, "right", log);
            assert.isFalse(left > right);
            assert.strictEqual(log.join(","), "left:number,right:number");

            log = [];
            left = makePrimitive(1n, "left", log);
            right = makePrimitive(2, "right", log);
            assert.isTrue(left <= right);
            assert.strictEqual(log.join(","), "left:number,right:number");

            log = [];
            left = makePrimitive(1n, "left", log);
            right = makePrimitive(2, "right", log);
            assert.isFalse(left >= right);
            assert.strictEqual(log.join(","), "left:number,right:number");

            log = [];
            assert.isTrue(2n == makePrimitive(2n, "right", log));
            assert.strictEqual(log.join(","), "right:default");
        }
    },
    {
        name: "Wrapper multiplication unwraps with ToPrimitive before BigInt selection",
        body() {
            assert.strictEqual(Object(1n) * 1n, 1n);
            assert.strictEqual(Object(2n) * 3n, 6n);
            assert.strictEqual(3n * Object(2n), 6n);
            assert.strictEqual(Object(-2n) * 3n, -6n);
            assert.strictEqual({ valueOf: function () { return 2n; } } *
                { valueOf: function () { return 3n; } }, 6n);

            var savedValueOf = BigInt.prototype.valueOf;
            try {
                BigInt.prototype.valueOf = function () {
                    return savedValueOf.call(this) * 2n;
                };
                assert.strictEqual(Object(1n) * 1n, 2n);
            }
            finally {
                BigInt.prototype.valueOf = savedValueOf;
            }

            assert.throws(() => Object(1n) * 2, TypeError);
            assert.throws(() => 2 * Object(1n), TypeError);
            assert.throws(() => Object(1n) * "2", TypeError);
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
