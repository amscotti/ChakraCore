//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "replaceAll with a null @@replace searches the regexp string",
        body() {
            var re = /a/g;
            re[Symbol.replace] = null;
            assert.areEqual("xxQyy", "xx/a/gyy".replaceAll(re, "Q"));
        }
    },
    {
        name: "replaceAll with an undefined @@replace does not regexp-replace",
        body() {
            var re = /a/g;
            re[Symbol.replace] = undefined;
            assert.areEqual("abaca", "abaca".replaceAll(re, "x"));
        }
    },
    {
        name: "replaceAll calls an own @@replace once and returns its value",
        body() {
            var re = /a/g;
            var called = 0;
            re[Symbol.replace] = function () {
                called++;
                return "Z";
            };
            assert.areEqual("Z", "abaca".replaceAll(re, "x"));
            assert.areEqual(1, called);
        }
    },
    {
        name: "replaceAll still requires the global flag",
        body() {
            assert.throws(function () { "a".replaceAll(/a/, "x"); }, TypeError);
        }
    },
    {
        name: "replaceAll uses the builtin @@replace for a global regexp",
        body() {
            assert.areEqual("xbxcx", "abaca".replaceAll(/a/g, "x"));
        }
    },
    {
        name: "replaceAll calls @@replace on a plain object",
        body() {
            var obj = {};
            obj[Symbol.replace] = function () { return "Z"; };
            assert.areEqual("Z", "abc".replaceAll(obj, "x"));
        }
    },
    {
        name: "replaceAll throws when a plain object's @@replace is not callable",
        body() {
            var obj = {};
            obj[Symbol.replace] = 1;
            assert.throws(function () { "abc".replaceAll(obj, "x"); }, TypeError);
        }
    },
    {
        name: "replaceAll with Symbol.match false uses builtin @@replace",
        body() {
            var re = /a/g;
            re[Symbol.match] = false;
            assert.areEqual("xx/Q/gyy", "xx/a/gyy".replaceAll(re, "Q"));
        }
    },
    {
        name: "replaceAll with Symbol.match false does not require the global flag",
        body() {
            var re = /a/;
            var calls = 0;
            re[Symbol.match] = false;
            Object.defineProperty(re, Symbol.replace, {
                configurable: true,
                get: function () {
                    calls++;
                    return function () { return "NOFLAGS"; };
                }
            });
            assert.areEqual("NOFLAGS", "abc".replaceAll(re, "x"));
            assert.areEqual(1, calls);
        }
    },
    {
        name: "replaceAll does not call getOwnPropertyDescriptor on a regexp-like proxy",
        body() {
            if (typeof Proxy !== "function") {
                return;
            }
            var target = {};
            target[Symbol.match] = true;
            target.flags = "g";
            target[Symbol.replace] = function () { return "Z"; };
            var proxy = new Proxy(target, {
                getOwnPropertyDescriptor: function () {
                    throw new TypeError("gopd");
                }
            });
            assert.areEqual("Z", "abc".replaceAll(proxy, "x"));
        }
    },
    {
        name: "replaceAll passes an omitted replace value to @@replace",
        body() {
            var re = /a/g;
            re[Symbol.replace] = function () {
                return arguments.length + ":" + (1 in arguments);
            };
            assert.areEqual("2:true", "abc".replaceAll(re));
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
        name: "replaceAll does not read @@replace on a string, number, or boolean primitive",
        body() {
            var hits = 0;
            function poison(proto) {
                Object.defineProperty(proto, Symbol.replace, {
                    configurable: true,
                    get: function () {
                        hits++;
                        throw new Error("primitive @@replace");
                    }
                });
            }
            poison(String.prototype);
            poison(Number.prototype);
            poison(Boolean.prototype);
            poison(Symbol.prototype);
            try {
                assert.areEqual("aXbXc", "a,b,c".replaceAll(",", "X"));
                assert.areEqual("aXbXc", "a1b1c".replaceAll(1, "X"));
                assert.areEqual("aXbXc", "atruebtruec".replaceAll(true, "X"));
                assert.throws(function () { "a".replaceAll(Symbol("s"), "X"); }, TypeError);
                assert.areEqual(0, hits);
            } finally {
                delete String.prototype[Symbol.replace];
                delete Number.prototype[Symbol.replace];
                delete Boolean.prototype[Symbol.replace];
                delete Symbol.prototype[Symbol.replace];
            }
        }
    },
    {
        name: "replaceAll reads @@replace on a boxed string",
        body() {
            var search = new String(",");
            var called = 0;
            search[Symbol.replace] = function () {
                called++;
                return "Z";
            };
            assert.areEqual("Z", "a,b".replaceAll(search, "x"));
            assert.areEqual(1, called);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
