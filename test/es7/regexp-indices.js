//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2022 RegExp 'd' flag / match indices, core slice (doc/c2-s9-namedgroups.md,
// sub-batch 2): 'd' flag parsing (literal, constructor, compile), the
// "indices" own property on exec/match results ('d' flag only), per-capture
// [start, end] pairs with undefined for non-participating groups, and the
// indices array's "groups" property (null-prototype object with named
// groups, present-with-value-undefined without). Everything here is
// unconditional (no flag); the -ES6RegExSymbols shapes (matchAll, @@match,
// @@replace) live in regexp-indices-symbols.js. The RegExp.prototype
// accessor surface (hasIndices getter, flags getter with 'd') is gated on
// -ES6RegExPrototypeProperties and is covered by the test262 corpus runs.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

function assertPair(pair, start, end, message) {
    assert.isTrue(Array.isArray(pair), (message || "pair") + " is an array");
    assert.areEqual(2, pair.length, (message || "pair") + " length");
    assert.areEqual(start, pair[0], (message || "pair") + " start");
    assert.areEqual(end, pair[1], (message || "pair") + " end");
}

function sameIndices(actual, expected, message) {
    // expected: array whose entries are [start, end] or undefined
    assert.isTrue(Array.isArray(actual), (message || "indices") + " is an array");
    assert.areEqual(expected.length, actual.length, (message || "indices") + " length");
    for (var i = 0; i < expected.length; i++) {
        if (expected[i] === undefined) {
            assert.isUndefined(actual[i], (message || "indices") + "[" + i + "]");
        } else {
            assertPair(actual[i], expected[i][0], expected[i][1], (message || "indices") + "[" + i + "]");
        }
    }
}

