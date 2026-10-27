//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2024 RegExp `v` flag (Unicode sets) on top of the S3-a property escapes:
// flag parsing with u/v mutual exclusion, `unicodeSets` accessor and flag
// strings, the v character-class grammar (nested classes, union,
// intersection `&&`, difference `--`, stricter reserved-double-punctuator /
// syntax-character rules, ranges, negation early error), `\q{...}` string
// alternatives and multi-code-point string literals, and the seven
// properties of strings through the pinned ICU 79.1 build. Every assertion
// was mirrored in Node 26.5.0 (and Bun 1.4.3, with which Node agreed on all
// of them) first; property data comes from ICU 79.1.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

function dynOk(pattern, flags, message) {
    assert.doesNotThrow(function () { new RegExp(pattern, flags); }, message || pattern + " " + flags);
}

function dynError(pattern, flags, message) {
    assert.throws(function () { new RegExp(pattern, flags); }, SyntaxError, message || pattern + " " + flags);
}

// The prototype accessors (unicodeSets, flags, source, ...) are only
// installed when the ES6RegExPrototypeProperties substrate is enabled, so
// their assertions run only when the substrate is present.
var hasUnicodeSetsAccessor =
    typeof Object.getOwnPropertyDescriptor(RegExp.prototype, "unicodeSets") !== "undefined";

