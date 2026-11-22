//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "Iterator.prototype.includes SameValueZero and basic matches",
        body: function () {
            assert.isTrue([1, NaN, 3].values().includes(NaN), "NaN matches NaN");
            assert.isFalse([1, 2, 3].values().includes(NaN), "NaN missing");
            assert.isTrue([+0].values().includes(-0), "+0 matches -0");
            assert.isTrue([-0].values().includes(+0), "-0 matches +0");
            assert.isTrue([1n, 2n].values().includes(2n), "BigInt SameValueZero");
            assert.isFalse([1n].values().includes(1), "BigInt does not match Number");

            var sym = Symbol("k");
            assert.isTrue([sym].values().includes(sym), "Symbol identity match");
            assert.isFalse([sym].values().includes(Symbol("k")), "Distinct Symbol miss");

            var obj = { a: 1 };
            assert.isTrue([obj].values().includes(obj), "Object identity match");
            assert.isFalse([obj].values().includes({ a: 1 }), "Distinct object miss");
            assert.isTrue([1, , 3].values().includes(undefined), "Sparse array hole yields undefined");
        }
    },
    {
        name: "Iterator.prototype.includes closes on match and handles skippedElements validation",
        body: function () {
            var closed = 0;
            var steps = 0;
            var iter = {
                __proto__: Iterator.prototype,
                next: function () {
                    steps++;
                    return { done: false, value: steps };
                },
                return: function () {
                    closed++;
                    return {};
                }
            };
            assert.isTrue(iter.includes(4), "match found at step 4");
            assert.strictEqual(steps, 4, "stops stepping on match");
            assert.strictEqual(closed, 1, "calls return() immediately on match");

            // Exhaustion must not access return.
            var exhaustedIter = {
                __proto__: Iterator.prototype,
                next: function () {
                    return { done: true, value: undefined };
                },
                get return() {
                    throw new Error("return should not be read on exhaustion");
                }
            };
            assert.isFalse(exhaustedIter.includes(1), "exhausted returns false");

            // Validation failures close underlying iterator before reading next and swallow return() errors.
            function assertValidationCloses(arg, ExpectedError, label) {
                var returnCalled = 0;
                var target = {
                    __proto__: Iterator.prototype,
                    get next() {
                        throw new Error("next should not be read: " + label);
                    },
                    return: function () {
                        returnCalled++;
                        throw new Error("return error should be swallowed");
                    }
                };
                assert.throws(function () { target.includes(0, arg); }, ExpectedError, label);
                assert.strictEqual(returnCalled, 1, label + " closed underlying iterator");
            }

            assertValidationCloses(NaN, TypeError, "NaN skippedElements");
            assertValidationCloses(0.5, TypeError, "0.5 skippedElements");
            assertValidationCloses(-0.5, TypeError, "-0.5 skippedElements");
            assertValidationCloses("1", TypeError, "string skippedElements");
            assertValidationCloses(null, TypeError, "null skippedElements");
            assertValidationCloses(true, TypeError, "boolean skippedElements");
            assertValidationCloses(1n, TypeError, "BigInt skippedElements");
            assertValidationCloses(Symbol(), TypeError, "Symbol skippedElements");
            assertValidationCloses(new Number(1), TypeError, "Number object skippedElements");
            assertValidationCloses({ valueOf: function () { throw new Error("no coercion"); } }, TypeError, "object no valueOf");
            assertValidationCloses(-1, RangeError, "-1 skippedElements");
            assertValidationCloses(-Infinity, RangeError, "-Infinity skippedElements");
            assertValidationCloses(Number.MAX_SAFE_INTEGER + 1, RangeError, "MAX_SAFE_INTEGER + 1");

            // Valid boundary skippedElements.
            assert.isFalse([10, 20, 30].values().includes(10, 1), "skipped index 0");
            assert.isTrue([10, 20, 30].values().includes(20, 1), "matched after skip");
            assert.isTrue([10, 20, 30].values().includes(10, -0), "-0 skips nothing");
            assert.isFalse([10, 20, 30].values().includes(10, Infinity), "+Infinity skips all");
            assert.isFalse([10, 20].values().includes(10, Number.MAX_SAFE_INTEGER), "MAX_SAFE_INTEGER accepted");

            // Abrupt return() on match propagates; throwing next/getter does not close.
            var throwingReturn = {
                __proto__: Iterator.prototype,
                next: function () { return { done: false, value: 42 }; },
                return: function () { throw new RangeError("close-fail"); }
            };
            assert.throws(function () { throwingReturn.includes(42); }, RangeError, "return() throw propagates on match");

            var nonObjectReturn = {
                __proto__: Iterator.prototype,
                next: function () { return { done: false, value: 42 }; },
                return: function () { return 123; }
            };
            assert.throws(function () { nonObjectReturn.includes(42); }, TypeError, "non-object return() result throws TypeError");

            var throwingNext = {
                __proto__: Iterator.prototype,
                next: function () { throw new RangeError("next-fail"); },
                get return() { throw new Error("return must not be accessed"); }
            };
            assert.throws(function () { throwingNext.includes(42); }, RangeError, "next() throw skips return");
        }
    },
    {
        name: "Iterator.prototype.join basic, nullish, separator, and closing semantics",
        body: function () {
            assert.strictEqual([].values().join(), "", "empty iterator");
            assert.strictEqual(["only"].values().join(), "only", "single element");
            assert.strictEqual([null].values().join(","), "", "single null element");
            assert.strictEqual([null, "a"].values().join(","), ",a", "null first element still emits separator");
            assert.strictEqual([undefined, null].values().join(","), ",", "two nullish elements emit separator");
            assert.strictEqual(["a", null, "b", undefined, "c"].values().join(), "a,,b,,c", "default comma separator");
            assert.strictEqual(["a", "b", "c"].values().join(""), "abc", "empty separator");
            assert.strictEqual(["a", "b", "c"].values().join(undefined), "a,b,c", "explicit undefined separator");
            assert.strictEqual(["a", "b", "c"].values().join(null), "anullbnullc", "null separator coerced");

            // Effect order: separator ToString before get next.
            var effects = [];
            var sep = {
                toString: function () {
                    effects.push("sep");
                    return "|";
                }
            };
            var iterEffects = {
                get next() {
                    effects.push("next");
                    var i = 0;
                    return function () {
                        i++;
                        return i <= 2 ? { done: false, value: i } : { done: true, value: undefined };
                    };
                }
            };
            assert.strictEqual(Iterator.prototype.join.call(iterEffects, sep), "1|2");
            assert.strictEqual(effects.join(","), "sep,next", "separator coerced before next lookup");

            // Abrupt separator coercion closes iterator before reading next.
            var closedOnSep = 0;
            var badSepIter = {
                get next() { throw new Error("next should not be read"); },
                return: function () { closedOnSep++; return {}; }
            };
            assert.throws(function () {
                Iterator.prototype.join.call(badSepIter, { toString: function () { throw new RangeError("bad-sep"); } });
            }, RangeError, "abrupt separator toString propagates");
            assert.strictEqual(closedOnSep, 1, "abrupt separator closes iterator");

            var closedOnSymSep = 0;
            var symSepIter = {
                get next() { throw new Error("next should not be read"); },
                return: function () { closedOnSymSep++; return {}; }
            };
            assert.throws(function () {
                Iterator.prototype.join.call(symSepIter, Symbol("s"));
            }, TypeError, "Symbol separator throws TypeError");
            assert.strictEqual(closedOnSymSep, 1, "Symbol separator closes iterator");

            // Abrupt element coercion closes iterator.
            var closedOnElem = 0;
            var badElemIter = {
                next: function () {
                    return { done: false, value: { toString: function () { throw new RangeError("bad-elem"); } } };
                },
                return: function () { closedOnElem++; return {}; }
            };
            assert.throws(function () { Iterator.prototype.join.call(badElemIter, ","); }, RangeError);
            assert.strictEqual(closedOnElem, 1, "abrupt element toString closes iterator");

            var closedOnSymElem = 0;
            var symElemIter = {
                next: function () { return { done: false, value: Symbol("x") }; },
                return: function () { closedOnSymElem++; return {}; }
            };
            assert.throws(function () { Iterator.prototype.join.call(symElemIter, ","); }, TypeError);
            assert.strictEqual(closedOnSymElem, 1, "Symbol element closes iterator");

            // Next getter/step errors do not close iterator.
            var noCloseIter = {
                get next() { throw new RangeError("no-next"); },
                get return() { throw new Error("return should not be read"); }
            };
            assert.throws(function () { Iterator.prototype.join.call(noCloseIter); }, RangeError);
        }
    },
    {
        name: "Iterator.prototype.includes and join long stream, CompoundString, GC, and JIT warmup",
        body: function () {
            // Long stream join with CompoundString separator and multi-char items.
            var count = 50000;
            var n = 0;
            var longIter = {
                __proto__: Iterator.prototype,
                next: function () {
                    if (n < count) {
                        var v = "x" + (n % 10);
                        n++;
                        return { done: false, value: v };
                    }
                    return { done: true, value: undefined };
                }
            };
            var compoundSep = ["-", "-"].values().join("");
            var joined = longIter.join(compoundSep);
            assert.strictEqual(joined.length, count * 2 + (count - 1) * 2, "long join length");
            assert.strictEqual(joined.slice(0, 10), "x0--x1--x2", "long join prefix");
            assert.strictEqual(joined.slice(-10), "x7--x8--x9", "long join suffix");

            // Reentrant toString allocating strings under GC pressure.
            var gcStep = 0;
            var gcIter = {
                __proto__: Iterator.prototype,
                next: function () {
                    if (gcStep < 200) {
                        var k = gcStep++;
                        return {
                            done: false,
                            value: {
                                toString: function () {
                                    var tmp = [];
                                    for (var j = 0; j < 20; j++) {
                                        tmp.push("alloc_" + k + "_" + j);
                                    }
                                    return "[" + k + "]";
                                }
                            }
                        };
                    }
                    return { done: true, value: undefined };
                }
            };
            var gcJoined = gcIter.join("|");
            assert.isTrue(gcJoined.indexOf("[0]|[1]|[2]") === 0, "GC join prefix intact");
            assert.isTrue(gcJoined.endsWith("[198]|[199]"), "GC join suffix intact");

            // JIT warmup loop for both includes and join.
            function runWarmup(idx) {
                var a = [1, 2, NaN, -0, "s" + idx];
                assert.isTrue(a.values().includes(NaN), "warmup includes NaN");
                assert.isTrue(a.values().includes(+0), "warmup includes +0/-0");
                assert.isFalse(a.values().includes("missing", 2), "warmup includes miss with skip");
                assert.strictEqual([idx, null, "end"].values().join(":"), idx + "::end", "warmup join");
            }
            for (var w = 0; w < 500; w++) {
                runWarmup(w);
            }
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