var tests = [
    {
        name: "'d' flag syntax: accepted and rejected shapes",
        body() {
            compileOk("/a/d");
            compileOk("/a/dg");
            compileOk("/a/gd");
            compileOk("/a/dgimsuy");
            compileOk("/a/ud", "d combines with u");
            compileOk("/a/yd", "d combines with y");
            compileOk("/a/sd", "d combines with s");
            assert.doesNotThrow(function () { new RegExp("a", "d"); }, "constructor, d alone");
            assert.doesNotThrow(function () { new RegExp("a", "dgimsuy"); }, "constructor, all flags");
            assert.doesNotThrow(function () { new RegExp("a", "gimsuyd"); }, "constructor, order irrelevant");
            var re = new RegExp("a", "d");
            assert.isTrue(re.test("a"), "d flag pattern still matches");
            compileError("/a/dd", "duplicate d");
            assert.throws(function () { new RegExp("a", "dd"); }, SyntaxError, "constructor duplicate d");
            compileError("/a/qd", "unknown flag before d");
            assert.throws(function () { new RegExp("a", "dq"); }, SyntaxError, "unknown flag after d");
        }
    },
    {
        name: "indices is present only with the 'd' flag",
        body() {
            var plain = /b(c)/.exec("abcd");
            assert.isFalse(plain.hasOwnProperty("indices"), "no d flag: no own indices");
            assert.isUndefined(plain.indices, "no d flag: reading indices yields undefined");
            assert.isFalse("ab".match(/a/g).hasOwnProperty("indices"), "global array never has indices");
            var withD = /b(c)/d.exec("abcd");
            assert.isTrue(withD.hasOwnProperty("indices"), "d flag: own indices");
            assert.isFalse("abcd".match(/a*/gd).hasOwnProperty("indices"), "global d match returns the plain matches array");
        }
    },
    {
        name: "indices basics: capture 0 first, pairs are two-element arrays",
        body() {
            var m = /b(c)/d.exec("abcd");
            sameIndices(m.indices, [[1, 3], [2, 3]], "b(c) on abcd");
            assert.areEqual(m.length, m.indices.length, "indices length matches match length");
            assert.areEqual("bc", "abcd".slice(m.indices[0][0], m.indices[0][1]), "pair 0 slices to the whole match");
            assert.areEqual("c", "abcd".slice(m.indices[1][0], m.indices[1][1]), "pair 1 slices to the capture");
            assert.isTrue(Array.isArray(m.indices[0]), "pair is an array");
            assert.areEqual("number", typeof m.indices[0][0], "start is a number");
            assert.areEqual("number", typeof m.indices[0][1], "end is a number");
            // Empty match reports [offset, offset]
            var empty = /(?:)/d.exec("ab");
            sameIndices(empty.indices, [[0, 0]], "empty match at 0");
        }
    },
    {
        name: "indices descriptors are CreateDataProperty-shaped",
        body() {
            var counter = 0;
            Object.defineProperty(Array.prototype, "groups", { set() { counter++; } });
            var m = /(?<x>a)/d.exec("a");
            assert.areEqual(0, counter, "groups on indices is defined, not set");
            var d0 = Object.getOwnPropertyDescriptor(m.indices, "0");
            assert.isTrue(d0.writable && d0.enumerable && d0.configurable, "element descriptor");
            assert.isTrue(Array.isArray(m.indices), "indices is a real Array");
            assert.areEqual(Array.prototype, Object.getPrototypeOf(m.indices), "indices prototype");
            delete Array.prototype.groups;
        }
    },
    {
        name: "non-participating and optional groups",
        body() {
            sameIndices("abd".match(/b(c)?/d).indices, [[1, 2], undefined], "optional group unmatched");
            sameIndices("ab".match(/(a)(b)?(c)?/d).indices, [[0, 2], [0, 1], [1, 2], undefined], "mixed optional groups");
            sameIndices("ab".match(/(?:(a)|(b))+/d).indices, [[0, 2], undefined, [1, 2]], "alternation: only the last branch participates");
        }
    },
    {
        name: "quantified groups report the last iteration",
        body() {
            sameIndices("aaaa".match(/(a)+/d).indices, [[0, 4], [3, 4]], "last iteration of +");
            sameIndices("abab".match(/(ab)+/d).indices, [[0, 4], [2, 4]], "last iteration of a multi-char group");
            sameIndices("aaa".match(/(a*)/d).indices, [[0, 3], [0, 3]], "greedy star covers the run");
        }
    },
    {
        name: "lookahead captures get indices",
        body() {
            sameIndices("ab".match(/a(?=b)/d).indices, [[0, 1]], "lookaround itself is not a capture");
            sameIndices("ab".match(/a(?=(b))/d).indices, [[0, 1], [1, 2]], "capture inside lookahead has indices");
            sameIndices("ab".match(/(?!(c))a/d).indices, [[0, 1], undefined], "capture inside failed lookahead is undefined");
        }
    },
    {
        name: "indices.groups without named groups: present with value undefined",
        body() {
            var indices = /./d.exec("a").indices;
            assert.isTrue(indices.hasOwnProperty("groups"), "groups property exists without named groups");
            assert.isUndefined(indices.groups, "its value is undefined");
            var desc = Object.getOwnPropertyDescriptor(indices, "groups");
            assert.isTrue(desc.writable && desc.enumerable && desc.configurable, "groups descriptor");
        }
    },
    {
        name: "indices.groups with named groups: null prototype, textual order, unmatched undefined",
        body() {
            var m = /(?<a>a)(y)?/d.exec("a");
            assert.areEqual(null, Object.getPrototypeOf(m.indices.groups), "null prototype");
            assertPair(m.indices.groups.a, 0, 1, "named group a");
            assert.isUndefined(m.indices.groups.y, "non-participating named group");
            assert.areEqual("fst,snd", Object.getOwnPropertyNames(/(?<fst>.)|(?<snd>.)/du.exec("abcd").indices.groups).join(), "property names in textual order");
            // Backreference-driven match keeps per-group pairs consistent
            var bm = /(?<a>.)(?<b>.)(?<c>.)\k<c>\k<b>\k<a>/d.exec("abccba").indices.groups;
            assertPair(bm.a, 0, 1, "backref a");
            assertPair(bm.b, 1, 2, "backref b");
            assertPair(bm.c, 2, 3, "backref c");
            // Named and unnamed groups mix: numbered pairs for both, named mirror only for named
            var mix = /(?<n>x)(y)/d.exec("xy");
            assert.areEqual(3, mix.indices.length, "three captures including capture 0");
            assertPair(mix.indices[2], 1, 2, "unnamed group pair");
            assertPair(mix.indices.groups.n, 0, 1, "named group pair");
            assert.isFalse(Object.prototype.hasOwnProperty.call(mix.indices.groups, "length"), "groups mirrors names only");
        }
    },
    {
        name: "'__proto__' group name in indices.groups",
        body() {
            var m = /(?<__proto__>.)/d.exec("a");
            assertPair(m.indices.groups.__proto__, 0, 1, "__proto__ name is an own data property");
            assert.areEqual(null, Object.getPrototypeOf(m.indices.groups), "prototype stays null");
        }
    },
    {
        name: "sticky + d and lastIndex interplay",
        body() {
            var sticky = /a/dy;
            assert.areEqual(null, sticky.exec("bab"), "sticky at lastIndex 0 finds no 'a'");
            assert.areEqual(0, sticky.lastIndex, "failed sticky resets lastIndex");
            sticky.lastIndex = 1;
            var hit = sticky.exec("bab");
            sameIndices(hit.indices, [[1, 2]], "sticky hit at lastIndex 1");
            assert.areEqual(2, sticky.lastIndex, "sticky advanced past the match");
            var failing = /x/dy;
            failing.lastIndex = 1;
            assert.areEqual(null, failing.exec("abc"), "failed match");
            assert.areEqual(0, failing.lastIndex, "reset to 0");
            var g = /a/gd;
            sameIndices(g.exec("aa").indices, [[0, 1]], "global exec first");
            assert.areEqual(1, g.lastIndex, "global exec advances");
            sameIndices(g.exec("aa").indices, [[1, 2]], "global exec second");
            assert.areEqual(null, g.exec("aa"), "global exec exhausted");
        }
    },
    {
        name: "constructor, literal and compile() paths agree",
        body() {
            var constructed = new RegExp("b(c)", "d");
            sameIndices(constructed.exec("abcd").indices, [[1, 3], [2, 3]], "constructed");
            var compiled = /z/;
            compiled.compile("b(c)", "d");
            sameIndices(compiled.exec("abcd").indices, [[1, 3], [2, 3]], "compiled");
            assert.areEqual("/b(c)/d", compiled.toString(), "compile round-trips source and flags");
            sameIndices(/b(c)/d.exec("abcd").indices, [[1, 3], [2, 3]], "literal");
            var noD = /b(c)/;
            noD.compile("b(c)", "d");
            assert.isTrue(noD.exec("abcd").hasOwnProperty("indices"), "compile can add the d flag");
            noD.compile("b(c)");
            assert.isFalse(noD.exec("abcd").hasOwnProperty("indices"), "compile can drop the d flag");
        }
    },
    {
        name: "flags round-trip: d sorts first, order-preserving elsewhere",
        body() {
            assert.areEqual("/x/d", /x/d.toString(), "d alone");
            assert.areEqual("/x/dg", /x/dg.toString(), "d before g");
            assert.areEqual("/x/dg", /x/gd.toString(), "input order does not matter");
            assert.areEqual("/x/dsuy", /x/ydsu.toString(), "canonical order with s, u, y");
            assert.areEqual("/x/dgimsuy", new RegExp("x", "uysmgid").toString(), "all flags canonicalize");
            assert.areEqual("/(?:)/d", new RegExp("", "d").toString(), "empty source round-trip");
        }
    },
    {
        name: "replace, split and search are unaffected by the d flag",
        body() {
            assert.areEqual("b", "ab".replace(/a(b)/d, "$1"), "replace with capture");
            assert.areEqual("[a]", "ab".replace(/(?<y>a)(b)?/d, "[$<y>]"), "replace with named substitution");
            assert.areEqual("a-b", "a1b".split(/\d/d).join("-"), "split");
            assert.areEqual(1, "bab".search(/a/d), "search");
            assert.areEqual(true, /a/d.test("a"), "test");
        }
    },
    {
        name: "non-global String.prototype.match returns the exec result with indices",
        body() {
            var m = "abcd".match(/b(c)/d);
            assert.isTrue(m.hasOwnProperty("indices"), "match result is the exec result");
            sameIndices(m.indices, [[1, 3], [2, 3]], "match indices");
            var g = "abab".match(/a/gd);
            assert.areEqual("a,a", g.toString(), "global match collects strings");
            assert.isFalse(g.hasOwnProperty("indices"), "no indices on the global array");
        }
    },
    {
        name: "unicode patterns report code-unit indices",
        body() {
            // U+1D400 is two code units; indices stay in code units of the input
            sameIndices("\u{1D400}a".match(/(.)/du).indices, [[0, 2], [0, 2]], "astral whole match");
            sameIndices("\u{1D400}a".match(/(a)/du).indices, [[2, 3], [2, 3]], "match after an astral char");
            sameIndices("bab".match(/.(\w\w)/du).indices, [[0, 3], [1, 3]], "bmp unicode pairs");
        }
    },
    {
        name: "'d' flag in deferred functions parses and validates the same",
        body() {
            // Scripts past the deferral threshold scan regex literals without
            // an AST; the flag validation must run there too.
            var deferredPad = new Array(1500).join("x=1;");
            assert.doesNotThrow(function () { eval("function f(){ return /a/d.test('a'); }" + deferredPad); }, "d literal in deferred function");
            assert.throws(function () { eval("function f(){ /a/dd; }" + deferredPad); }, SyntaxError, "duplicate d in deferred function");
        }
    },];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
