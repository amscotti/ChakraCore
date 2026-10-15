//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var setMethods = ["union", "intersection", "difference", "symmetricDifference", "isSubsetOf", "isSupersetOf", "isDisjointFrom"];

// Builds a plain Set-like object whose getters and methods log into order.
function makeRecorder(order, values, hasResult) {
    return {
        get size() {
            order.push("get size");
            return {
                valueOf: function () {
                    order.push("toNumber size");
                    return values.length;
                }
            };
        },
        get has() {
            order.push("get has");
            return function (v) {
                order.push("call has " + v);
                return hasResult(v);
            };
        },
        get keys() {
            order.push("get keys");
            return function () {
                order.push("call keys");
                var index = 0;
                return {
                    next: function () {
                        return { done: index >= values.length, value: values[index++] };
                    }
                };
            };
        }
    };
}

var tests = [
    {
        name: "all seven methods are installed with length 1 and their own name",
        body() {
            for (var i = 0; i < setMethods.length; i++) {
                var name = setMethods[i];
                var fn = Set.prototype[name];
                assert.areEqual("function", typeof fn, name + " is a function");
                assert.areEqual(1, fn.length, name + " length");
                assert.areEqual(name, fn.name, name + " name");
                assert.isFalse(Object.prototype.hasOwnProperty.call(fn, "prototype"), name + " has no prototype property");
                assert.throws(function () { new fn(); }, TypeError, name + " is not a constructor");
            }
        }
    },
    {
        name: "set arguments produce new plain Sets in insertion order",
        body() {
            var s1 = new Set([1, 2]);
            var s2 = new Set([2, 3]);
            var combined = s1.union(s2);
            assert.isTrue(combined instanceof Set);
            assert.isFalse(combined === s1);
            assert.areEqual([1, 2, 3], [...combined]);
            assert.areEqual([2], [...s1.intersection(s2)]);
            assert.areEqual([1], [...s1.difference(s2)]);
            assert.areEqual([1, 3], [...s1.symmetricDifference(s2)]);
            assert.areEqual([], [...s1.difference(s1)]);
            assert.areEqual([1, 2], [...s1.union(s1)]);

            // receiver order leads; new values from the argument follow
            assert.areEqual([2, 3, 1], [...new Set([2, 3]).union(new Set([1, 2]))]);

            var empty = new Set([]);
            assert.areEqual([1, 2], [...empty.union(s1)]);
            assert.areEqual([1, 2], [...s1.union(empty)]);
            assert.areEqual([], [...empty.union(empty)]);
            assert.areEqual([], [...empty.intersection(s1)]);
        }
    },
    {
        name: "is* methods return booleans",
        body() {
            var s1 = new Set([1, 2]);
            var s2 = new Set([2, 3]);
            var inner = new Set([2]);
            assert.isFalse(s1.isSubsetOf(s2));
            assert.isTrue(inner.isSubsetOf(s1));
            assert.isTrue(s1.isSubsetOf(s1));
            assert.isFalse(s1.isSupersetOf(s2));
            assert.isTrue(s1.isSupersetOf(inner));
            assert.isTrue(s1.isSupersetOf(s1));
            assert.isFalse(s1.isDisjointFrom(s2));
            assert.isTrue(s1.isDisjointFrom(new Set([8, 9])));
            assert.isTrue(new Set([]).isDisjointFrom(s1));
        }
    },
    {
        name: "results ignore species and constructor and never call the receiver's add",
        body() {
            class MySet extends Set {
                static get [Symbol.species]() {
                    speciesCalls++;
                    return Set;
                }
                has(...rest) {
                    methodCalls++;
                    return super.has(...rest);
                }
                keys(...rest) {
                    methodCalls++;
                    return super.keys(...rest);
                }
            }
            var speciesCalls = 0;
            var methodCalls = 0;
            var addCalls = 0;
            var mine = new MySet([1, 2]);
            var other = new Set([2, 3]);
            var bigger = new Set([1, 2, 3]);
            var smaller = new Set([1]);
            var sup = new Set([1, 2]);
            var originalAdd = Set.prototype.add;
            Set.prototype.add = function (...rest) {
                addCalls++;
                return originalAdd.apply(this, rest);
            };
            try {
                assert.areEqual([1, 2, 3], [...mine.union(other)]);
                assert.areEqual([2], [...mine.intersection(other)]);
                assert.areEqual([1], [...mine.difference(other)]);
                assert.areEqual([1, 3], [...mine.symmetricDifference(other)]);
                assert.isTrue(mine.isSubsetOf(bigger));
                assert.isTrue(sup.isSupersetOf(smaller));
                assert.isFalse(mine.isDisjointFrom(other));
                var result = mine.union(other);
                assert.isTrue(result instanceof Set);
                assert.isFalse(result instanceof MySet);
                assert.areEqual(0, speciesCalls, "species is never called");
                assert.areEqual(0, methodCalls, "receiver has/keys are never called");
                assert.areEqual(0, addCalls, "add is never called");
            } finally {
                Set.prototype.add = originalAdd;
            }
        }
    },
    {
        name: "plain objects work as Set-like arguments",
        body() {
            var s1 = new Set([1, 2]);
            var like = {
                size: 2,
                has: function (v) { return v === 2; },
                keys: function () { return [2, 3].values(); }
            };
            assert.areEqual([1, 2, 3], [...s1.union(like)]);
            assert.areEqual([2], [...s1.intersection(like)]);
            assert.areEqual([1], [...s1.difference(like)]);
            assert.areEqual([1, 3], [...s1.symmetricDifference(like)]);
            assert.isFalse(s1.isSubsetOf(like));
            assert.isFalse(s1.isSupersetOf(like));
            assert.isFalse(s1.isDisjointFrom(like));
            assert.isTrue(new Set([2]).isSubsetOf(like));
        }
    },
    {
        name: "Maps and array-like records work as Set-like arguments",
        body() {
            var s1 = new Set([1, 2]);
            var map = new Map([[2, "two"], [3, "three"]]);
            assert.areEqual([1, 2, 3], [...s1.union(map)]);
            assert.areEqual([2], [...s1.intersection(map)]);
            assert.areEqual([1], [...s1.difference(map)]);
            assert.areEqual([1, 3], [...s1.symmetricDifference(map)]);
            assert.isFalse(s1.isSubsetOf(map));
            assert.isFalse(s1.isSupersetOf(map));
            assert.isFalse(s1.isDisjointFrom(map));

            // an Array with size/has/keys installed is consumed as a record
            var arrayLike = [5, 6];
            arrayLike.size = 2;
            arrayLike.has = function () { throw new Error("has must not be called by union"); };
            arrayLike.keys = function () { return [2, 3].values(); };
            assert.areEqual([1, 2, 3], [...s1.union(arrayLike)]);

            // a bare Array is not a record: Get("size") is its length, and has is not callable
            assert.throws(function () { s1.union([3]); }, TypeError);
        }
    },
    {
        name: "set-like protocol validation order and error types",
        body() {
            var order = [];
            var recorder = makeRecorder(order, [], function () { return false; });
            new Set([1]).union(recorder);
            assert.areEqual(
                ["get size", "toNumber size", "get has", "get keys", "call keys"],
                order,
                "size, its ToNumber, has, and keys are observed in order before iteration");

            for (var i = 0; i < setMethods.length; i++) {
                var name = setMethods[i];
                var nonObject = ["number", "string", "boolean", "undefined", "null", "symbol"];
                for (var j = 0; j < nonObject.length; j++) {
                    var value;
                    switch (nonObject[j]) {
                        case "undefined": value = undefined; break;
                        case "null": value = null; break;
                        case "symbol": value = Symbol("s"); break;
                        default: value = nonObject[j] === "number" ? 1 : nonObject[j] === "string" ? "" : false;
                    }
                    assert.throws(function () { Set.prototype[name].call(new Set([1]), value); }, TypeError, name + " rejects " + nonObject[j]);
                }
                // missing other is a TypeError too
                assert.throws(function () { Set.prototype[name].call(new Set([1])); }, TypeError, name + " rejects a missing argument");

                var steps = [
                    { patch: function (o) { o.size = NaN; }, label: "NaN size" },
                    { patch: function (o) { o.size = { valueOf: function () { return NaN; } }; }, label: "NaN-coercing size" },
                    { patch: function (o) { o.size = "not a number"; }, label: "non-numeric string size" },
                    { patch: function (o) { o.size = -1; }, label: "negative size" },
                    { patch: function (o) { o.has = {}; }, label: "non-callable has" },
                    { patch: function (o) { o.keys = undefined; }, label: "undefined keys" }
                ];
                for (var k = 0; k < steps.length; k++) {
                    var base = { size: 1, has: function () { return false; }, keys: function () { return [].values(); } };
                    steps[k].patch(base);
                    var expectedType = (steps[k].label === "negative size") ? RangeError : TypeError;
                    assert.throws(function (m, b) { return function () { Set.prototype[m].call(new Set([1]), b); }; }(name, base), expectedType, name + " rejects " + steps[k].label);
                }
            }
        }
    },
    {
        name: "non-Set receivers throw TypeError",
        body() {
            var realSet = new Set([1]);
            var receivers = [undefined, null, true, 1, "", Symbol(), {}, [], new Map()];
            for (var i = 0; i < setMethods.length; i++) {
                var name = setMethods[i];
                for (var j = 0; j < receivers.length; j++) {
                    assert.throws(function (m, r) { return function () { Set.prototype[m].call(r, realSet); }; }(name, receivers[j]), TypeError, name + " rejects " + String(receivers[j]));
                }
                // a Set-like receiver is still not a Set
                var setLike = { size: 1, has: function () { return false; }, keys: function () { return [].values(); } };
                assert.throws(function () { Set.prototype[name].call(setLike, realSet); }, TypeError, name + " rejects a Set-like receiver");
                assert.throws(function () { Set.prototype[name].call(Set.prototype, realSet); }, TypeError, name + " rejects Set.prototype");
            }
        }
    },
    {
        name: "size chooses the has walk or the keys walk per method",
        body() {
            // difference with this.size <= other.size walks this and calls other.has once per element
            var order = [];
            var recorder = makeRecorder(order, ["x", "y"], function () { return false; });
            assert.areEqual(["a", "d"], [...new Set(["a", "d"]).difference(recorder)], "smaller receiver calls has twice");
            assert.areEqual(
                ["get size", "toNumber size", "get has", "get keys", "call has a", "call has d"],
                order,
                "keys is read but never called while has is walked");

            // same size still walks this (size <= otherSize), three has calls
            order = [];
            recorder = makeRecorder(order, ["x", "y", "z"], function () { return false; });
            assert.areEqual([], [...new Set(["a", "b", "d"]).intersection(recorder)], "has always false empties the intersection");
            assert.areEqual(3, order.filter(function (e) { return e.indexOf("call has ") === 0; }).length, "three has calls at equal size");

            // this.size > other.size iterates other's keys and never calls has
            order = [];
            recorder = makeRecorder(order, ["x"], function () { throw new Error("has must not be called"); });
            assert.areEqual(["a", "d", "e", "x"], [...new Set(["a", "d", "e"]).union(recorder)]);
            assert.areEqual(["d", "e"], [...new Set(["a", "b", "d", "e"]).difference({ size: 2, get has() { return function () { throw new Error("has must not be called"); }; }, keys() { return ["a", "b"].values(); } })]);

            // isSubsetOf of a larger receiver is false without any has call
            order = [];
            recorder = makeRecorder(order, [], function () { throw new Error("has must not be called"); });
            assert.isFalse(new Set(["a", "b", "c", "d"]).isSubsetOf(recorder));
            assert.areEqual(["get size", "toNumber size", "get has", "get keys"], order);

            // isSupersetOf of a smaller receiver is false without any iteration:
            // size, has, and keys are read from the record but never called
            order = [];
            var noIteration = {
                get size() { order.push("get size"); return 2; },
                get has() { order.push("get has"); return function () { throw new Error("has must not be called"); }; },
                get keys() { order.push("get keys"); return function () { throw new Error("keys must not be called"); }; }
            };
            assert.isFalse(new Set(["a"]).isSupersetOf(noIteration));
            assert.areEqual(["get size", "get has", "get keys"], order);

            // symmetricDifference removes shared values from the copy and appends
            // the rest, in iteration order
            assert.areEqual(["b", "d", "c"], [...new Set(["a", "b", "d"]).symmetricDifference({ size: 2, get has() { return function () { throw new Error("has must not be called"); }; }, keys() { return ["a", "c"].values(); } })]);

            // union and symmetricDifference always iterate keys and never call has
            order = [];
            recorder = makeRecorder(order, ["b", "c"], function () { throw new Error("has must not be called"); });
            assert.areEqual(["a", "d", "b", "c"], [...new Set(["a", "d"]).union(recorder)]);
            assert.areEqual(["a", "d", "e", "f", "b", "c"], [...new Set(["a", "d", "e", "f"]).union(recorder)]);
            assert.areEqual(["a", "d", "b", "c"], [...new Set(["a", "d"]).symmetricDifference(recorder)]);
        }
    },
    {
        name: "isSupersetOf closes the keys iterator only on early exit",
        body() {
            var nextCalls = 0;
            var returnCalls = 0;
            var values = [4, 5, 6];
            var iter = {
                next() {
                    var done = nextCalls >= values.length;
                    var value = values[nextCalls];
                    nextCalls++;
                    return { done: done, value: value };
                },
                return() {
                    returnCalls++;
                    return {};
                }
            };
            var setLike = { size: 3, has: function () { return true; }, keys: function () { return iter; } };

            assert.isTrue(new Set([4, 5, 6, 7]).isSupersetOf(setLike));
            assert.areEqual(4, nextCalls, "exhausted iterator is stepped to done");
            assert.areEqual(0, returnCalls, "no close after natural exhaustion");

            nextCalls = 0;
            assert.isFalse(new Set([0, 1, 2, 3]).isSupersetOf(setLike));
            assert.areEqual(1, nextCalls, "early exit stops at the first miss");
            assert.areEqual(1, returnCalls, "early exit closes the iterator");

            // isDisjointFrom takes the keys walk when this is larger, and closes on a hit
            nextCalls = 0;
            returnCalls = 0;
            assert.isFalse(new Set([0, 1, 2, 3, 4]).isDisjointFrom(setLike));
            assert.areEqual(1, nextCalls, "disjoint early exit stops at the first hit");
            assert.areEqual(1, returnCalls, "disjoint early exit closes the iterator");
        }
    },
    {
        name: "early exit closes the keys iterator with error propagation",
        body() {
            // The close itself follows spec IteratorClose on a normal
            // completion: a throwing return(), a non-object return() result,
            // and a throwing "return" getter all propagate instead of being
            // swallowed, and the method does not return false in those cases.
            function makeKeysLike(iter) {
                return { size: 3, has: function () { throw new Error("has must not be called"); }, keys: function () { return iter; } };
            }
            function freshIter() {
                return { next: function () { return { done: false, value: 4 }; } };
            }

            // return() throws: the same error propagates from both methods
            var iter = freshIter();
            iter.return = function () { throw new Error("boom-return"); };
            var like = makeKeysLike(iter);
            assert.throws(function () { new Set([0, 1, 2, 3]).isSupersetOf(like); }, Error, "isSupersetOf propagates a throwing return()", "boom-return");
            assert.throws(function () { new Set([0, 1, 2, 3, 4]).isDisjointFrom(like); }, Error, "isDisjointFrom propagates a throwing return()", "boom-return");

            // return() result is not an Object: TypeError
            iter = freshIter();
            iter.return = function () { return 42; };
            like = makeKeysLike(iter);
            assert.throws(function () { new Set([0, 1, 2, 3]).isSupersetOf(like); }, TypeError, "isSupersetOf rejects a non-object return() result");
            assert.throws(function () { new Set([0, 1, 2, 3, 4]).isDisjointFrom(like); }, TypeError, "isDisjointFrom rejects a non-object return() result");

            // the "return" getter itself throws: the getter's error propagates
            iter = freshIter();
            Object.defineProperty(iter, "return", { get: function () { throw new Error("boom-getter"); } });
            like = makeKeysLike(iter);
            assert.throws(function () { new Set([0, 1, 2, 3]).isSupersetOf(like); }, Error, "isSupersetOf propagates a throwing return getter", "boom-getter");
            assert.throws(function () { new Set([0, 1, 2, 3, 4]).isDisjointFrom(like); }, Error, "isDisjointFrom propagates a throwing return getter", "boom-getter");

            // "return" is present but not callable: TypeError
            iter = freshIter();
            iter.return = 42;
            like = makeKeysLike(iter);
            assert.throws(function () { new Set([0, 1, 2, 3]).isSupersetOf(like); }, TypeError, "isSupersetOf rejects a non-callable return");
            assert.throws(function () { new Set([0, 1, 2, 3, 4]).isDisjointFrom(like); }, TypeError, "isDisjointFrom rejects a non-callable return");

            // no "return" at all: the early false is still returned normally
            iter = freshIter();
            like = makeKeysLike(iter);
            assert.isFalse(new Set([0, 1, 2, 3]).isSupersetOf(like), "missing return leaves the early false in place");
            assert.isFalse(new Set([0, 1, 2, 3, 4]).isDisjointFrom(like), "missing return leaves the early false in place");
        }
    },
    {
        name: "mutation during iteration is resolved against snapshots and live data per method",
        body() {
            // union snapshots this after GetSetRecord (q lands in the copy) and
            // ignores deletions that happen once the iterator runs
            var baseSet = new Set(["a", "b", "c", "d", "e"]);
            var mutatingIterator = {
                index: 0,
                values: ["x", "y"],
                next() {
                    baseSet.delete("b");
                    baseSet.delete("c");
                    baseSet.add("b");
                    baseSet.add("d");
                    return { done: this.index >= this.values.length, value: this.values[this.index++] };
                }
            };
            var evil = {
                size: 2,
                get has() {
                    baseSet.add("q");
                    return function () { throw new Error("union must not call has"); };
                },
                keys() { return mutatingIterator; }
            };
            assert.areEqual(["a", "b", "c", "d", "e", "q", "x", "y"], [...baseSet.union(evil)], "the copy predates the iteration");
            assert.areEqual(["a", "d", "e", "q", "b"], [...baseSet], "the live receiver was still mutated");

            // difference (this larger): b is removed from the copy even though it was
            // re-added to the live set; c keeps its slot because the walk removes from
            // the copy, keyed by the record's values
            baseSet = new Set(["a", "b", "c", "d", "e"]);
            mutatingIterator = {
                index: 0,
                values: ["x", "b", "b"],
                next() {
                    if (this.index === 0) {
                        baseSet.delete("b");
                        baseSet.delete("c");
                        baseSet.add("b");
                        baseSet.add("d");
                    }
                    return { done: this.index >= this.values.length, value: this.values[this.index++] };
                }
            };
            evil = {
                size: 3,
                get has() {
                    baseSet.add("q");
                    return function () { throw new Error("difference must not call has when this is larger"); };
                },
                keys() { return mutatingIterator; }
            };
            assert.areEqual(["a", "c", "d", "e", "q"], [...baseSet.difference(evil)]);

            // intersection (this larger): membership is checked against the live
            // receiver, so b survives its delete+re-add and x never joins
            baseSet = new Set(["a", "b", "c", "d", "e"]);
            mutatingIterator = {
                index: 0,
                values: ["x", "b", "b"],
                next() {
                    if (this.index === 0) {
                        baseSet.delete("b");
                        baseSet.delete("c");
                        baseSet.add("b");
                        baseSet.add("d");
                    }
                    return { done: this.index >= this.values.length, value: this.values[this.index++] };
                }
            };
            evil = {
                size: 3,
                get has() {
                    baseSet.add("q");
                    return function () { throw new Error("intersection must not call has when this is larger"); };
                },
                keys() { return mutatingIterator; }
            };
            assert.areEqual(["b"], [...baseSet.intersection(evil)], "duplicate b values collapse into one entry");

            // symmetricDifference: a value deleted from the live receiver is appended
            // only when absent from the copy, so c keeps its original slot
            baseSet = new Set(["a", "b", "c", "d", "e"]);
            mutatingIterator = {
                index: 0,
                values: ["x", "b", "c", "c"],
                next() {
                    if (this.index === 0) {
                        baseSet.delete("b");
                        baseSet.delete("c");
                        baseSet.add("b");
                        baseSet.add("d");
                    }
                    return { done: this.index >= this.values.length, value: this.values[this.index++] };
                }
            };
            evil = {
                size: 4,
                get has() {
                    baseSet.add("q");
                    return function () { throw new Error("symmetricDifference must not call has"); };
                },
                keys() { return mutatingIterator; }
            };
            assert.areEqual(["a", "c", "d", "e", "q", "x"], [...baseSet.symmetricDifference(evil)]);
        }
    },
    {
        name: "has-walk mutations skip deleted entries and visit re-added ones",
        body() {
            var live = new Set(["a", "b", "c"]);
            var seen = [];
            var evil = {
                size: 3,
                has(v) {
                    seen.push(v);
                    if (v === "a") {
                        live.delete("b");
                        live.delete("c");
                        live.add("b");
                    }
                    return v !== "z";
                },
                keys() { throw new Error("keys must not be called"); }
            };
            assert.isTrue(live.isSubsetOf(evil));
            assert.areEqual(["a", "b"], seen, "c is deleted before it is visited and b is re-added in time");
            assert.areEqual(["a", "b"], [...live]);

            // difference walks its own copy, so a value has() deletes from the
            // receiver is still passed to has
            var diffSeen = [];
            var diffLike = {
                size: 2,
                has(v) {
                    diffSeen.push(v);
                    return v === "a";
                },
                keys() { throw new Error("keys must not be called"); }
            };
            var diffSet = new Set(["a", "b"]);
            assert.areEqual(["b"], [...diffSet.difference(diffLike)]);
            assert.areEqual(["a", "b"], diffSeen);
        }
    },
    {
        name: "values from the record's iterator are canonicalized from -0 to +0",
        body() {
            var minusZeroKeys = {
                size: 1,
                has() { throw new Error("has must not be called"); },
                keys() { return [-0].values(); }
            };
            var u1 = new Set([1]).union(minusZeroKeys);
            assert.areEqual(2, u1.size);
            var items = [...u1];
            assert.areEqual(1, items[0]);
            assert.isTrue(Object.is(0, items[1]), "-0 becomes +0 in the result");

            var u2 = new Set([+0]).union(minusZeroKeys);
            assert.areEqual(1, u2.size, "+0 already present is not added again");

            assert.areEqual([+0], [...new Set([1, -0]).symmetricDifference(new Set([1]))], "receiver data is canonical already");
        }
    },
    {
        name: "duplicated and self-referential records",
        body() {
            var s = new Set([1, 2]);
            // the record may yield the same value twice
            var dup = {
                size: 2,
                has() { return true; },
                keys() { return [2, 2].values(); }
            };
            assert.areEqual([1, 2], [...s.union(dup)], "duplicates collapse in the union copy");
            assert.areEqual([1, 2], [...s.intersection(dup)], "the has walk visits each entry once and keeps both");
            assert.areEqual([], [...s.difference(dup)], "has true for everything removes everything");
            assert.areEqual([1], [...s.symmetricDifference(dup)], "a value both hold is removed once");

            // receiver and argument are the same object
            assert.areEqual([1, 2], [...s.union(s)]);
            assert.areEqual([1, 2], [...s.intersection(s)]);
            assert.areEqual([], [...s.difference(s)]);
            assert.areEqual([], [...s.symmetricDifference(s)]);
            assert.isTrue(s.isSubsetOf(s));
            assert.isTrue(s.isSupersetOf(s));
            assert.isFalse(s.isDisjointFrom(s));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
