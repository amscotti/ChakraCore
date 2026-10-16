//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2022 'd' flag / match indices on the -ES6RegExSymbols surfaces
// (doc/c2-s9-namedgroups.md, sub-batch 2): matchAll iterator results carry
// per-step indices (and indices.groups), @@match returns the exec result
// with indices, and @@replace neither needs nor reads them. Registered
// with -ES6RegExSymbols only, like matchall.js; the unconditional core
// lives in regexp-indices.js.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertPair(pair, start, end, message) {
    assert.isTrue(Array.isArray(pair), (message || "pair") + " is an array");
    assert.areEqual(2, pair.length, (message || "pair") + " length");
    assert.areEqual(start, pair[0], (message || "pair") + " start");
    assert.areEqual(end, pair[1], (message || "pair") + " end");
}

var tests = [
    {
        name: "matchAll results carry indices per step",
        body() {
            var matches = [..."abab".matchAll(/a/gd)];
            assert.areEqual(2, matches.length);
            assertPair(matches[0].indices[0], 0, 1, "first step");
            assertPair(matches[1].indices[0], 2, 3, "second step");
            assert.isUndefined(matches[0].indices[1], "no captures");
        }
    },
    {
        name: "matchAll results carry indices.groups per step",
        body() {
            var matches = [..."a1b2".matchAll(/(?<letter>[ab])|(?<digit>\d)/gd)];
            assert.areEqual(4, matches.length);
            assertPair(matches[0].indices.groups.letter, 0, 1, "letter step 1");
            assert.isUndefined(matches[0].indices.groups.digit, "digit step 1");
            assert.isUndefined(matches[1].indices.groups.letter, "letter step 2");
            assertPair(matches[1].indices.groups.digit, 1, 2, "digit step 2");
            assert.areEqual(undefined, matches[0].indices.groups.nonexistent, "unknown name");
            // Non-participating numbered group is undefined in the array itself
            assert.isUndefined(matches[0].indices[2], "alternation: losing branch undefined");
        }
    },
    {
        name: "non-global @@matchAll yields one exec result with indices",
        body() {
            var iter = /b(c)/d[Symbol.matchAll]("abcd");
            var first = iter.next();
            assert.areEqual(false, first.done, "one result");
            assertPair(first.value.indices[0], 1, 3, "whole match");
            assertPair(first.value.indices[1], 2, 3, "capture");
            assert.areEqual(true, iter.next().done, "then done");
            // String.prototype.matchAll itself requires the global flag
            assert.throws(function () { "abcd".matchAll(/b(c)/d); }, TypeError, "non-global matchAll throws");
        }
    },
    {
        name: "@@match returns the exec result, indices included",
        body() {
            var m = RegExp.prototype[Symbol.match].call(/b(c)/d, "abcd");
            assertPair(m.indices[0], 1, 3, "@@match non-global");
            assertPair(m.indices[1], 2, 3, "@@match capture");
            var g = RegExp.prototype[Symbol.match].call(/a/gd, "abab");
            assert.areEqual("a,a", g.join(), "@@match global collects strings");
            assert.isFalse(g.hasOwnProperty("indices"), "@@match global array has no indices");
        }
    },
    {
        name: "@@replace does not read indices",
        body() {
            assert.areEqual("b", "ab".replace(/a(b)/d, "$1"), "captures substitute");
            assert.areEqual("[a]", "ab".replace(/(?<y>a)(b)?/d, "[$<y>]"), "named substitution with an unmatched group");
            // A custom exec result without indices must not break @@replace
            class FakeRegExp extends RegExp {
                exec(subject) {
                    const fakeResult = ["ab", "a"];
                    fakeResult.index = 0;
                    fakeResult.groups = { a: "b" };
                    return fakeResult;
                }
            }
            var fake = new FakeRegExp("z", "d");
            assert.areEqual("bX", "ab".replace(fake, "$<a>X"), "replace only consults captures and groups");
            assert.doesNotThrow(function () { "ab".replace(fake, "$<a>"); }, "no indices property needed");
        }
    },
    {
        name: "exec through the symbol path keeps lastIndex semantics with d",
        body() {
            var re = /a/dy;
            re.lastIndex = 2;
            assert.areEqual(null, re.exec("bab"), "sticky miss");
            assert.areEqual(0, re.lastIndex, "reset");
            re.lastIndex = 1;
            assertPair(re.exec("bab").indices[0], 1, 2, "sticky hit");
            assert.areEqual(2, re.lastIndex, "advance");
        }
    },];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