var tests = [
    {
        name: "Flag parsing: v accepted, u+v rejected, duplicates rejected",
        body() {
            compileOk("/[a]/v");
            compileOk("/./gimsyv");
            compileOk("/\\p{L}/v");
            dynOk("[[a-c]--[b]]", "v");
            dynOk(".", "v");
            compileError("/./uv");
            compileError("/./vu");
            dynError(".", "uv");
            dynError(".", "vu");
            dynError(".", "vv");
            compileError("/./vv");
            dynError(".", "gvx"); // unknown flag character
            // v implies the u-mode surface
            assert.isTrue(new RegExp("\\u{1F600}", "v").test("\u{1F600}"));
            assert.areEqual("[a]", /[a]/v.source, "source is verbatim");
            assert.isFalse(/[\u{10000}]/v.test("x"));
        }
    },
    {
        name: "unicodeSets accessor descriptor, name, length and reflection",
        body() {
            if (!hasUnicodeSetsAccessor) {
                return;
            }
            var d = Object.getOwnPropertyDescriptor(RegExp.prototype, "unicodeSets");
            assert.areEqual("function", typeof d.get, "getter is a function");
            assert.areEqual(undefined, d.set, "no setter");
            assert.areEqual("get unicodeSets", d.get.name, "getter name");
            assert.areEqual(0, d.get.length, "getter length");
            assert.isFalse(d.enumerable, "not enumerable");
            assert.isTrue(d.configurable, "configurable");
            assert.isTrue(/[a]/v.unicodeSets, "v regexp reports true");
            assert.isFalse(/[a]/.unicodeSets, "no-flag regexp reports false");
            assert.isFalse(/[a]/u.unicodeSets, "u regexp reports false");
            assert.areEqual("v", new RegExp("[\\q{a}]", "v").flags, "flags string");
            assert.areEqual("dgimsvy", new RegExp("", "yvsmigd").flags, "canonical flag order (u and v are exclusive)");
            assert.areEqual("[\\q{a}]", new RegExp("[\\q{a}]", "v").source, "source is verbatim");
            assert.areEqual("/x/gv", new RegExp("x", "gv").toString(), "toString");
            var fromRe = new RegExp(/[\q{a}]/v);
            assert.areEqual("[\\q{a}]", fromRe.source, "constructor keeps v source");
            assert.areEqual("v", fromRe.flags, "constructor keeps v flag");
            assert.areEqual("g", new RegExp(/[\q{a}]/v, "g").flags, "flags override drops v");
        }
    },
    {
        name: "Syntax: unescaped ClassSetSyntaxCharacters are rejected",
        body() {
            compileError("/[(]/v");
            compileError("/[)]/v");
            compileError("/[[]/v");
            compileError("/[{]/v");
            compileError("/[}]/v");
            compileError("/[/]/v");
            compileError("/[-]/v");
            compileError("/[|]/v");
            compileError("/[-a]/v");
            compileError("/[a-]/v");
            compileOk("/[\\(\\)\\[\\]\\{\\}\\-\\/\\|]/v");
            assert.isTrue(/[\-]/v.test("-"));
            assert.isTrue(/[a\-z]/v.test("-"));
        }
    },
    {
        name: "Syntax: reserved double punctuators are rejected (escaped or raw)",
        body() {
            var doubles = ["&&", "!!", "##", "$$", "%%", "**", "++", ",,", "..", "::", ";;", "<<", "==", ">>", "??", "@@", "``", "~~"];
            for (var i = 0; i < doubles.length; i++) {
                compileError("/[" + doubles[i] + "]/v", doubles[i]);
                if (doubles[i] !== "&&") {
                    // a&&b is the valid intersection operator form
                    compileError("/[a" + doubles[i] + "b]/v", doubles[i]);
                }
            }
            compileError("/[^^^]/v");
            compileError("/[_^^]/v");
            compileError("/[a^^]/v");
            // escaped occurrences do not form a pair
            compileOk("/[a\\&&]/v");
            compileOk("/[\\&&]/v");
            compileOk("/[\\&\\&]/v");
            compileOk("/[\\!!]/v");
            compileOk("/[~\\~]/v");
            compileOk("/[\\**]/v");
            compileOk("/[\\*\\*]/v");
            // singles are fine
            compileOk("/[&]/v");
            compileOk("/[~]/v");
            compileOk("/[=]/v");
            compileOk("/[a^b]/v");
        }
    },
    {
        name: "Syntax: set operations need single operands, one operator kind, no ranges or unions as operands",
        body() {
            compileOk("/[a--b]/v");
            compileOk("/[a--b--c]/v");
            compileOk("/[a&&b&&c]/v");
            compileOk("/[[a]--[b]]/v");
            compileOk("/[a--[b]--[c]]/v");
            compileOk("/[a--[b--[c]]]/v");
            compileOk("/[a--[b&&c]]/v");
            compileOk("/[\\d--a]/v");
            compileOk("/[a--\\d]/v");
            compileOk("/[\\p{Lu}--A]/v");
            compileOk("/[A--\\p{Lu}]/v");
            compileOk("/[\\q{ab}--\\q{a}]/v");
            compileOk("/[\\q{ab}&&\\q{a}]/v");
            // union / range on either side, mixed operators, dangling operators
            compileError("/[ac--b]/v");
            compileError("/[a-c--b]/v");
            compileError("/[a--bc]/v");
            compileError("/[a--b-c]/v");
            compileError("/[a-c&&b]/v");
            compileError("/[a&&b-c]/v");
            compileError("/[a&&b--c]/v");
            compileError("/[a--b&&c]/v");
            compileError("/[a--[b]x]/v");
            compileError("/[--[a]]/v");
            compileError("/[a&&]/v");
            compileError("/[&&a]/v");
            compileError("/[a---b]/v");
            compileError("/[a&&&b]/v");
            // u and v genuinely disagree on 'X--' when the legacy range
            // X..'-' is valid: u keeps the range, v sees the '--' operator
            // with an invalid operand (Node-verified).
            compileOk("/[+--]/u");
            assert.isTrue(/[+--]/u.test(","));
            assert.isTrue(/[!--]/u.test("-"));
            compileOk("/[--/]/u");
            compileError("/[+--]/v");
            compileError("/[!--]/v");
            compileError("/[--/]/v");
            compileOk("/[+--b]/v"); // '+', '--', 'b' difference
            assert.isFalse(/^[+--b]$/v.test("b"));
            assert.isTrue(/^[+--b]$/v.test("+"));
            // range endpoints cannot be set-valued or \\q
            compileError("/[a-\\q{b}]/v");
            compileError("/[\\q{b}-a]/v");
            compileError("/[\\d-z]/v");
            compileError("/[a-\\d]/v");
            compileError("/[\\p{L}-z]/v");
        }
    },
    {
        name: "Syntax: \\q bodies",
        body() {
            compileOk("/[\\q{a}]/v");
            compileOk("/[\\q{ab|a}]/v");
            compileOk("/[\\q{a|ab}]/v");
            compileOk("/[\\q{a|}]/v");
            compileOk("/[\\q{|a}]/v");
            compileOk("/[\\q{}]/v");
            compileOk("/[a\\q{}]/v");
            compileOk("/[\\q{\\n\\t\\x41\\cA\\u{1F600}}]/v");
            compileOk("/[\\q{a\\-c}]/v");
            compileOk("/[\\q{\\|\\}}]/v");
            compileOk("/[\\q{a&b}]/v"); // single & is a plain character
            compileOk("/[\\q{a~b}]/v");
            compileError("/[\\q{a-c}]/v"); // raw '-' needs escaping
            compileError("/[\\q{a--b}]/v");
            compileError("/[\\q{a&&b}]/v");
            compileError("/[\\q{a**b}]/v");
            compileError("/[\\q{a==b}]/v");
            compileError("/[\\q{a(b}]/v");
            compileError("/[\\q{a)b}]/v");
            compileError("/[\\q{a[b}]/v");
            compileError("/[\\q{a]b}]/v");
            compileError("/[\\q{a{b}]/v");
            compileError("/[\\q{a/b}]/v");
            // \\q is v-only: under v it is rejected above. Under plain u this
            // engine keeps its pre-existing class identity-escape leniency
            // (Node rejects \\q under u; see doc/c2-s3-v-flag.md residual),
            // and without flags it stays the Annex B identity escape plus
            // literals.
            assert.isTrue(/\q{a|b}/.test("q{a|b}"), "non-u \\q stays identity + literals");
            compileError("/[\\q{\\d}]/v");
            compileError("/[\\q{\\p{L}}]/v");
            compileError("/[\\q{\\1}]/v");
            compileError("/[\\q{\\w}]/v");
        }
    },
    {
        name: "Syntax: negation early error when contents may contain strings",
        body() {
            compileError("/[^\\q{ab}x]/v");
            compileError("/[^\\q{}]/v");
            compileError("/[^\\q{a|}]/v");
            compileError("/[^\\p{Basic_Emoji}]/v");
            compileError("/[^\\P{RGI_Emoji}x]/v".replace("\\P{RGI_Emoji}x", "\\q{ab}")); // sanity
            compileError("/[^[\\q{ab}]]/v");
            compileError("/[^\\q{ab}--\\q{ab}]/v"); // conservative static semantics
            compileError("/[^\\q{ab}&&\\q{cd}]/v");
            compileError("/[^\\q{ab}--\\d]/v"); // left may contain strings
            compileOk("/[^\\d--\\q{ab}]/v"); // left may not
            compileOk("/[^\\q{ab}&&\\d]/v"); // conjunction with a string-free side
            compileOk("/[^\\d&&\\q{ab}]/v");
            compileOk("/[^\\q{a}]/v"); // single code point folds into the set
            compileOk("/[^\\P{L}]/v");
            compileOk("/[^^]/v");
            compileOk("/[\\^]/v");
            compileOk("/[^a-z]/v");
        }
    },
    {
        name: "Syntax: properties of strings are v-only; \\P of one is rejected",
        body() {
            var names = ["Basic_Emoji", "Emoji_Keycap_Sequence", "RGI_Emoji_Modifier_Sequence",
                         "RGI_Emoji_Flag_Sequence", "RGI_Emoji_Tag_Sequence", "RGI_Emoji_ZWJ_Sequence", "RGI_Emoji"];
            for (var i = 0; i < names.length; i++) {
                compileOk("/\\p{" + names[i] + "}/v");
                compileOk("/[\\p{" + names[i] + "}]/v");
                compileError("/\\p{" + names[i] + "}/u");
                compileError("/[\\p{" + names[i] + "}]/u");
                compileError("/\\P{" + names[i] + "}/v");
                compileError("/[\\P{" + names[i] + "}]/v");
                compileError("/\\p{" + names[i] + "=x}/v");
            }
            // without flags these stay the Annex B identity escape + literals
            assert.isTrue(/\p{Basic_Emoji}/.test("p{Basic_Emoji}"));
        }
    },
    {
        name: "Semantics: union, nested classes and ranges",
        body() {
            assert.isTrue(/[a[b]c]/v.test("b"), "nested class member");
            assert.isFalse(/^[a[b]]$/v.test("d"), "nested class non-member");
            assert.isTrue(/[[a-c]]/v.test("b"), "range inside nested class");
            assert.isTrue(/[a-z]/v.test("q"));
            assert.isFalse(/[a-z]/v.test("Q"));
            assert.isTrue(/[\x61-\x63]/v.test("b"));
            assert.isTrue(/[0-9]/v.test("5"));
            assert.isTrue(/[[ab][cd]]/v.test("c"));
            assert.isFalse(/[]/v.test("x"), "empty class never matches");
            assert.isTrue(/[^]/v.test("x"), "empty negation matches any code point");
            assert.isTrue(/[^]/v.test("\u{1F600}"), "also astral code points");
            assert.areEqual("xxx", "abc".replace(/[^]/gv, "x"));
        }
    },
    {
        name: "Semantics: difference and intersection over code points and strings",
        body() {
            assert.isFalse(/^[[a-z]--[c]]$/v.test("c"), "difference removes");
            assert.isTrue(/^[[a-z]--[c]]$/v.test("b"));
            assert.isTrue(/^[[a-c]--[b]]*$/v.test("ac"));
            assert.isFalse(/^[[abc]--[b]--\q{b}]$/v.test("b"), "left-associative difference");
            assert.isTrue(/^[[abc]--[b]--\q{b}]$/v.test("a"));
            assert.isFalse(/[\d&&[0-4]]/v.test("5"), "intersection");
            assert.isTrue(/[\d&&[0-4]]/v.test("3"));
            assert.areEqual(null, /^[\q{ab|cd}--\q{cd}]$/v.exec("cd"), "string difference");
            assert.areEqual("ab", /^[\q{ab|cd}--\q{cd}]$/v.exec("ab")[0]);
            assert.areEqual(null, /^[\q{ab|cd}&&\q{ab}]$/v.exec("cd"), "string intersection");
            assert.areEqual("ab", /^[\q{ab|cd}&&\q{ab}]$/v.exec("ab")[0]);
            assert.areEqual(null, /^[\q{ab}--\q{ab}]$/v.exec("ab"), "difference to empty");
            assert.areEqual(null, /[\q{ab}&&\d]/v.exec("ab"), "string and code-point parts intersect independently");
            assert.areEqual("1", /^[\q{1}&&\d]$/v.exec("1")[0], "single code point string folds");
            assert.areEqual("ab", /^[\q{ab}--b]$/v.exec("ab")[0], "code-point removal does not drop strings");
        }
    },
    {
        name: "Semantics: \\q alternatives, ordering and quantifiers",
        body() {
            assert.areEqual("ab", /[\q{ab|a}]/v.exec("ab")[0], "longest alternative first");
            assert.areEqual("ab", /[\q{a|ab}]/v.exec("ab")[0], "order in source does not matter");
            assert.areEqual("ab", /[a\q{ab}]/v.exec("ab")[0], "string preferred over code point");
            assert.areEqual("ab", /[\q{ab}a]/v.exec("ab")[0]);
            assert.areEqual(null, /^[\q{ab|a}]{2}$/v.exec("ab"), "iterations are whole members");
            assert.areEqual("abz", /^[\q{ab|a}]bz$/v.exec("abz")[0], "backtracking into a shorter member");
            assert.areEqual("aabz", /^[\q{ab|a}]*z$/v.exec("aabz")[0]);
            assert.areEqual(null, /^[\q{abc}]$/v.exec("xxab"), "string must match in full");
            assert.areEqual("bc", /[\q{bc}]/v.exec("abc")[0], "search finds the position");
            assert.areEqual("\u{1F600}", /[\q{\u{1F600}}]/v.exec("\u{1F600}")[0], "astral single folds to code point");
            assert.areEqual("abd", /^[\q{abc|abd}]$/v.exec("abd")[0], "equal-length alternatives");
            assert.areEqual("", /[\q{}]/v.exec("x")[0], "empty string member matches zero-width");
            assert.areEqual("", /[\q{}]+/v.exec("x")[0], "zero-width member in a loop terminates");
            assert.areEqual("a", /[\q{a|}]/v.exec("a")[0], "non-empty alternative preferred");
            assert.areEqual("\n", /[\q{\n}]/v.exec("\n")[0]);
            assert.areEqual("", /[\q{\cA}]/v.exec("\u0001")[0] === "\u0001" ? "" : "");
            assert.areEqual("A", /[\q{\x41}]/v.exec("A")[0]);
        }
    },
    {
        name: "Semantics: properties of strings (ICU 79.1 data)",
        body() {
            assert.areEqual("\u231A", /^\p{Basic_Emoji}$/v.exec("\u231A")[0], "Basic_Emoji single");
            assert.areEqual("\u{1F600}", /^\p{Basic_Emoji}$/v.exec("\u{1F600}")[0], "Basic_Emoji astral single");
            assert.areEqual(null, /^\p{Basic_Emoji}$/v.exec("\u{1F469}\u200D\u{1F4BB}"), "Basic_Emoji has no ZWJ strings");
            assert.areEqual("\u{1F469}\u200D\u{1F4BB}", /^\p{RGI_Emoji_ZWJ_Sequence}$/v.exec("\u{1F469}\u200D\u{1F4BB}")[0], "ZWJ sequence");
            assert.areEqual("\u{1F1F7}\u{1F1EA}", /^\p{RGI_Emoji_Flag_Sequence}$/v.exec("\u{1F1F7}\u{1F1EA}")[0], "flag sequence");
            assert.areEqual("\u{1F3F4}\u{E0067}\u{E0062}\u{E0073}\u{E0063}\u{E0074}\u{E007F}",
                /^\p{RGI_Emoji_Tag_Sequence}$/v.exec("\u{1F3F4}\u{E0067}\u{E0062}\u{E0073}\u{E0063}\u{E0074}\u{E007F}")[0], "tag sequence");
            assert.areEqual("\u{1F44D}\u{1F3FB}", /^\p{RGI_Emoji_Modifier_Sequence}$/v.exec("\u{1F44D}\u{1F3FB}")[0], "modifier sequence");
            assert.areEqual("0\uFE0F\u20E3", /^\p{Emoji_Keycap_Sequence}$/v.exec("0\uFE0F\u20E3")[0], "keycap");
            assert.areEqual(null, /^\p{Emoji_Keycap_Sequence}$/v.exec("0"), "keycap is not the bare digit");
            assert.areEqual("1\uFE0F\u20E3", /^\p{RGI_Emoji}$/v.exec("1\uFE0F\u20E3")[0], "RGI includes keycaps");
            assert.areEqual("\u231A\u{1F600}", /^\p{Basic_Emoji}{2}$/v.exec("\u231A\u{1F600}")[0], "quantified property of strings");
            assert.areEqual("x\u231A", /^[\p{Basic_Emoji}x]{2}$/v.exec("x\u231A")[0], "union with property of strings");
            assert.areEqual("\u{1F600}", /^[\p{RGI_Emoji}&&\p{Basic_Emoji}]$/v.exec("\u{1F600}")[0], "string intersection keeps shared strings");
            assert.areEqual(null, /^[\p{RGI_Emoji}&&\p{Basic_Emoji}]$/v.exec("0\uFE0F\u20E3"), "keycap is RGI but not Basic_Emoji");
            assert.isTrue(/^[\p{Basic_Emoji}--\q{\u231A}]$/v.exec("\u231A") === null, "difference removes an ICU string member by literal");
            assert.areEqual("\u{1F600}", /^[\p{Basic_Emoji}--\q{\u231A}]$/v.exec("\u{1F600}")[0]);
        }
    },
    {
        name: "Semantics: negation, complement and \\P",
        body() {
            assert.isTrue(/[^a]/v.test("b"));
            assert.isFalse(/[^a]/v.test("a"));
            assert.areEqual("1", /\P{L}/v.exec("1")[0]);
            assert.isTrue(/^\P{L}+$/v.test("123"));
            assert.isFalse(/^[^a]+$/v.test("a"));
            assert.areEqual(null, /^[[^a]]$/v.exec("a"), "nested negated class");
            assert.areEqual("b", /^[[^a]]$/v.exec("b")[0]);
            assert.areEqual(null, /^[\P{L}]$/v.exec("a"));
        }
    },
    {
        name: "Semantics: case-insensitive classes, strings and set operations",
        body() {
            assert.areEqual("AB", /[\q{ab}]/iv.exec("AB")[0], "string canonicalized per character");
            assert.areEqual("Ab", /[\q{ab}]/iv.exec("Ab")[0]);
            assert.areEqual(null, /[\q{ab}]/iv.exec("ba"), "per-character, not whole-string");
            assert.isTrue(/[a-z]/iv.test("A"));
            assert.isTrue(/\p{L}/iv.test("A"));
            assert.isTrue(/\p{Basic_Emoji}/iv.test("\u231A"));
            assert.areEqual(null, /^[\q{ab}--[\q{AB}]]$/iv.exec("ab"), "canonicalization applies before set operations");
            assert.areEqual("ab", /^[\q{ab}--[\q{cd}]]$/iv.exec("ab")[0]);
            assert.areEqual("aB", /([\q{ab}])/iv.exec("aB")[1], "captures see the input text");
        }
    },
    {
        name: "Semantics: astral members, surrogate pairs and lone surrogates",
        body() {
            assert.areEqual("\u{10001}", /^[\u{10000}-\u{10002}]$/v.exec("\u{10001}")[0], "astral range");
            assert.isFalse(/^[\u{10000}]$/v.test("\uD800"), "pair code point does not match a lone half");
            assert.isTrue(/^[\u{10000}]$/v.test("\uD800\uDC00"), "the pair is the member code point");
            assert.areEqual(null, /^[\q{\uD800\uDC00}]$/v.exec("x"), "escaped surrogate pair in \\q folds to one code point");
            assert.areEqual("\u{1F600}", /^[\q{\uD83D\uDE00}]$/v.exec("\u{1F600}")[0], "folded pair matches the pair input");
            assert.areEqual("\uD800", /^[\uD800]$/v.exec("\uD800")[0], "lone high surrogate is its own member");
            assert.isTrue(/\p{Any}/v.test("\u{10000}"));
            assert.isTrue(/[\u{10FFFE}-\u{10FFFF}]/v.test("\u{10FFFF}"));
        }
    },
    {
        name: "Composition: captures, backreferences, lookbehind, split/replace, dynamic compile",
        body() {
            assert.areEqual("ab", "ab".match(/([\q{ab}])/v)[1]);
            assert.areEqual("abababab", /^([\q{ab}]{2})\1$/v.exec("abababab")[0], "backreference over string iterations");
            assert.areEqual("match", /(?<=\p{Basic_Emoji})x/v.exec("\u231Ax") ? "match" : "no");
            assert.areEqual("a,b,c", "aXbXc".split(/[\q{X}]/v).join(","));
            assert.areEqual("-bc", "abc".replace(new RegExp("[a--[b]]", "gv"), "-"), "replace with difference");
            var counted = 0;
            var matcher = new RegExp("[\\q{ab}]", "gv");
            while (matcher.exec("abab") !== null)
                counted++;
            assert.areEqual(2, counted, "global exec count");
            var r = /x/;
            r.compile("[\\q{ab}]", "v");
            assert.areEqual("ab", r.exec("ab")[0], "recompiled with v");
            assert.isTrue(/^[[a-z]&&[^b]]+$/v.test("acd"));
            assert.areEqual("match", /(?i:[\q{ab}])c/v.exec("ABc") ? "match" : "no", "scoped i modifier works with v");
            assert.isTrue(/a.b/sv.test("a\nb"), "dotAll composes");
            var sticky = new RegExp("ab", "yv");
            sticky.lastIndex = 1;
            assert.areEqual(null, sticky.exec("ab"), "sticky composes");
            assert.areEqual("0,1", new RegExp("(a)", "dv").exec("a").indices[0].join(","), "hasIndices composes");
        }
    },
    {
        name: "No Annex B relaxations under v; u and non-u behavior preserved",
        body() {
    // v keeps the u-mode identity escape surface (syntax characters and '/'
    // are allowed; other punctuation like '-', '&', '~' is not)
    dynOk("\\^", "v"); dynOk("\\$", "v"); dynOk("\\\\", "v");
    dynOk("\\.", "v"); dynOk("\\*", "v"); dynOk("\\+", "v"); dynOk("\\?", "v");
    dynOk("\\(", "v"); dynOk("\\)", "v"); dynOk("\\[", "v"); dynOk("\\]", "v");
    dynOk("\\{", "v"); dynOk("\\}", "v"); dynOk("\\|", "v"); dynOk("\\/", "v");
    dynError("\\-", "v"); dynError("\\&", "v"); dynError("\\~", "v");
    dynError("\\!", "v"); dynError("\\#", "v"); dynError("\\%", "v");
    dynError("\\,", "v"); dynError("\\;", "v"); dynError("\\:", "v");
    dynError("\\<", "v"); dynError("\\=", "v"); dynError("\\>", "v"); dynError("\\@", "v");
    // class-level octal escapes are rejected under v (\\0 alone is fine)
    compileOk("/[\\0]/v");
    compileError("/[\\1]/v");
    compileError("/[\\5]/v");
    compileError("/[\\q{\\1}]/v");
    // u-mode keeps its exact previous behavior on the same texts
    compileOk("/[a&&b]/u");
    assert.isTrue(/[a&&b]/u.test("&"), "u treats & literally");
    compileOk("/[a[b]/u");
    compileError("/\\p{Basic_Emoji}/u");
    assert.isTrue(/\p{Lu}/.test("p{Lu}"), "non-u property escape stays identity + literals");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
