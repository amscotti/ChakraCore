//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "replaceAll substitutes every non-overlapping match",
        body() {
            assert.areEqual("xbx", "aba".replaceAll("a", "x"));
            assert.areEqual("aba", "aba".replaceAll("z", "x"));
            assert.areEqual("ba", "aaa".replaceAll("aa", "b"));
        }
    },
    {
        name: "replaceAll empty search matches every boundary",
        body() {
            assert.areEqual("xaxbx", "ab".replaceAll("", "x"));
            assert.areEqual("x", "".replaceAll("", "x"));
        }
    },
    {
        name: "replaceAll functional replacer receives match, offset, and string",
        body() {
            var calls = [];
            var bag = "";
            var result = "ab".replaceAll("", function (match, offset, string) {
                calls.push({ match: match, offset: offset, string: string, thiz: this });
                bag += match;
                return "x";
            });
            assert.areEqual("xaxbx", result);
            assert.areEqual(3, calls.length);
            assert.areEqual("", bag);
            assert.areEqual("", calls[0].match);
            assert.areEqual(0, calls[0].offset);
            assert.areEqual("ab", calls[0].string);
            // Non-strict [[Call]] boxes an undefined this to the global object,
            // the same as String.prototype.replace.
            assert.areEqual(this, calls[0].thiz);
            assert.areEqual("", calls[1].match);
            assert.areEqual(1, calls[1].offset);
            assert.areEqual("ab", calls[1].string);
            assert.areEqual(this, calls[1].thiz);
            assert.areEqual("", calls[2].match);
            assert.areEqual(2, calls[2].offset);
            assert.areEqual("ab", calls[2].string);
            assert.areEqual(this, calls[2].thiz);

            var collected = "";
            var count = 0;
            var replaced = "aba".replaceAll("a", function (match) {
                count++;
                collected += match;
                return "x";
            });
            assert.areEqual("xbx", replaced);
            assert.areEqual(2, count);
            assert.areEqual("aa", collected);
        }
    },
    {
        name: "replaceAll substitution patterns",
        body() {
            assert.areEqual("$b", "ab".replaceAll("a", "$$"));
            assert.areEqual("a.a", "a.a".replaceAll("a", "$&"));
            assert.areEqual("aa", "ab".replaceAll("b", "$`"));
            assert.areEqual("bb", "ab".replaceAll("a", "$'"));
            assert.areEqual("$11", "a1".replaceAll("a", "$1"));
        }
    },
    {
        name: "replaceAll this coercion and missing replacement",
        body() {
            assert.throws(function () { String.prototype.replaceAll.call(null, "a", "b"); }, TypeError);
            assert.throws(function () { String.prototype.replaceAll.call(undefined, "a", "b"); }, TypeError);
            assert.areEqual("xx", String.prototype.replaceAll.call(11, "1", "x"));
            assert.areEqual("undefined", "a".replaceAll("a"));
        }
    },
    {
        name: "replaceAll requires the global flag on real regexps",
        body() {
            assert.throws(function () { "a".replaceAll(/a/, "x"); }, TypeError);
            assert.throws(function () { "a".replaceAll(new RegExp("a"), "x"); }, TypeError);
            assert.areEqual("xbxcx", "abaca".replaceAll(/a/g, "x"));
        }
    },
    {
        name: "replaceAll ToStrings a non-callable replacement when nothing matches",
        body() {
            var n = 0;
            var result = "abc".replaceAll("z", { toString: function () { n++; return "Q"; } });
            assert.areEqual("abc", result);
            assert.areEqual(1, n);
        }
    },
    {
        name: "replaceAll throws when the replacement is a symbol and nothing matches",
        body() {
            assert.throws(function () { "abc".replaceAll("z", Symbol("s")); }, TypeError);
        }
    },
    {
        name: "replaceAll throws when a real regexp has an own flags value of undefined",
        body() {
            var re = /a/g;
            Object.defineProperty(re, "flags", { value: undefined });
            assert.throws(function () { "a".replaceAll(re, "x"); }, TypeError);
        }
    },
    {
        name: "replaceAll observes an inherited flags getter instead of the internal global bit",
        body() {
            var calls = 0;
            try {
                Object.defineProperty(RegExp.prototype, "flags", {
                    configurable: true,
                    get: function () {
                        calls++;
                        return "y";
                    }
                });
                assert.throws(function () { "abaca".replaceAll(/a/g, "x"); }, TypeError);
                assert.areEqual(1, calls);
            } finally {
                delete RegExp.prototype.flags;
            }
        }
    },
    {
        name: "replaceAll throws on a non-global regexp-like before reading @@replace",
        body() {
            if (typeof Symbol.match === "undefined" || typeof Symbol.replace === "undefined") {
                return;
            }
            var called = false;
            var search = {};
            search[Symbol.match] = true;
            search.flags = "y";
            search[Symbol.replace] = function () {
                called = true;
                return "nope";
            };
            assert.throws(function () { "abc".replaceAll(search, "x"); }, TypeError);
            assert.areEqual(false, called);
        }
    },
    {
        name: "replaceAll delegates a global regexp-like to @@replace",
        body() {
            if (typeof Symbol.match === "undefined" || typeof Symbol.replace === "undefined") {
                return;
            }
            var called = 0;
            var search = {};
            search[Symbol.match] = true;
            search.flags = "g";
            search[Symbol.replace] = function (string, replaceValue) {
                called++;
                return "ok:" + string + ":" + replaceValue;
            };
            var result = "abc".replaceAll(search, "x");
            assert.areEqual(1, called);
            assert.areEqual("ok:abc:x", result);
        }
    },
    {
        name: "replaceAll reads flags once and does not touch @@replace when not global",
        body() {
            if (typeof Symbol.match === "undefined" || typeof Symbol.replace === "undefined") {
                return;
            }
            var flagsCount = 0;
            var replaceCount = 0;
            var search = {};
            search[Symbol.match] = true;
            Object.defineProperty(search, "flags", {
                get: function () {
                    flagsCount++;
                    return "y";
                },
                configurable: true
            });
            Object.defineProperty(search, Symbol.replace, {
                get: function () {
                    replaceCount++;
                    return function () { return "no"; };
                },
                configurable: true
            });
            assert.throws(function () { "abc".replaceAll(search, "z"); }, TypeError);
            assert.areEqual(1, flagsCount);
            assert.areEqual(0, replaceCount);
        }
    },
    {
        name: "replaceAll releases the match list when a replacer throws",
        body() {
            var calls = 0;
            assert.throws(function () {
                "abab".replaceAll("a", function () {
                    calls++;
                    throw new Error("stop");
                });
            }, Error);
            assert.areEqual(1, calls);
            assert.areEqual("XbXb", "abab".replaceAll("a", "X"));
        }
    },
    {
        name: "replaceAll survives a throw from a nested replaceAll",
        body() {
            var inner = 0;
            assert.throws(function () {
                "ab".replaceAll("a", function () {
                    return "xy".replaceAll("y", function () {
                        inner++;
                        throw new Error("inner");
                    });
                });
            }, Error);
            assert.areEqual(1, inner);
            assert.areEqual("XbX", "aba".replaceAll("a", "X"));
        }
    },
    {
        name: "replaceAll allows a nested replaceAll from the replacer",
        body() {
            function nest(depth) {
                if (depth === 0) {
                    return "Z";
                }
                return "a".replaceAll("a", function () { return nest(depth - 1); });
            }
            assert.areEqual("Z", nest(8));
            assert.areEqual("QbQ", "aba".replaceAll("a", "Q"));
        }
    },
    {
        name: "replaceAll completes a long input after a throwing large replace",
        body() {
            var input = "";
            var i;
            for (i = 0; i < 4000; i++) {
                input += "a";
            }
            var calls = 0;
            assert.throws(function () {
                input.replaceAll("a", function () {
                    calls++;
                    if (calls === 50) {
                        throw new Error("stop");
                    }
                    return "b";
                });
            }, Error);
            assert.areEqual(50, calls);
            var out = input.replaceAll("a", "b");
            assert.areEqual(4000, out.length);
            assert.areEqual("b", out.charAt(0));
            assert.areEqual("b", out.charAt(3999));
            assert.areEqual("xaxbx", "ab".replaceAll("", "x"));
        }
    },
    {
        name: "String.prototype.replaceAll descriptor",
        body() {
            var desc = Object.getOwnPropertyDescriptor(String.prototype, "replaceAll");
            assert.areEqual(2, desc.value.length);
            assert.areEqual("replaceAll", desc.value.name);
            assert.areEqual(true, desc.writable);
            assert.areEqual(false, desc.enumerable);
            assert.areEqual(true, desc.configurable);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
