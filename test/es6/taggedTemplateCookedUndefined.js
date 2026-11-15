//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
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
        name: "control: untagged `\\08` stays accepted (e7-R1 owns the compound spelling)",
        body: function () {
            assert.areEqual("\0" + "8", eval("`\\08`;"), "untagged \\08 still cooks NUL + 8");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
