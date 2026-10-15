//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "toReversed reads holes and does not mutate",
        body() {
            var arr = [0, , 2, , 4];
            Array.prototype[3] = 3;
            try {
                var result = arr.toReversed();
                assert.areEqual([4, 3, 2, undefined, 0], result);
                assert.isTrue(result.hasOwnProperty(3));
                assert.isFalse(result === arr);
                assert.isFalse(arr.hasOwnProperty(1));
                assert.isFalse(arr.hasOwnProperty(3));
                assert.areEqual(0, arr[0]);
                assert.areEqual(2, arr[2]);
                assert.areEqual(4, arr[4]);
                assert.areEqual(5, arr.length);
            } finally {
                delete Array.prototype[3];
            }
            var empty = [];
            assert.areEqual([], empty.toReversed());
            assert.isFalse(empty.toReversed() === empty);
        }
    },
    {
        name: "toSorted rejects a non-callable comparefn before length",
        body() {
            var samples = [1, null, true, "x", /a/, {}, []];
            for (var i = 0; i < samples.length; i++) {
                var seen = false;
                var obj = {
                    get length() {
                        seen = true;
                        return 1;
                    }
                };
                assert.throws(function () {
                    Array.prototype.toSorted.call(obj, samples[i]);
                }, TypeError);
                assert.isFalse(seen);
            }
        }
    },
    {
        name: "toSorted reads holes, keeps the receiver, and string-sorts",
        body() {
            var arr = [3, , 4, , 1];
            Array.prototype[3] = 2;
            try {
                var result = arr.toSorted();
                assert.areEqual([1, 2, 3, 4, undefined], result);
                assert.isTrue(result.hasOwnProperty(4));
                assert.isFalse(result === arr);
                assert.isFalse(arr.hasOwnProperty(1));
                assert.isFalse(arr.hasOwnProperty(3));
                assert.areEqual(3, arr[0]);
                assert.areEqual(4, arr[2]);
                assert.areEqual(1, arr[4]);
            } finally {
                delete Array.prototype[3];
            }

            assert.areEqual([10, 2], [10, 2].toSorted());

            var stable = [{ n: 1, i: 0 }, { n: 1, i: 1 }, { n: 1, i: 2 }];
            var sorted = stable.toSorted(function () { return 0; });
            assert.areEqual(0, sorted[0].i);
            assert.areEqual(1, sorted[1].i);
            assert.areEqual(2, sorted[2].i);
            assert.isFalse(sorted === stable);
            assert.areEqual(0, stable[0].i);

            var emptySorted = [];
            assert.areEqual([], emptySorted.toSorted());
            assert.isFalse(emptySorted.toSorted() === emptySorted);
        }
    },
    {
        name: "array copy-by-change ignores species and constructor",
        body() {
            var calls = 0;
            var b = [3, 1, 2];
            Object.defineProperty(b, "constructor", {
                get: function () {
                    calls++;
                    throw new Error("ctor");
                }
            });
            var sorted = b.toSorted();
            var reversed = b.toReversed();
            assert.areEqual(0, calls);
            assert.isTrue(sorted instanceof Array);
            assert.isTrue(Object.getPrototypeOf(sorted) === Array.prototype);
            assert.isTrue(Object.getPrototypeOf(reversed) === Array.prototype);
            assert.areEqual([1, 2, 3], sorted);

            var a = [1, 2];
            a.constructor = {};
            Object.defineProperty(a.constructor, Symbol.species, {
                get: function () {
                    calls++;
                    throw new Error("species");
                }
            });
            a.toReversed();
            a.toSorted();
            a.toSpliced(0, 0);
            a.with(0, 9);
            assert.areEqual(0, calls);
        }
    },
    {
        name: "toSpliced copies, inserts, and turns a hole into an own undefined",
        body() {
            var arr = [1, , 3];
            var copy = arr.toSpliced(1, undefined);
            assert.areEqual([1, undefined, 3], copy);
            assert.isTrue(copy.hasOwnProperty(1));
            assert.isFalse(copy === arr);
            assert.isFalse(arr.hasOwnProperty(1));
            assert.areEqual(3, arr.length);

            assert.areEqual([], [1, 2, 3].toSpliced(undefined));
            assert.areEqual([1, 2, 3], [1, 2, 3].toSpliced(undefined, undefined));
            assert.areEqual([1, 2, 3], [1, 2, 3].toSpliced());
            assert.areEqual([1, 2, 9, 4], [1, 2, 3, 4].toSpliced(-2, 1, 9));
            assert.areEqual([1, 8, 9, 3], [1, 2, 3].toSpliced(1, 1, 8, 9));

            var holed = [1, , 3];
            var filled = holed.toSpliced(1, 0);
            assert.areEqual([1, undefined, 3], filled);
            assert.isTrue(filled.hasOwnProperty(1));
            assert.isFalse(holed.hasOwnProperty(1));
        }
    },
    {
        name: "toSpliced overflow throws before copying",
        body() {
            // newLen == 2^32 is ArrayCreate's RangeError. TypeError is only above 2^53-1.
            var seen = 0;
            var atUint32 = {
                get length() {
                    seen++;
                    return 0xFFFFFFFF;
                },
                get 0() {
                    throw new Error("element");
                }
            };
            assert.throws(function () {
                Array.prototype.toSpliced.call(atUint32, 0, 0, 1);
            }, RangeError);
            assert.areEqual(1, seen);

            var atLimit = {
                get length() {
                    return 9007199254740991;
                }
            };
            assert.throws(function () {
                Array.prototype.toSpliced.call(atLimit, 0, 0, 1);
            }, TypeError);
        }
    },
    {
        name: "with replaces one index and rejects an out-of-range index",
        body() {
            var arr = [1, 2, 3];
            var replaced = arr.with(-2, 9);
            assert.areEqual([1, 9, 3], replaced);
            assert.areEqual([1, 2, 3], arr);
            assert.isFalse(replaced === arr);
            assert.throws(function () { arr.with(3, 0); }, RangeError);
            assert.throws(function () { arr.with(-4, 0); }, RangeError);

            var like = { 0: 1, 1: 2, 2: 3, length: 3 };
            assert.areEqual([1, 9, 3], Array.prototype.with.call(like, -2, 9));
            assert.areEqual(2, like[1]);
        }
    },
    {
        name: "typed array toReversed, toSorted, and with ignore species",
        body() {
            function same(actual, expected) {
                assert.areEqual(expected.length, actual.length);
                for (var i = 0; i < expected.length; i++) {
                    assert.areEqual(expected[i], actual[i]);
                }
            }

            var bytes = new Uint8Array([1, 2, 3]);
            same(bytes.toReversed(), [3, 2, 1]);
            assert.areEqual(1, bytes[0]);
            var numeric = new Uint8Array([10, 2, 10]);
            same(numeric.toSorted(), [2, 10, 10]);
            assert.areEqual(10, numeric[0]);
            same(numeric.toSorted(function (a, b) { return b - a; }), [10, 10, 2]);

            var floats = new Float64Array([10, 2, 3]);
            same(floats.toReversed(), [3, 2, 10]);
            same(floats.toSorted(), [2, 3, 10]);
            same(floats.with(-1, 7), [10, 2, 7]);
            assert.areEqual(3, floats[2]);
            assert.throws(function () { floats.with(3, 1); }, RangeError);
            assert.throws(function () { floats.with(-4, 1); }, RangeError);

            var empty = new Uint8Array(0);
            var emptyCopy = empty.toReversed();
            assert.areEqual(0, emptyCopy.length);
            assert.isFalse(emptyCopy === empty);
            assert.isTrue(emptyCopy instanceof Uint8Array);
            assert.isFalse(empty.toSorted() === empty);
            var emptyFloat = new Float64Array(0);
            assert.isFalse(emptyFloat.toSorted() === emptyFloat);

            var calls = 0;
            var ta = new Uint8Array(0);
            Object.defineProperty(ta, "constructor", {
                get: function () {
                    calls++;
                    throw new Error("ctor");
                }
            });
            var sorted = ta.toSorted();
            assert.areEqual(0, calls);
            assert.isTrue(Object.getPrototypeOf(sorted) === Uint8Array.prototype);

            var ta2 = new Float64Array([1]);
            ta2.constructor = {};
            Object.defineProperty(ta2.constructor, Symbol.species, {
                get: function () {
                    calls++;
                    throw new Error("species");
                }
            });
            ta2.toReversed();
            ta2.toSorted();
            ta2.with(0, 4);
            assert.areEqual(0, calls);

            var bad = new Uint8Array([1, 2]);
            assert.throws(function () { bad.toSorted(1); }, TypeError);
            assert.areEqual(1, bad[0]);
        }
    },
    {
        name: "toReversed survives a throwing getter and a reentrant getter",
        body() {
            var throwing = {
                length: 2,
                get 0() { throw new Error("index"); },
                1: 5
            };
            assert.throws(function () {
                Array.prototype.toReversed.call(throwing);
            }, Error);
            assert.areEqual([7], Array.prototype.toReversed.call({ length: 1, 0: 7 }));

            var calls = 0;
            var reentered = {
                length: 2,
                get 0() { return 1; },
                get 1() {
                    calls++;
                    if (calls === 1) {
                        Array.prototype.toReversed.call({ length: 1, 0: 9 });
                    }
                    return 2;
                }
            };
            assert.areEqual([2, 1], Array.prototype.toReversed.call(reentered));
            assert.areEqual(1, calls);
            assert.areEqual([4, 3], Array.prototype.toReversed.call({ length: 2, 0: 3, 1: 4 }));
        }
    },
    {
        name: "typed array with applies ToNumber before detach and range errors",
        body() {
            var ta = new Uint8Array([1, 2, 3]);
            var seen = false;
            assert.throws(function () {
                ta.with(
                    { valueOf: function () { ArrayBuffer.detach(ta.buffer); return 1; } },
                    { valueOf: function () { seen = true; throw new Error("value"); } }
                );
            }, Error, "value throw wins over a detach during index coercion", "value");
            assert.isTrue(seen);

            var ta2 = new Uint8Array([7]);
            assert.throws(function () {
                ta2.with(0, { valueOf: function () { ArrayBuffer.detach(ta2.buffer); return 9; } });
            }, RangeError);

            var ta3 = new Uint8Array([1, 2, 3]);
            assert.throws(function () {
                ta3.with(1, { valueOf: function () { ArrayBuffer.detach(ta3.buffer); return 9; } });
            }, RangeError);

            var ta4 = new Uint8Array([1, 2, 3]);
            var seenRange = false;
            assert.throws(function () {
                ta4.with(99, { valueOf: function () { seenRange = true; throw new Error("value"); } });
            }, Error, "out-of-range index still runs ToNumber", "value");
            assert.isTrue(seenRange);
        }
    },
    {
        name: "Array.prototype[Symbol.unscopables] lists the copy methods except with",
        body() {
            var unscopables = Array.prototype[Symbol.unscopables];
            assert.isTrue(unscopables.toReversed === true);
            assert.isTrue(unscopables.toSorted === true);
            assert.isTrue(unscopables.toSpliced === true);
            assert.isFalse(Object.prototype.hasOwnProperty.call(unscopables, "with"));
        }
    },
    {
        name: "with stores the replacement value uncoerced",
        body() {
            var arr = [1, 2, 3];
            var obj = {};
            var replaced = arr.with(1, obj);
            assert.isTrue(replaced[1] === obj);
            assert.areEqual(2, arr[1]);
            var text = arr.with(0, "9");
            assert.isTrue(text[0] === "9");
            assert.areEqual(1, arr[0]);
        }
    },
    {
        name: "typed toSorted of length 512 uses the mergesort path and leaves the source",
        body() {
            var n = 512;
            var source = new Float64Array(n);
            for (var i = 0; i < n; i++) {
                source[i] = n - 1 - i;
            }
            var sorted = source.toSorted();
            assert.isFalse(sorted === source);
            assert.isTrue(sorted instanceof Float64Array);
            assert.areEqual(n, sorted.length);
            for (var i = 0; i < n; i++) {
                assert.areEqual(i, sorted[i]);
                assert.areEqual(n - 1 - i, source[i]);
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
