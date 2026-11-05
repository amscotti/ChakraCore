//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT License. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// E11: ES2025 early length-limit validation for Array.prototype.push/splice/unshift on
// generic receivers. Each must throw a TypeError before moving or storing any element when
// the resulting length would exceed 2^53-1 (22.1.3.22 step 5, 22.1.3.31 step 8, 22.1.3.35
// step 4.a), instead of attempting O(2^53) work (splice/unshift) or silently storing one
// element past the limit (push). Coercion order is observable: 'length' (and for splice,
// start/deleteCount) is read before the throw. See doc/e11-tco-timeouts.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var MAX_SAFE = 9007199254740991; // 2^53 - 1

function arrayLikeAt(length) {
    return { length: length };
}

var tests = [
    {
        name: "push/splice/unshift throw TypeError when the new length exceeds 2^53-1",
        body: function () {
            var lengths = [Math.pow(2, 53) - 1, Math.pow(2, 53), Math.pow(2, 53) + 2, Infinity];
            var i;
            for (i = 0; i < lengths.length; i++) {
                assert.throws(function () { Array.prototype.push.call(arrayLikeAt(lengths[i]), null); },
                    TypeError, "push should throw at length " + lengths[i]);
                assert.throws(function () { Array.prototype.splice.call(arrayLikeAt(lengths[i]), 0, 0, null); },
                    TypeError, "splice should throw at length " + lengths[i]);
                assert.throws(function () { Array.prototype.unshift.call(arrayLikeAt(lengths[i]), null); },
                    TypeError, "unshift should throw at length " + lengths[i]);
            }
        }
    },
    {
        name: "push at the boundary stores the element and reports the capped length",
        body: function () {
            var justBelow = arrayLikeAt(MAX_SAFE - 1);
            assert.areEqual(MAX_SAFE, Array.prototype.push.call(justBelow, "x"),
                "push of one item at length 2^53-2 succeeds and returns 2^53-1");
            assert.areEqual("x", justBelow[MAX_SAFE - 1], "the pushed item lands at index 2^53-2");
            assert.areEqual(MAX_SAFE, justBelow.length, "length is capped at 2^53-1");

            var twoBelow = arrayLikeAt(MAX_SAFE - 2);
            assert.areEqual(MAX_SAFE, Array.prototype.push.call(twoBelow, 1, 2),
                "pushing two items at length 2^53-2 still fits");
            assert.throws(function () { Array.prototype.push.call(arrayLikeAt(MAX_SAFE - 2), 1, 2, 3); },
                TypeError, "pushing three items at length 2^53-2 exceeds 2^53-1");
        }
    },
    {
        name: "the limit check runs after 'length' is read and before anything is stored",
        body: function () {
            var reads;
            var getter = { get length() { reads.push("length"); return MAX_SAFE; } };

            reads = [];
            assert.throws(function () { Array.prototype.push.call(getter, 1); }, TypeError, "push throws");
            assert.areEqual("length", reads.join(","), "push reads length before throwing");

            reads = [];
            assert.throws(function () { Array.prototype.unshift.call(getter, 1); }, TypeError, "unshift throws");
            assert.areEqual("length", reads.join(","), "unshift reads length before throwing");
        }
    },
    {
        name: "splice reads start and deleteCount before the limit throw",
        body: function () {
            var order = [];
            var obj = { get length() { order.push("length"); return MAX_SAFE; } };
            var start = { valueOf: function () { order.push("start"); return 0; } };
            var deleteCount = { valueOf: function () { order.push("deleteCount"); return 0; } };
            assert.throws(function () { Array.prototype.splice.call(obj, start, deleteCount, null); },
                TypeError, "splice throws after coercing its arguments");
            assert.areEqual("length,start,deleteCount", order.join(","),
                "splice coercion order is length, start, deleteCount, then the limit check");
        }
    },
    {
        name: "unshift with no items and splice that only deletes never hit the limit",
        body: function () {
            var atCap = arrayLikeAt(MAX_SAFE);
            assert.areEqual(MAX_SAFE, Array.prototype.unshift.call(atCap),
                "unshift with no items only reports the length");
            assert.areEqual(MAX_SAFE, Array.prototype.push.call(atCap),
                "push with no items only reports the length");
        }
    },
    {
        name: "generic and real-array push/splice/unshift behavior is otherwise unchanged",
        body: function () {
            var generic = { 0: "A", 1: "B", length: 2 };
            assert.areEqual(3, Array.prototype.unshift.call(generic, "z"), "generic unshift returns the new length");
            assert.areEqual("z,A,B", [generic[0], generic[1], generic[2]].join(","), "generic unshift shifts up");
            assert.areEqual(3, generic.length, "generic unshift updates length");

            var spliced = { 0: "A", 1: "B", 2: "C", 3: "D", length: 4 };
            var removed = Array.prototype.splice.call(spliced, 1, 2, "x", "y", "z");
            assert.areEqual("B,C", removed.join(","), "generic splice returns deleted elements");
            assert.areEqual("A,x,y,z,D", [spliced[0], spliced[1], spliced[2], spliced[3], spliced[4]].join(","),
                "generic splice renumbers around the insertion");
            assert.areEqual(5, spliced.length, "generic splice updates length");

            var arr = [];
            assert.areEqual(1, arr.push("a"), "real-array push still works");
            assert.areEqual(2, arr.unshift("b"), "real-array unshift still works");
            assert.areEqual("b,a", arr.join(","), "real-array push/unshift store in order");
            assert.areEqual("a", arr.splice(1, 1).join(","), "real-array splice still deletes");
            assert.areEqual("b", arr.join(","), "real-array splice result");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
