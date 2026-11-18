//-------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertSameNumber(expected, actual, msg) {
    assert.isTrue(Object.is(expected, actual), (msg || "") + " (expected " + expected + ", got " + actual + ")");
}

var tests = [
    {
        name: "Math.sumPrecise descriptor, name, length, and non-constructibility",
        body: function () {
            var desc = Object.getOwnPropertyDescriptor(Math, "sumPrecise");
            assert.areEqual("function", typeof Math.sumPrecise);
            assert.isTrue(desc.writable, "Math.sumPrecise should be writable");
            assert.isFalse(desc.enumerable, "Math.sumPrecise should not be enumerable");
            assert.isTrue(desc.configurable, "Math.sumPrecise should be configurable");

            assert.areEqual("sumPrecise", Math.sumPrecise.name);
            assert.areEqual(1, Math.sumPrecise.length);

            assert.throws(function () { new Math.sumPrecise(); }, TypeError);
            assert.throws(function () { Reflect.construct(Math.sumPrecise, [[]]); }, TypeError);
        }
    },
    {
        name: "Math.sumPrecise handles empty iterables and signed zeros",
        body: function () {
            assertSameNumber(-0, Math.sumPrecise([]), "empty array");
            assertSameNumber(-0, Math.sumPrecise(""), "empty string");
            assertSameNumber(-0, Math.sumPrecise([-0]), "single -0");
            assertSameNumber(-0, Math.sumPrecise([-0, -0, -0]), "all -0");

            assertSameNumber(0, Math.sumPrecise([0]), "single +0");
            assertSameNumber(0, Math.sumPrecise([-0, 0]), "-0 then +0");
            assertSameNumber(0, Math.sumPrecise([0, -0]), "+0 then -0");
            assertSameNumber(0, Math.sumPrecise([1, -1]), "int cancellation");
            assertSameNumber(0, Math.sumPrecise([0.1, -0.1]), "float cancellation");
            assertSameNumber(0, Math.sumPrecise([1e308, -1e308]), "1e308 cancellation");
        }
    },
    {
        name: "Math.sumPrecise handles exact summation, DBL_MAX cancellation, and large counts",
        body: function () {
            assertSameNumber(6, Math.sumPrecise([1, 2, 3]));
            assertSameNumber(0.30000000000000004, Math.sumPrecise([0.1, 0.2]));
            assertSameNumber(0.1, Math.sumPrecise([1e30, 0.1, -1e30]));
            assertSameNumber(0.30000000000000004, Math.sumPrecise([1e308, 1e308, 0.1, 0.1, 1e30, 0.1, -1e30, -1e308, -1e308]));

            // Naive Shewchuk Two-Sum overflows to Infinity on this sequence, whereas
            // exact summation preserves the tiny negative offset below the tie.
            assertSameNumber(
                1.7976931348623157e+308,
                Math.sumPrecise([-1.0214557991173964e+61, 1.7976931348623157e+308, 8.98846567431158e+307, -8.988465674311579e+307]),
                "DBL_MAX cancellation below tie"
            );
            assertSameNumber(
                Infinity,
                Math.sumPrecise([1.7976931348623157e+308, 8.98846567431158e+307, -8.988465674311579e+307]),
                "DBL_MAX + 2^970 tie rounds to Infinity"
            );
            assertSameNumber(
                1.7976931348623157e+308,
                Math.sumPrecise([1.7976931348623157e+308, 8.988465674311579e+307, -8.988465674311579e+307]),
                "exact DBL_MAX cancellation"
            );
            assertSameNumber(
                -1.7976931348623157e+308,
                Math.sumPrecise([8.988465674311579e+307, 7.999999999999998, 7.029158107234023e-308, -2.2303483759420562e-172, -1.7976931348623157e+308, -8.98846567431158e+307]),
                "-DBL_MAX cancellation"
            );

            // Repeated DBL_MAX cancellation across >2048 iterations leaving Number.MIN_VALUE intact.
            var big = [];
            for (var i = 0; i < 600; i++) {
                big.push(Number.MAX_VALUE);
                big.push(-Number.MAX_VALUE);
            }
            big.push(Number.MIN_VALUE);
            for (var i = 0; i < 600; i++) {
                big.push(Number.MAX_VALUE);
                big.push(-Number.MAX_VALUE);
            }
            assertSameNumber(Number.MIN_VALUE, Math.sumPrecise(big), "2401-element DBL_MAX cancellation");
        }
    },
    {
        name: "Math.sumPrecise handles IEEE-754 ties-to-even, overflow, and subnormals",
        body: function () {
            // 2^970 is exactly 0.5 ulp above Number.MAX_VALUE; odd LSB rounds to Infinity.
            var halfUlpMax = Math.pow(2, 970);
            assertSameNumber(Infinity, Math.sumPrecise([Number.MAX_VALUE, halfUlpMax]), "MAX_VALUE + 2^970 -> +Infinity");
            assertSameNumber(-Infinity, Math.sumPrecise([-Number.MAX_VALUE, -halfUlpMax]), "-MAX_VALUE - 2^970 -> -Infinity");

            // Subtracting Number.MIN_VALUE drops strictly below halfway across 2044 bits -> rounds back to MAX_VALUE.
            assertSameNumber(
                Number.MAX_VALUE,
                Math.sumPrecise([Number.MAX_VALUE, halfUlpMax, -Number.MIN_VALUE]),
                "MAX_VALUE + 2^970 - MIN_VALUE -> MAX_VALUE"
            );
            assertSameNumber(
                -Number.MAX_VALUE,
                Math.sumPrecise([-Number.MAX_VALUE, -halfUlpMax, Number.MIN_VALUE]),
                "-MAX_VALUE - 2^970 + MIN_VALUE -> -MAX_VALUE"
            );

            // Carry out of bit 52: (2 - 2^-52) + 2^-53 -> 2
            assertSameNumber(2, Math.sumPrecise([2 - Math.pow(2, -52), Math.pow(2, -53)]), "carry out at 2.0");

            // Subnormal arithmetic and normal/subnormal boundary ties-to-even.
            assertSameNumber(Number.MIN_VALUE, Math.sumPrecise([Number.MIN_VALUE, Number.MIN_VALUE, -Number.MIN_VALUE]), "subnormal exact");
            assertSameNumber(2 * Number.MIN_VALUE, Math.sumPrecise([Number.MIN_VALUE, Number.MIN_VALUE]), "2 * MIN_VALUE");
            var minNormal = Math.pow(2, -1022);
            assertSameNumber(minNormal, Math.sumPrecise([minNormal - Number.MIN_VALUE, Number.MIN_VALUE]), "largest subnormal + MIN_VALUE -> smallest normal");

            var pow1021 = Math.pow(2, -1021);
            assertSameNumber(pow1021, Math.sumPrecise([pow1021, Number.MIN_VALUE]), "even LSB tie rounds down");
            var pow1021Odd = pow1021 * (1 + Math.pow(2, -52));
            assertSameNumber(
                pow1021 * (1 + 2 * Math.pow(2, -52)),
                Math.sumPrecise([pow1021Odd, Number.MIN_VALUE]),
                "odd LSB tie rounds up"
            );

            // Cross-word boundary at 2^-1010 (bit index 64 in superaccumulator).
            var twoNeg1010 = Math.pow(2, -1010);
            var twoNeg1063 = Math.pow(2, -1063); // exact half-ulp at 2^-1010
            assertSameNumber(twoNeg1010, Math.sumPrecise([twoNeg1010, twoNeg1063]), "cross-word even tie rounds down");
            assertSameNumber(
                twoNeg1010 * (1 + Math.pow(2, -52)),
                Math.sumPrecise([twoNeg1010, twoNeg1063, Number.MIN_VALUE]),
                "cross-word sticky bit rounds up"
            );
        }
    },
    {
        name: "Math.sumPrecise handles NaN, Infinity, and other iterables",
        body: function () {
            assertSameNumber(Infinity, Math.sumPrecise([Infinity]));
            assertSameNumber(Infinity, Math.sumPrecise([Infinity, 1e308, -1e308]));
            assertSameNumber(-Infinity, Math.sumPrecise([-Infinity, 1e308]));
            assertSameNumber(NaN, Math.sumPrecise([NaN]));
            assertSameNumber(NaN, Math.sumPrecise([NaN, -0]));
            assertSameNumber(NaN, Math.sumPrecise([Infinity, -Infinity]));
            assertSameNumber(NaN, Math.sumPrecise([-Infinity, Infinity, 1]));

            function* gen() {
                yield 10;
                yield 0.5;
                yield -2.5;
            }
            assertSameNumber(8, Math.sumPrecise(gen()));
            assertSameNumber(6, Math.sumPrecise(new Set([1, 2, 3])));
            assertSameNumber(0.30000000000000004, Math.sumPrecise(new Float64Array([0.1, 0.2])));
        }
    },
    {
        name: "Math.sumPrecise validation, non-short-circuiting, and IteratorClose semantics",
        body: function () {
            assert.throws(function () { Math.sumPrecise(); }, TypeError);
            assert.throws(function () { Math.sumPrecise(undefined); }, TypeError);
            assert.throws(function () { Math.sumPrecise(null); }, TypeError);
            assert.throws(function () { Math.sumPrecise(1, 2); }, TypeError);
            assert.throws(function () { Math.sumPrecise({}); }, TypeError);

            // Reject boxed Number, BigInt, String, Boolean, Object without invoking coercion.
            var coercions = 0;
            var evil = {
                valueOf: function () { coercions++; return 1; },
                toString: function () { coercions++; return "1"; }
            };
            assert.throws(function () { Math.sumPrecise([new Number(1)]); }, TypeError);
            assert.throws(function () { Math.sumPrecise([0n]); }, TypeError);
            assert.throws(function () { Math.sumPrecise(["1"]); }, TypeError);
            assert.throws(function () { Math.sumPrecise([true]); }, TypeError);
            assert.throws(function () { Math.sumPrecise([1, , 2]); }, TypeError);
            assert.throws(function () { Math.sumPrecise([evil]); }, TypeError);
            assert.areEqual(0, coercions);

            // Must NOT short-circuit on NaN or +Infinity + -Infinity: later non-Number still closes & throws TypeError.
            function makeClosingIterable(values, returnBehavior) {
                var idx = 0;
                var state = { nextCalls: 0, returnCalls: 0, returnThisOk: false };
                var iterator = {
                    next: function () {
                        state.nextCalls++;
                        if (idx < values.length) {
                            return { done: false, value: values[idx++] };
                        }
                        return { done: true, value: "ignored" };
                    },
                    return: function () {
                        state.returnCalls++;
                        state.returnThisOk = (this === iterator);
                        return returnBehavior();
                    }
                };
                return {
                    state: state,
                    iterable: {
                        [Symbol.iterator]: function () { return iterator; }
                    }
                };
            }

            var case1 = makeClosingIterable([NaN, evil], function () { return {}; });
            assert.throws(function () { Math.sumPrecise(case1.iterable); }, TypeError);
            assert.areEqual(0, coercions);
            assert.areEqual(2, case1.state.nextCalls);
            assert.areEqual(1, case1.state.returnCalls);
            assert.isTrue(case1.state.returnThisOk);

            // Original TypeError wins over return() throwing a different error or returning primitive.
            var case2 = makeClosingIterable([Infinity, -Infinity, new Number(5)], function () {
                throw new RangeError("return threw");
            });
            assert.throws(function () { Math.sumPrecise(case2.iterable); }, TypeError);
            assert.areEqual(3, case2.state.nextCalls);
            assert.areEqual(1, case2.state.returnCalls);

            var case3 = makeClosingIterable([{}], function () { return 42; });
            assert.throws(function () { Math.sumPrecise(case3.iterable); }, TypeError);
            assert.areEqual(1, case3.state.returnCalls);

            // Done: true does not validate value or call return().
            var case4 = makeClosingIterable([1, 2], function () { return {}; });
            assertSameNumber(3, Math.sumPrecise(case4.iterable));
            assert.areEqual(0, case4.state.returnCalls);

            // Abrupt next() / done / value must NOT call return().
            var abruptReturnCalls = 0;
            var abruptIterable = {
                [Symbol.iterator]: function () {
                    return {
                        next: function () { throw new RangeError("next threw"); },
                        return: function () { abruptReturnCalls++; return {}; }
                    };
                }
            };
            assert.throws(function () { Math.sumPrecise(abruptIterable); }, RangeError);
            assert.areEqual(0, abruptReturnCalls);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
