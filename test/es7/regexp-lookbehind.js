//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2018 RegExp lookbehind assertions (doc/c2-s9-namedgroups.md
// sub-batch 4): (?<=...) and (?<!...), matched right-to-left by mirroring
// the body and running it over a reversed window (alternation order kept,
// greedy loops extend leftward, forward references see groups in reversed
// temporal order, captures persist from positive and reset from negative
// lookbehinds). Lookbehinds are not quantifiable in either mode. Every
// assertion was mirrored in Node 26.5.0 first. Symbols-flag surfaces
// (matchAll etc.) stay in the -symbols files' territory; this file runs
// on the default launch.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

var tests = [
    {
        name: "Syntax: forms accepted and rejected",
        body() {
            compileOk("/(?<=a)b/", "positive");
            compileOk("/(?<!a)b/", "negative");
            compileOk("/(?<=a|b)c/", "alternation");
            compileOk("/(?<=[a-c]*)x/", "class star");
            compileOk("/(?<=a)b/u", "u flag");
            compileOk("/(?<=a)b/gimsuy", "all flags");
            compileOk("/(?<=)/", "empty body");
            compileOk("/(?<=a(?=b))c/", "lookahead inside");
            compileOk("/(?=(?<=a))b/", "lookbehind inside lookahead");
            compileOk("/(?<=a(?<=b))c/", "nested lookbehind");
            compileOk("/(?<a>x)(?<=\\k<a>)/", "named backreference inside");
            compileOk("new RegExp('(?<=a)b')");
            compileOk("new RegExp('(?<=a)b', 'u')");
            compileError("/(?<=a)*/", "star");
            compileError("/(?<=a)?/", "question");
            compileError("/(?<=a)+/", "plus");
            compileError("/(?<=a){2}/", "braces");
            compileError("/(?<=a){1,2}?/", "braces lazy");
            compileError("/(?<!a)*/", "negative star");
            compileError("/(?<!a)?/u", "u question");
            compileError("/(?<=/", "unterminated");
            compileError("new RegExp('(?<=a)*')");
            compileError("/x(?<=a)?/", "quantifier after mid-pattern lookbehind");
            compileError("/(?</", "eof after (?<");
            // A '{' that is not a quantifier is still a literal.
            compileOk("/(?<=a){,x}/", "brace not a quantifier");
            // Named groups still parse beside lookbehind syntax.
            compileOk("/(?<ab>x)|(?<=a\\k<ab>)/", "named group + lookbehind");
        }
    },
    {
        name: "Positive lookbehind: fixed and variable widths",
        body() {
            assert.areEqual(3, "abcd".search(/(?<=abc)d/), "fixed");
            assert.areEqual(null, "abd".match(/(?<=abc)d/), "fixed miss");
            assert.areEqual(2, "a,1".search(/(?<=,)\d/), "looks left of match start");
            assert.areEqual(3, "abcd".search(/(?<=a.c)d/), "dot in body");
            var m = "abcdefdef".match(/(?<=^(\w+))def/);
            assert.areEqual("def|abc", m[0] + "|" + m[1], "unbounded greedy from ^");
            assert.areEqual("cd", "xabcd".match(/.*(?<=(..|...|....))(.*)/)[1], "alternation textual order");
            assert.areEqual("bcd", "xabcd".match(/.*(?<=(xx|...|....))(.*)/)[1], "first matching alternative");
            assert.areEqual(0, "abc".search(/(?<=)/), "empty body matches");
            assert.areEqual(null, "foooo".match(/^foooo(?<=^o+)$/), "^ in body anchors at real 0");
            assert.areEqual("foooo", "foooo".match(/^foooo(?<=fo+)/)[0], "f prefix body");
            assert.areEqual("foooo", "foooo".match(/^foooo(?<=fo*)/)[0], "f prefix star body");
        }
    },
    {
        name: "Right-to-left semantics: reversed exploration order",
        body() {
            assert.areEqual("a", "abcdef".match(/(?<=(\w){3})def/)[1], "quantified group last iteration");
            assert.areEqual("c", "abcdef".match(/(?<=(?<a>\w){3})f/u).groups.a, "named quantified");
            var m = "aaa".match(/a(?<=(a?)(a*))/);
            assert.areEqual("", m[1], "later term matched first (reverse order)");
            assert.areEqual("a", m[2], "earlier term greedy leftward");
            var alts = "xabbb".match(/abbb(?<=(a*)|(?:(bb)))/);
            assert.areEqual("", alts[1], "first alternative wins even matching empty");
            assert.areEqual(undefined, alts[2], "second alternative never ran");
            assert.areEqual("abab", "ababc".match(/(?<=(\w+)\1)c/)[1], "forward ref inside body sees stale group");
            assert.areEqual(null, "ababdc".match(/(?<=\1(\w+))c/), "self ref to later group");
        }
    },
    {
        name: "Negative lookbehind",
        body() {
            assert.areEqual("abc", "abcdef".match(/(?<!abc)\w\w\w/)[0], "filters position after abc");
            assert.areEqual(null, "abcdef".match(/(?<!a.c)def/), "dot body");
            assert.areEqual(null, "abcdef".match(/(?<!a[a-z]{2})def/), "class body");
            var m = "abcdef".match(/(?<!(^|[ab]))\w{2}/);
            assert.areEqual(undefined, m[1], "captures inside negative body reset");
            assert.areEqual(null, "a".match(/(?<!)a/), "negative of empty body always fails");
            var neg = "abcdef".match(/(?<!(?<a>\d){3})f|f/);
            assert.areEqual("f", neg[0], "negative alternative skipped");
            assert.areEqual(undefined, neg.groups.a, "named group undefined");
            assert.areEqual(null, "abcdef".match(/(?<!(?<a>\D){3})f/), "negative body matches");
            assert.areEqual("fno", "fno".match(/^f.o(?<!foo)$/)[0], "negative not foo");
        }
    },
    {
        name: "Captures: persistence and reset on backtracking out",
        body() {
            var m = "xy".match(/(?<=(x))y/);
            assert.areEqual("x", m[1], "positive lookbehind captures persist");
            assert.areEqual(undefined, "z".match(/(?:(?<=a)(x)|z)/)[1], "failed positive resets");
            assert.areEqual(["a", undefined], "abcdef".match(/(?<=([ab]{1,2})\D|(abc))\w/).slice(1), "alternation groups");
            assert.areEqual(["a", "b"], "abcdef".match(/\D(?<=([ab]+))(\w)/).slice(1), "capture feeding later group");
        }
    },
    {
        name: "Backreferences inside lookbehind",
        body() {
            assert.areEqual(["b", "bb"], "abb".match(/(.)(?<=(\1\1))/).slice(1), "outer capture replayed twice");
            assert.areEqual(["B", "bB"], "abB".match(/(.)(?<=(\1\1))/i).slice(1), "case-insensitive");
            assert.areEqual(["aB", "a"], "aabAaBa".match(/((\w)\w)(?<=\1\2\1)/i).slice(1), "multi-char reversed replay");
            assert.areEqual(["Ba", "a"], "aabAaBa".match(/(\w(\w))(?<=\1\2\1)/i).slice(1), "reversed replay position");
            assert.areEqual(["b", "B"], "abaBbAa".match(/(?=(\w))(?<=(\1))./i).slice(1), "lookahead capture read inside");
            assert.areEqual(["ac", "ac"], /(?<=a(\2)b(..\1))b/.exec("aacbacb").slice(1), "mutual forward references");
            assert.areEqual(null, /(a)x(?<=\1)/.exec("ax"), "outer text not immediately left");
            assert.areEqual(["a"], /(a)a(?<=\1)/.exec("aa").slice(1), "outer text immediately left");
            assert.areEqual(null, "  .foo\"  ".match(/(?<=(.))(\w+)(?=\1)/), "quote mismatch");
            assert.areEqual(["b1234"], "ab1234c".match(/(?<=(b\d+))c/).slice(1), "greedy mixed body");
        }
    },
    {
        name: "Named and duplicate named groups inside lookbehind",
        body() {
            var m = "abcdef".match(/(?<=(?<n>\w{2}))def/);
            assert.areEqual("bc", m.groups.n, "named group value");
            assert.areEqual("bc", m[1], "numbered alias");
            var mi = "ab".match(/(?<=(?<n>a))b/d);
            assert.areEqual("0,1", mi.indices[1].join(","), "indices for lookbehind capture");
            assert.areEqual("0,1", mi.indices.groups.n.join(","), "indices.groups");
            assert.areEqual(undefined, "abcdef".match(/(?<!(?<n>\d){3})f|f/u).groups.n, "other alternative leaves undefined");
            assert.areEqual(["fst", "snd"], Object.getOwnPropertyNames(/(?<=(?<fst>.)|(?<snd>.))/u.exec("abcd").groups), "property order textual");
            assert.areEqual("y", "y".match(/(?<=(?<a>x))|(?<a>y)/).groups.a, "duplicate names: second alternative");
            assert.areEqual("x", "x".match(/(?<=(?<a>x))|(?<a>y)/).groups.a, "duplicate names: lookbehind alternative");
            assert.areEqual("a[a]", "ab".replace(/(?<=(?<g>a))b/, "[$<g>]"), "$<name> from lookbehind capture");
        }
    },
    {
        name: "Nested assertions",
        body() {
            assert.areEqual(["bc"], "abcdef".match(/(?<=a(?=([^a]{2})d)\w{3})\w\w/).slice(1), "capture inside lookahead inside lookbehind");
            assert.areEqual(null, "abcdef".match(/(?<=a(?=([bc]{2}(?<!a*))d)\w{3})\w\w/), "unbounded negative inside");
            assert.areEqual(["bc"], "abcdef".match(/(?<=a(?=([bc]{2}(?<!a{2}))d)\w{3})\w\w/).slice(1), "bounded negative inside");
            assert.areEqual(1, "ab".search(/(?=(?<=a))b/), "lookbehind inside lookahead");
            assert.areEqual(4, "ab cdef".search(/(?<=\B)(?<=c(?<=\w))\w{3}/), "lookbehind inside lookbehind");
            assert.areEqual("faaa", "faaao".match(/^faaao?(?<=^f[oa]+(?=o))/)[0], "mix at line start");
            assert.areEqual(null, "abcdefgabcdefg".slice(1).match(/(?=(abcdefg))(?<=\1)/), "sliced string lookahead + lookbehind");
        }
    },
    {
        name: "Anchors and boundaries inside lookbehind",
        body() {
            assert.areEqual("def", "abcdef".match(/(?<=^abc)def/)[0], "^ literal");
            assert.areEqual("def", "abcdef".match(/(?<=^[a-c]{3})def/)[0], "^ class");
            assert.areEqual(null, "abcdef".match(/(?<=^[^a-c]{3})def/), "^ class must fail");
            assert.areEqual(7, "xyz\nabcdef".search(/(?<=^[a-c]{3})def/m), "multiline ^");
            assert.areEqual(["ab", "cd", "efg"], "ab\ncd\nefg".match(/(?<=^)\w+/gm), "multiline ^ global");
            assert.areEqual(["ab", "cd", "efg"], "ab\ncd\nefg".match(/\w+(?<=$)/gm), "multiline $ global");
            assert.areEqual(null, "ax".match(/(?<=a$)x/), "$ not at real end");
            assert.areEqual(1, "a".search(/(?<=a$)/), "$ at real end");
            assert.areEqual(4, "abc def".search(/(?<=\b)[d-f]{3}/), "word boundary");
            assert.areEqual(null, "abcdef".match(/(?<=\b)[d-f]{3}/), "no boundary mid-word");
            assert.areEqual(4, "ab cdef".search(/(?<=\B)\w{3}/), "non-boundary");
        }
    },
    {
        name: "Unicode mode shapes",
        body() {
            assert.areEqual(2, "\u{1F600}x".search(/(?<=\u{1F600})x/u), "astral literal");
            assert.areEqual(2, "\u{1F600}x".search(/(?<=.)x/u), "astral dot");
            assert.areEqual(2, "\u{1F600}x".search(/(?<=[\u{1F600}])x/u), "astral class");
            assert.areEqual(2, "\u{10428}x".search(/(?<=[\u{10400}-\u{10428}])x/u), "astral class range");
            assert.areEqual(2, "\u{10428}x".search(/(?<=[\u{10400}-\u{10428}])x/iu), "astral class range case-insensitive");
            assert.areEqual(3, "a\u{1F600}x".search(/(?<=\u{1F600}|a)x/u), "astral alternation");
            var m = "\u{1F600}x".match(/(?<=(?<emoji>\u{1F600}))x/u);
            assert.areEqual("\u{1F600}", m.groups.emoji, "astral named capture");
            assert.areEqual(0, "x".search(/(?<=(?:\u{1F600}|))x/u), "empty alternative");
            var ci = "\u{1F600}\u{1F600}x".match(/(?<=\u{1F600}(?<z>.)?)x/u);
            assert.areEqual(4, ci.index, "astral optional");
            assert.areEqual("\u{1F600}", ci.groups.z, "astral optional capture");
        }
    },
    {
        name: "Unicode mode: code-point-reversed window (astral backreferences)",
        body() {
            // The reversed lookbehind window is code-point reversed in u-mode
            // (surrogate pairs stay intact), so backreferences and literals
            // compare real code points. All expectations mirrored in Node
            // 26.5.0.
            var iu = /(\u{1F600})(?<=\1)/iu.exec("\u{1F600}\u{1F600}");
            assert.areEqual(["\u{1F600}", "\u{1F600}"], [iu[0], iu[1]], "i+u astral backref");
            var uu = /(\u{1F600})(?<=\1)/u.exec("\u{1F600}\u{1F600}");
            assert.areEqual(["\u{1F600}", "\u{1F600}"], [uu[0], uu[1]], "u-only astral backref");
            var mix = /(a\u{1F600})(?<=\1)/u.exec("ba\u{1F600}a\u{1F600}");
            assert.areEqual(["a\u{1F600}", "a\u{1F600}"], [mix[0], mix[1]], "multi-code-point group");
            assert.areEqual(null, /(\u{1F600})(?<=\1)x/u.exec("\u{1F600}\u{1F601}"), "u-only astral backref mismatch");
            var hit = /(\u{1F600})(?<=\1)x/u.exec("\u{1F600}\u{1F600}x");
            assert.areEqual(["\u{1F600}x", "\u{1F600}"], [hit[0], hit[1]], "u-only astral backref hit");
            // DESERET small (U+10428) / capital (U+10400) letters are one
            // simple case-folding pair; the comparison must fold over the
            // code-point-reversed window.
            var des = /(\u{10428})(?<=\1)/iu.exec("\u{10428}\u{10428}");
            assert.areEqual(["\u{10428}", "\u{10428}"], [des[0], des[1]], "i+u DESERET same case");
            var fold = /(\u{10400}).(?<=\1..)!/iu.exec("\u{10400}\u{10428}\u{10428}\u{10400}!");
            assert.areEqual(["\u{10428}\u{10400}!", "\u{10428}"], [fold[0], fold[1]], "i+u DESERET case pair across body");
            assert.areEqual(null, /(\u{10400}).(?<=\1..)!/u.exec("\u{10400}\u{10428}\u{10428}\u{10400}!"), "case-sensitive control");
            // BMP case-insensitive backreference through the reversed window.
            var bmp = "aAb".match(/(a)(?<=\1)b/iu);
            assert.areEqual(["Ab", "A"], [bmp[0], bmp[1]], "i+u BMP backref");
            assert.areEqual(null, "aAb".match(/(a)(?<=\1)b/u), "u BMP backref case-sensitive control");
        }
    },
    {
        name: "Unicode mode: code-point-reversed window (atoms and captures)",
        body() {
            // Atoms and captures over astral left contexts. (The counting
            // shapes that would require >=2 code points over a single astral
            // left-context character -- (?<=..)x/u over "\u{1F600}x" etc. --
            // are pinned in doc/c2-s9-namedgroups.md sub-batch 4's Review
            // fixes: the engine's u-mode dot/class expansion admits a
            // unit-wise parse in the FORWARD path too, a pre-existing
            // divergence from Node unrelated to lookbehind.)
            assert.areEqual(2, "\u{1F600}x".search(/(?<=.{1})x/u), "one astral dot count");
            assert.areEqual(4, "\u{1F600}\u{1F601}z".search(/(?<=..)z/u), "two astral code points");
            assert.areEqual(3, "a\u{1F600}x".search(/(?<=[\s\S][\s\S])x/u), "BMP + astral code points");
            assert.areEqual(2, "\u{1F600}x".search(/(?<=^\u{1F600})x/u), "anchor after astral");
            assert.areEqual(true, /\u{1F600}(?<=\u{1F600}$)/u.test("\u{1F600}"), "dollar at astral end");
            assert.areEqual(null, /(?<=..)x/u.exec("\uDC00x"), "lone trail is one code point");
            var lone = /(?<=\uD800x)z/u.exec("\uD800xz");
            assert.areEqual("z", lone && lone[0], "lone lead in left context");
            var two = "\u{1F600}\u{1F601}z".match(/(?<=(..))z/u);
            assert.areEqual(["\u{1F600}\u{1F601}", 4], [two[1], two[1].length], "two-code-point capture span");
            var split = "\u{1F600}\u{1F601}z".match(/(?<=(.)(.))z/u);
            assert.areEqual(["\u{1F600}", "\u{1F601}"], [split[1], split[2]], "per-code-point captures");
            var ciSpan = "a\u{1F600}b".match(/(?<=(?<q>\u{1F600}|\w))b/ud);
            assert.areEqual("[1,3]", JSON.stringify(ciSpan.indices.groups.q), "d-flag indices of astral body group");
            var star = "\u{1F600}\u{1F601}x".match(/(?<=(\u{1F600})*)x/u);
            assert.areEqual([4, undefined], [star.index, star[1]], "astral star zero iterations");
            var plus = "a\u{1F600}x".match(/(?<=([\s\S])+)x/u);
            assert.areEqual([3, "a"], [plus.index, plus[1]], "leftward greedy last iteration");
        }
    },
    {
        name: "Lookbehind body success flag does not leak from nested assertions",
        body() {
            // A nested suffix-frame assertion's success used to leave
            // lookbehindBodySucceeded set, so a later mirrored-term failure
            // still reported the enclosing body as successful.
            assert.areEqual(null, /(?<=x(?=c)c)y/.exec("aacy"), "positive lookbehind false positive");
            var neg = /(?<!x(?=c)c)y/.exec("aacy");
            assert.areEqual("y", neg && neg[0], "negative lookbehind false negative");
            assert.areEqual(null, /(?<=x(?=(?<=a))c)y/.exec("aacy"), "three assertions deep");
            assert.areEqual(null, /(?<=x(?=c)(?=c)c)y/.exec("aacy"), "two suffix assertions");
            assert.areEqual("aacy", "aacy".replace(/(?<=x(?=c)c)y/g, "!"), "global replace no-op");
            assert.areEqual("aaxc!", "aaxcy".replace(/(?<=x(?=c)c)y/g, "!"), "genuine nested success replaces");
            var pos = /(?<=x(?=c)c)y/.exec("aaxcy");
            assert.areEqual("y", pos && pos[0], "genuine nested success matches");
            var nested = /(?<=(?<=a))b/.exec("ab");
            assert.areEqual("b", nested && nested[0], "nested lookbehind still matches");
            var lainlb = /(?<=a(?=b))b/.exec("ab");
            assert.areEqual("b", lainlb && lainlb[0], "lookahead-in-lookbehind still matches");
        }
    },
    {
        name: "Sticky, global, lastIndex interplay",
        body() {
            var re = /(?<=a)y/y;
            re.lastIndex = 1;
            assert.areEqual(null, re.exec("xay"), "sticky anchors at lastIndex");
            re.lastIndex = 2;
            assert.areEqual("y", re.exec("xay")[0], "sticky at the y");
            var g = /(?<=^(\w+))def/g;
            var r1 = g.exec("abcdefdef");
            assert.areEqual(["def", "abc"], [r1[0], r1[1]], "first exec");
            var r2 = g.exec("abcdefdef");
            assert.areEqual(["def", "abcdef"], [r2[0], r2[1]], "second exec sees grown left context");
            assert.areEqual(null, g.exec("abcdefdef"), "third exec exhausted");
            g.lastIndex = 0;
            assert.areEqual("abc[abc][abcdef]", "abcdefdef".replace(g, "[$1]"), "replace with lookbehind capture");
            assert.areEqual(["cd", "ef"], "abcdef".match(/(?<=[b-e])\w{2}/g), "global match stepping");
            assert.areEqual(true, /(?<=a)b/.test("ab"), "test");
            assert.areEqual(2, "xab".search(/(?<=a)b/), "search");
        }
    },
    {
        name: "Constructor, compile, and dynamic parity",
        body() {
            var dyn = new RegExp("(?<=\\d)x");
            assert.areEqual(3, "a12x".search(dyn), "dynamic constructor");
            dyn.compile("(?<!\\d)x");
            assert.areEqual(-1, "a12x".search(dyn), "recompiled negative");
            assert.areEqual("/(?<=a)b/", /(?<=a)b/.toString(), "toString round-trip");
            assert.areEqual("(?<=a)b", new RegExp("(?<=a)b").source, "dynamic source");
            var lit = /(?<=[ab])c/;
            var dyn2 = new RegExp(lit);
            assert.areEqual(2, "abc".search(dyn2), "regex-from-regex");
        }
    },
    {
        name: "Lookbehind scratch buffers survive collection",
        body() {
            var re = /(?<=(?<a>a+)|(?<b>b+))c/;
            for (var i = 0; i < 30; i++) {
                assert.areEqual("aa[aa]", "aac".replace(re, "[$<a>$<b>]"), "replace iteration " + i);
            }
            re = null;
            for (var j = 0; j < 40; j++) new RegExp("lb-gc-" + j + "(?<=x)");
            CollectGarbage();
            assert.areEqual(4, "zzabc".search(/(?<=[ab]{2})c/), "fresh pattern after GC");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
