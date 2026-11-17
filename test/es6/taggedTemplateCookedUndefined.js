//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Tagged templates with invalid escapes keep cooked=undefined (ES2018+), and
// strict-mode \8\9 string escapes are early SyntaxErrors (Annex B is [~Strict] only).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function tagIdentity(callsite) {
    return callsite;
}

var tests = [
    {
        name: "tagged template with invalid unicode escape: cooked undefined, raw preserved",
        body: function () {
            var r = tagIdentity`\unicode`;
            assert.isTrue(r[0] === undefined, "cooked element is undefined");
            assert.areEqual("\\unicode", r.raw[0], "raw element preserves source text");
            assert.isTrue(r.hasOwnProperty("raw"), "callsite has raw");
        }
    },
    {
        name: "tagged template invalid escapes in multiple pieces with substitution",
        body: function () {
            var r = tagIdentity`a\1${42}b\2`;
            assert.isTrue(r[0] === undefined, "first cooked element is undefined");
            assert.isTrue(r[1] === undefined, "second cooked element is undefined");
            assert.areEqual("a\\1", r.raw[0], "first raw piece preserved");
            assert.areEqual("b\\2", r.raw[1], "second raw piece preserved");
        }
    },
    {
        name: "String.raw with invalid escape returns the raw text",
        body: function () {
            assert.areEqual("\\unicode", String.raw`\unicode`, "String.raw of invalid escape");
            assert.areEqual("a\\142b", String.raw`a\1${42}b`, "String.raw stitches raw pieces");
        }
    },
    {
        name: "untagged templates with invalid escapes still reject",
        body: function () {
            assert.throws(function () { eval("`\\1`"); }, SyntaxError, "untagged \\1 rejects");
            assert.throws(function () { eval("`\\8`"); }, SyntaxError, "untagged \\8 rejects");
            assert.throws(function () { eval("`\\unicode`"); }, SyntaxError, "untagged \\unicode rejects");
            assert.throws(function () { eval("`a\\1${42}b`"); }, SyntaxError, "untagged with substitution rejects");
            assert.throws(function () { eval("\"use strict\"; `\\1`;"); }, SyntaxError, "untagged strict \\1 rejects");
        }
    },
    {
        name: "strict-mode \\8 and \\9 string escapes reject",
        body: function () {
            assert.throws(function () { eval("\"use strict\"; var s=\"\\8\";"); }, SyntaxError, "strict \\8 rejects");
            assert.throws(function () { eval("\"use strict\"; var s=\"\\9\";"); }, SyntaxError, "strict \\9 rejects");
            assert.throws(function () { eval("\"\\8\"; \"use strict\";"); }, SyntaxError, "prologue \\8 then use-strict rejects");
            assert.throws(function () { eval("class A { \"\\8\"() {} }"); }, SyntaxError, "class body \\8 rejects");
        }
    },
    {
        name: "sloppy \\8 and \\9 keep cooking to 8 and 9",
        body: function () {
            assert.areEqual("8", eval("var s=\"\\8\"; s;"), "sloppy \\8 cooks to 8");
            assert.areEqual("9", eval("var s='\\9'; s;"), "sloppy \\9 cooks to 9");
            assert.areEqual(1, eval("var o={\"\\8\":1}; o[\"8\"];"), "sloppy object key \\8 works");
        }
    },
    {
        name: "template \\08 and \\09: untagged rejects, tagged cooks undefined with raw preserved",
        body: function () {
            assert.throws(function () { eval("`\\08`;"); }, SyntaxError, "untagged \\08 rejects");
            assert.throws(function () { eval("`\\09`;"); }, SyntaxError, "untagged \\09 rejects");
            assert.throws(function () { eval("\"use strict\"; `\\08`;"); }, SyntaxError, "strict untagged \\08 rejects");
            assert.throws(function () { eval("`a\\08${1}b\\09`;"); }, SyntaxError, "untagged multi-piece \\08/\\09 rejects");

            var r8 = tagIdentity`\08`;
            assert.isTrue(r8[0] === undefined, "tagged \\08 cooked is undefined");
            assert.areEqual("\\08", r8.raw[0], "tagged \\08 raw preserved");

            var r9 = tagIdentity`x\09${1}y\08`;
            assert.isTrue(r9[0] === undefined && r9[1] === undefined, "tagged \\09/\\08 pieces cook undefined");
            assert.areEqual("x\\09", r9.raw[0], "tagged \\09 raw piece preserved");
            assert.areEqual("y\\08", r9.raw[1], "tagged \\08 raw piece preserved");

            assert.areEqual("\0" + "a", eval("`\\0a`;"), "untagged \\0 followed by non-digit stays NUL + a");
        }
    },
    {
        name: "strict 08/09 numerics reject while sloppy stays decimal",
        body: function () {
            assert.throws(function () { eval("\"use strict\"; 08;"); }, SyntaxError, "strict 08 rejects");
            assert.throws(function () { eval("\"use strict\"; 09;"); }, SyntaxError, "strict 09 rejects");
            assert.throws(function () { eval("\"use strict\"; 08.5;"); }, SyntaxError, "strict 08.5 rejects");
            assert.throws(function () { eval("\"use strict\"; 08e1;"); }, SyntaxError, "strict 08e1 rejects");
            assert.throws(function () { eval("\"use strict\"; 078;"); }, SyntaxError, "strict 078 rejects");
            assert.throws(function () { eval("\"use strict\"; ({08: 1});"); }, SyntaxError, "strict {08:1} rejects");
            assert.throws(function () { eval("class C { 08() {} }"); }, SyntaxError, "class method 08 rejects");
            assert.throws(function () { eval("08_0;"); }, SyntaxError, "sloppy 08_0 separator rejects");
            assert.throws(function () { eval("09_0;"); }, SyntaxError, "sloppy 09_0 separator rejects");
            assert.throws(function () { eval("WScript.LoadScript('08;', 'module');"); }, SyntaxError, "module 08 rejects");
            assert.areEqual(8, eval("08;"), "sloppy 08 stays 8");
            assert.areEqual(9, eval("09;"), "sloppy 09 stays 9");
            assert.areEqual(8.5, eval("08.5;"), "sloppy 08.5 stays 8.5");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
