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

function exec0(re, input) {
    var m = re.exec(input);
    return m === null ? null : m[0];
}

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
    {
        name: "Repair: negated shorthand classes terminate and complement correctly",
        body() {
            assert.isTrue(/[\S]/v.test("x"));
            assert.isFalse(/[\S]/v.test(" "));
            assert.isTrue(/[^\S]/v.test(" "));
            assert.isFalse(/[^\S]/v.test("x"));
            assert.isTrue(/[\D]/v.test("x"));
            assert.isFalse(/[\D]/v.test("5"));
            assert.isTrue(/[\W]/v.test("~"), "~ is not a word char");
            assert.isFalse(/[\W]/v.test("_"));
            assert.isFalse(/[\W]/v.test("a"));
            assert.isFalse(/[\S\d]/v.test(" "), "union with negated shorthand");
            assert.isTrue(/[\S\d]/v.test("5"));
            assert.isFalse(/[\S--x]/v.test(" "), "difference with negated shorthand");
            assert.isTrue(/[\S--x]/v.test("y"));
            assert.isFalse(/[\S&&\d]/v.test("x"), "intersection with negated shorthand");
            assert.isTrue(/[\S&&\d]/v.test("5"));
            assert.isTrue(/[\S]/iv.test("X"), "i variant");
            assert.isTrue(/[^\W]/v.test("a"), "double negation through union");
            assert.areEqual("x x x", "a b c".replace(/\S/gv, "x"), "global replace over non-space runs");
        }
    },
    {
        name: "Repair: character escapes are valid -- right operands",
        body() {
            compileOk("/[a--\\t]/v");
            assert.isFalse(/[a--\t]/v.test("\t"), "removed member");
            assert.isTrue(/[a--\t]/v.test("a"), "kept member");
            compileOk("/[\\s--\\n]/v");
            assert.isFalse(/[\s--\n]/v.test("\n"));
            assert.isTrue(/[\s--\n]/v.test(" "));
            compileOk("/[a--\\0]/v");
            compileOk("/[a--\\x09]/v");
            assert.isFalse(/[a--\x09]/v.test("\t"));
            compileOk("/[a--\\u0009]/v");
            assert.isFalse(/[a--\u0009]/v.test("\t"));
            compileOk("/[a--\\u{9}]/v");
            assert.isFalse(/[a--\u{9}]/v.test("\t"));
            compileOk("/[a--\\b]/v");
            assert.isFalse(/[a--\b]/v.test("\b" === "b" ? "b" : "\u0008"));
            compileOk("/[\\q{a}--\\t]/v");
            assert.isFalse(/[\q{a}--\t]/v.test("\t"));
            assert.isTrue(/[\q{a}--\t]/v.test("a"));
            // set-valued and range left operands keep the rejection
            compileError("/[a-c--\\t]/v");
            compileError("/[\\d--a-c]/v");
            // legacy shapes stay preserved without v
            compileError("/[a--\\t]/");
            compileOk("/[--a]/");
            assert.isTrue(/[--a]/.test("."));
        }
    },
    {
        name: "Repair: escaped surrogate pairs form astral ranges and endpoints",
        body() {
            assert.areEqual("\u{1F600}", /^[\uD83D\uDE00-\uD83D\uDE01]$/v.exec("\u{1F600}")[0]);
            assert.areEqual("\u{1F601}", /^[\uD83D\uDE00-\uD83D\uDE01]$/v.exec("\u{1F601}")[0]);
            assert.areEqual(null, /^[\uD83D\uDE00-\uD83D\uDE01]$/v.exec("\u{1F602}"), "above the range");
            assert.areEqual(null, /^[\uD83D\uDE00-\uD83D\uDE01]$/v.exec("\u{1F5FF}"), "below the range");
            assert.areEqual(null, /^[\uD83D\uDE00]$/v.exec("\u{1F601}"));
            assert.areEqual("\u{1F601}", /^[\uD83D\uDE01-\uD83D\uDE02]$/v.exec("\u{1F601}")[0]);
            // set operations over astral ranges
                        assert.areEqual("\u{1F601}", /^[[\uD83D\uDE00-\uD83D\uDE01]--[\u{1F600}]]$/v.exec("\u{1F601}")[0]);
            assert.isTrue(/[[\uD83D\uDE00-\uD83D\uDE01]&&[\u{1F600}]]/v.test("\u{1F600}"), "intersection keeps");
            assert.isFalse(/[[\uD83D\uDE00-\uD83D\uDE01]&&[\u{1F601}]]/v.test("\u{1F600}"));
            // range-order rejection with pairs still works
            compileError("/[\\uD83D\\uDE01-\\uD83D\\uDE00]/v");
            // pairs in \q bodies still fold
            assert.areEqual("\u{1F600}", /^[\q{\uD83D\uDE00}]$/v.exec("\u{1F600}")[0]);
        }
    },
    {
        name: "Repair: spec Canonicalize for \\q string members under i (Kelvin, long s)",
        body() {
            // U+212A KELVIN SIGN folds to 'k' but canonicalizes to itself:
            // a \q member 'k' matches k/K and never U+212A (spec; Node agrees
            // on the Kelvin outcome).
            assert.areEqual("k", exec0(/[\q{k}]/iv, "k"));
            assert.areEqual("K", exec0(/[\q{k}]/iv, "K"), "symmetric canonicalization (spec; V8 quirk returns null)");
            assert.areEqual("\u212A", /[\q{k}]/iv.exec("\u212A")[0], "Kelvin folds onto k (spec scf; V8 null)");
            assert.areEqual("k", exec0(/[\q{K}]/iv, "k"));
            assert.areEqual("K", exec0(/[\q{K}]/iv, "K"));
            // U+017F LATIN SMALL LETTER LONG S folds to 's' but canonicalizes
            // to itself, so it matches only itself in a \q member. (The K/K
            // and long-s self-match outcomes follow the spec's symmetric
            // Canonicalize; Node 26.5.0/V8 quirk-diverges to null there —
            // Bun 1.4.3 and this engine implement the spec — see the
            // receipt's oracle divergence notes.)
            assert.areEqual("S", exec0(/[\q{s}]/iv, "S"));
            assert.areEqual("\u017F", /[\q{s}]/iv.exec("\u017F")[0], "long s folds onto s (spec scf)");
            assert.areEqual("s", /[\q{\u017F}]/iv.exec("s")[0]);
            assert.areEqual("S", /[\q{\u017F}]/iv.exec("S")[0]);
            assert.areEqual("\u017F", exec0(/[\q{\u017F}]/iv, "\u017F"));
            // multi-char members canonicalize per character
            assert.areEqual("Kk", exec0(/[\q{kk}]/iv, "Kk"));
            assert.areEqual("AB", exec0(/[\q{ab}]/iv, "AB"));
            assert.areEqual("\u017Fs", /[\q{ss}]/iv.exec("\u017Fs")[0], "long s folds inside multi-char members (spec scf; V8 agrees)");
            // ordinary class chars keep the engine's fold closure
            assert.areEqual("\u212A", /[k]/iv.exec("\u212A")[0]);
            assert.areEqual("K", /[k-k]/iv.exec("K")[0]);
            assert.areEqual("\u212A", /[k-k]/iv.exec("\u212A")[0]);
        }
    },
    {
        name: "Repair: operands are case-closed before && and --",
        body() {
            assert.areEqual(null, /[k--[k-k]]/iv.exec("k"), "fold-closed both sides");
            assert.areEqual(null, /[k--[k-k]]/iv.exec("K"));
            assert.areEqual(null, /[k--[k-k]]/iv.exec("\u212A"));
            assert.areEqual(null, /[\q{k}--\p{Ll}]/iv.exec("k"), "canonical member minus closed property");
            assert.areEqual(null, /[\q{k}--\p{Ll}]/iv.exec("K"));
            assert.areEqual(null, /[\q{k}--\p{Ll}]/iv.exec("\u212A"));
            assert.areEqual(null, /[\q{k}--[k-k]]/iv.exec("k"), "canonical member minus closed range");
            assert.areEqual(null, /[\q{k}--[k-k]]/iv.exec("K"));
            assert.areEqual(null, /[\q{k}--[k-k]]/iv.exec("\u212A"));
            assert.areEqual("k", /[\q{k}--[\u{17F}-\u{17F}]]/iv.exec("k")[0], "long-s range removes nothing from k");
            assert.areEqual("\u212A", /[\q{k}--[\u{17F}-\u{17F}]]/iv.exec("\u212A")[0],
                "Kelvin survives: the scf member class {k,K,U+212A} minus {s,S,U+017F} keeps it (spec; Node null via its asymmetric single-char members)");
            assert.areEqual(null, /[[k]--[k]]/iv.exec("K"), "nested class operands closed too");
            assert.areEqual(null, /[[k]--[k]]/iv.exec("k"));
            // singleton-vs-singleton and no-i behavior unchanged
            assert.areEqual(null, /[k--k]/iv.exec("K"));
            assert.areEqual(null, /[k--k]/iv.exec("k"));
            assert.areEqual(null, /[k--k]/v.exec("k"));
            assert.areEqual(null, /^[\q{ab}--\q{AB}]$/iv.exec("ab"), "canonicalization applies before string set ops");
            // union still closes over the whole set
            assert.isTrue(/[kx]/iv.test("K"));
            assert.isTrue(/[kx]/iv.test("X"));
        }
    },
    {
        name: "Repair: truncated classes and deep nesting fail cleanly",
        body() {
            dynError("[[a]--", "v");
            dynError("[[a]&&", "v");
            dynError("[[a]--\\", "v");
            dynError("[[a]--x", "v");
            dynError("[[", "v");
            dynError("[\\q{ab", "v");
            dynError("[a--", "v");
            dynError("[\\uD83D\\uDE0", "v");
            dynError("[x\\", "v");
            dynError("[", "v");
            // Deep nesting must never crash: within the profile's stack
            // budget the pattern compiles; beyond it the parser's stack
            // probe raises a clean out-of-stack error. Measured bounds
            // (this test): the test profile accepts >= 8000 levels, the
            // debug-static profile accepts 3000 and reports a clean error
            // at 4000 (larger debug frames). Node accepts 4000 and rejects
            // at 16000; Bun accepts 16000.
            var deep2000 = new Array(2001).join("[") + "a" + new Array(2001).join("]");
            assert.doesNotThrow(function () { new RegExp(deep2000, "v"); }, "N=2000 nesting both profiles");
            var deep4000 = new Array(4001).join("[") + "a" + new Array(4001).join("]");
            var deepResult = "no-throw";
            try { new RegExp(deep4000, "v"); } catch (e) { deepResult = e.name + " | " + e.message; }
            assert.isTrue(deepResult === "no-throw" || /stack/i.test(deepResult),
                "N=4000 either compiles or fails with a clean stack error, got " + deepResult);
        }
    },
    {
        name: "Repair 2: astral set-operation operands in every spelling",
        body() {
            // escaped surrogate pairs
            assert.areEqual("\u{1F600}", /^[\uD83D\uDE00--x]$/v.exec("\u{1F600}")[0]);
            assert.areEqual(null, /^[\uD83D\uDE00--x]$/v.exec("x"));
            assert.areEqual(null, /^[\uD83D\uDE00&&x]$/v.exec("\u{1F600}"), "intersection with disjoint cp");
            assert.areEqual(null, /^[\uD83D\uDE00&&x]$/v.exec("x"), "intersection is empty, no x either");
            assert.areEqual("x", /^[x--\uD83D\uDE00]$/v.exec("x")[0]);
            assert.areEqual(null, /^[x--\uD83D\uDE00]$/v.exec("\u{1F600}"));
            assert.areEqual("\u{1F600}", /^[\uD83D\uDE00--\uD83D\uDE01]$/v.exec("\u{1F600}")[0], "pair-pair subtraction keeps the lower");
            assert.areEqual(null, /^[\uD83D\uDE00--\uD83D\uDE01]$/v.exec("\u{1F601}"));
            assert.areEqual(null, /^[\p{Basic_Emoji}--\uD83D\uDE00]$/v.exec("\u{1F600}"), "property minus astral member");
            assert.areEqual("\u{1F601}", /^[\p{Basic_Emoji}--\uD83D\uDE00]$/v.exec("\u{1F601}")[0]);
            // raw astral characters (literal source); single dash is the range
            assert.isTrue(/[\u{1F600}-\u{1F601}]/v.test("\u{1F601}"));
            assert.isFalse(/[\u{1F600}-\u{1F601}]/v.test("\u{1F602}"));
            // consistency with \u{...} spelling
            assert.areEqual("\u{1F600}", /^[\u{1F600}--x]$/v.exec("\u{1F600}")[0]);
            assert.areEqual(null, /^[\u{1F600}&&x]$/v.exec("\u{1F600}"));
        }
    },
    {
        name: "Repair 2: canonical literal runs never mis-fuse",
        body() {
            assert.areEqual("yka", /y[\q{ka}]/iv.exec("yka")[0], "prefix ordinary + canonical member");
            assert.areEqual("yKA", /y[\q{ka}]/iv.exec("yKA")[0]);
            assert.areEqual("kay", /[\q{ka}]y/iv.exec("kay")[0], "canonical member + suffix");
            assert.areEqual("KAy", /[\q{ka}]y/iv.exec("KAy")[0]);
            assert.areEqual("\u212AAy", /[\q{ka}]y/iv.exec("\u212AAy")[0], "member folds the Kelvin sign onto k (spec scf)");
            assert.areEqual("yKAz", /(y[\q{ka}])z/iv.exec("yKAz")[0], "capture around the boundary");
            assert.areEqual("yKA", /x|y[\q{ka}]/iv.exec("yKA")[0], "alternative branch");
            assert.areEqual("yKAkaz", /y[\q{ka}]+z/iv.exec("yKAkaz")[0], "quantified across the boundary");
            assert.areEqual("yKak", /y[\q{ka}]k/iv.exec("yKak")[0], "member then ordinary char then member char");
            assert.areEqual("\u212Ax", /[\q{k}]x/iv.exec("\u212Ax")[0], "member folds the Kelvin sign onto k (spec scf)");
            // multi-member canonical runs stay separate per member
            assert.areEqual("ab", /^[\q{ab}b]$/iv.exec("ab")[0]);
            assert.areEqual("b", /^[\q{ab}b]$/iv.exec("b")[0]);
            assert.areEqual("AB", /^[\q{ab}b]$/iv.exec("AB")[0]);
        }
    },
    {
        name: "Repair 2: every multi-code-point \q member matches canonically",
        body() {
            assert.areEqual("ab", /^[\q{ab|cd}]$/iv.exec("ab")[0]);
            assert.areEqual("AB", /^[\q{ab|cd}]$/iv.exec("AB")[0]);
            assert.areEqual("cd", /^[\q{ab|cd}]$/iv.exec("cd")[0]);
            assert.areEqual("CD", /^[\q{ab|cd}]$/iv.exec("CD")[0]);
            assert.areEqual("aB", /^[\q{ab|cd}]$/iv.exec("aB")[0], "per-character canonicalization (Node-verified)");
            assert.areEqual("ABCD", /^[\q{ab|cd}]+$/iv.exec("ABCD")[0], "quantified alternation of canonical members");
            assert.areEqual("aB", /[\q{ab|cd}]+/iv.exec("aBdC")[0], "dC is not a canonical cd (Node-verified)");
            assert.areEqual(null, /^[\q{ab|cd}]+$/iv.exec("aBdC"));
            // union with a code point arm and nested classes
            assert.areEqual("AB", /^[\q{ab}x]$/iv.exec("AB")[0]);
            assert.areEqual("x", /^[\q{ab}x]$/iv.exec("x")[0]);
            assert.areEqual("CD", /^[[\q{cd}]]$/iv.exec("CD")[0], "inside a nested class");
            assert.areEqual("AbCd", /^[\q{ab}\q{cd}]{2}$/iv.exec("AbCd")[0], "repeated members");
            assert.areEqual("AB", /^[\q{ab}\q{ab}]$/iv.exec("AB")[0], "duplicate members deduped but still canonical");
        }
    },
    {
        name: "Repair 2: truncated nested classes throw under deferParse too",
        body() {
            dynError("[[a]--", "v");
            dynError("[[a]", "v");
            dynError("[[a]&&", "v");
            dynError("[[a]--x", "v");
            dynError("[[", "v");
            dynError("[\q{ab", "v");
            // same shapes inside a deferred function literal
            assert.throws(function () { eval("function d1() { return /[[a]--/v; }"); }, SyntaxError, "deferred truncated operator");
            assert.throws(function () { eval("function d2() { return /[[a]/v; }"); }, SyntaxError, "deferred truncated nested");
        }
    },
    {
        name: "Repair 2: astral complement shorthands equal their positive form",
        body() {
            assert.areEqual(null, /[^\D]/v.exec("\u{1F600}"), "[^\\D] is \\d, no astral");
            assert.areEqual(null, /[^\S]/v.exec("\u{1F600}"), "[^\\S] is \\s, no astral");
            assert.areEqual(null, /[^\W]/v.exec("\u{1F600}"), "[^\\W] is \\w, no astral");
            assert.areEqual("5", /^[^\D]$/v.exec("5")[0]);
            assert.areEqual(" ", /^[^\S]$/v.exec(" ")[0]);
            assert.areEqual("_", /^[^\W]$/v.exec("_")[0]);
            assert.areEqual(null, /[^\D]/u.exec("\u{1F600}"), "u mode agrees");
            assert.areEqual(null, /[^\S]/u.exec("\u{1F600}"));
            assert.areEqual(null, /[^\W]/u.exec("\u{1F600}"));
            assert.isTrue(/[a\D]/u.test("\u{1F600}"), "union with \\D still matches astral");
            assert.isTrue(/[\D]/v.test("\u{1F600}"), "positive \\D matches astral");
            assert.isTrue(/[\S]/v.test("\u{1F600}"));
        }
    },
    {
        name: "Repair 2: strict v empty/reserved-punctuator classes",
        body() {
            // [] and [^] stay legal (never-match / any-code-point)
            assert.areEqual(null, /[]/v.exec("x"));
            assert.isTrue(/[^]/v.test("x"));
            // ...but the shapes whose class closes early and leaves a bare
            // ']' atom, or a reserved triple operator, are SyntaxErrors
            dynError("[]--a]", "v");
            dynError("[]a]", "v");
            dynError("[^]--a]", "v");
            dynError("[]]", "v");
            dynError("[a&&&]", "v");
            dynError("[a---b]", "v");
            dynError("[a--&&]", "v");
            dynError("[a&&-]", "v");
            // legal operator chains stay legal
            dynOk("[a--b--c]", "v");
            dynOk("[a&&b&&c]", "v");
            dynOk("[a--&]", "v");
            // legacy non-v [] and ]-atom leniency preserved: the shape
            // parses and the empty class matches nothing (Node-verified)
            dynOk("[]", "");
            dynOk("[]--a]", "");
            assert.isFalse(/[]--a]/.test("--a]"), "legacy empty class matches nothing");
        }
    },
    {
        name: "Repair 3: u-mode sole complement shorthand with siblings",
        body() {
            // sole forms still flip to the positive set
            assert.areEqual("5", /^[^\D]$/u.exec("5")[0]);
            assert.areEqual(null, /^[^\D]$/u.exec("\u{1F600}"), "no astral leak, sole");
            assert.areEqual("a", /^[^\W]$/u.exec("a")[0]);
            // any sibling cancels the flip; the astral extension keeps the
            // complement exact (Node-verified expectations)
            assert.areEqual(null, /^[^\W_]$/u.exec("_"));
            assert.areEqual("a", /^[^\W_]$/u.exec("a")[0]);
            assert.areEqual(null, /^[^\W_]$/u.exec("\u{1F600}"));
            assert.areEqual(null, /^[^\D5]$/u.exec("5"));
            assert.areEqual(null, /^[^\D5]$/u.exec("x"));
            assert.areEqual(null, /^[^\D5]$/u.exec("\u{1F600}"));
            assert.areEqual(null, /^[^5\D]$/u.exec("5"), "sibling before, both orders");
            assert.areEqual(null, /^[^5\D]$/u.exec("x"));
            assert.areEqual(null, /^[^\S ]$/u.exec(" "));
            assert.areEqual(null, /^[^\S ]$/u.exec("x"));
            assert.areEqual("_", /^[^\W\d]$/u.exec("_")[0]);
            assert.areEqual(null, /^[^\W\d]$/u.exec("5"));
            assert.areEqual("a", /^[^\W\d]$/u.exec("a")[0]);
            assert.areEqual("_", /^[^\W0-9]$/u.exec("_")[0]);
            assert.areEqual(null, /^[^\W0-9]$/u.exec("5"));
            assert.areEqual(null, /^[^\D\d]$/u.exec("5"));
            assert.areEqual(null, /^[^\D\d]$/u.exec("x"));
            // i and v variants agree on the astral boundary
            assert.areEqual(null, /^[^\D]$/iu.exec("\u{1F600}"));
            assert.areEqual(null, /^[^\W_]$/iv.exec("\u{1F600}"));
            assert.areEqual("5", /^[^\D]$/v.exec("5")[0]);
            // positive unions keep astral members
            assert.isTrue(/[a\D]/u.test("\u{1F600}"));
            assert.isTrue(/[\D]/v.test("\u{1F600}"));
            // double-negation corner (all-BMP complement) is fixed
            assert.areEqual(null, /^[^\u{10000}-\u{10FFFF}]$/u.exec("\u{1F600}"));
            assert.areEqual("a", /^[^\u{10000}-\u{10FFFF}]$/u.exec("a")[0]);
        }
    },
    {
        name: "Repair 3: raw astral literal set-operation operands",
        body() {
            // literal source: the astral characters are raw (not \u escapes)
            assert.isTrue(/[😀--x]/v.test("\u{1F600}"));
            assert.isFalse(/[😀--x]/v.test("x"));
            assert.isFalse(/[😀&&x]/v.test("\u{1F600}"), "intersection with disjoint cp");
            assert.isTrue(/[x--😀]/v.test("x"));
            assert.isFalse(/[x--😀]/v.test("\u{1F600}"));
            assert.isTrue(/[😀--😁]/v.test("\u{1F600}"), "pair-pair subtraction");
            assert.isFalse(/[😀--😁]/v.test("\u{1F601}"));
            assert.isFalse(/^[\p{Basic_Emoji}--😀]$/v.test("\u{1F600}"), "property minus raw astral member");
            assert.areEqual("\u{1F601}", /^[\p{Basic_Emoji}--😀]$/v.exec("\u{1F601}")[0]);
            assert.isTrue(/[\q{x}--😀]/v.test("x"), "\q left operand minus raw astral");
            assert.isFalse(/[\q{x}--😀]/v.test("\u{1F600}"));
            // \u escape spellings stay equivalent
            assert.isTrue(/[\uD83D\uDE00--x]/v.test("\u{1F600}"));
            assert.isTrue(/[\u{1F600}--x]/v.test("\u{1F600}"));
        }
    },
    {
        name: "Repair 3: bare ']' atoms reject outside the v class span",
        body() {
            // false-accept family (sticky-flag over-exemption) now rejects
            dynError("[[a]]|]", "v");
            dynError("[[a]]*]", "v");
            dynError("[a[b]]|]", "v");
            dynError("[[a]]|x]", "v");
            dynError("[[ab][cd]]|]", "v");
            // valid nested/\q shapes stay accepted
            dynOk("[a[b]c]", "v");
            dynOk("[[ab][cd]]", "v");
            dynOk("[\\q{a|b}]", "v");
            dynOk("[\\q{|}]", "v");
            dynOk("[[0-9]\\q{0|2}]", "v");
            dynOk("[]x[a[b]]", "v");
            assert.isTrue(/[a[b]c]/v.test("c"), "nested content still matches");
            assert.isTrue(new RegExp("[\\q{a|b}]", "v").test("a"));
        }
    },
    {
        name: "Repair 3: simple case folding for \q string members under i",
        body() {
            // spec scf: U+212A folds onto 'k', U+017F onto 's' (ES2027
            // MaybeSimpleCaseFolding). Node 26.5.0/V8 diverges on
            // single-character members ([\q{k}] misses K/U+212A); this
            // engine implements the spec symmetrically.
            assert.areEqual("\u212Aa", /^[\q{ka}]$/iv.exec("\u212Aa")[0], "Kelvin folds onto k (spec)");
            assert.areEqual("\u017Fs", /^[\q{ss}]$/iv.exec("\u017Fs")[0], "long s folds onto s (spec)");
            assert.areEqual("Ss", /^[\q{ss}]$/iv.exec("Ss")[0]);
            assert.areEqual(null, /^[\q{ss}]$/iv.exec("\u00DF"), "simple folding does not expand ss");
            assert.areEqual("K", /[\q{k}]/iv.exec("K")[0], "single-char member matches K (spec; V8 null)");
            assert.areEqual("\u212A", /[\q{k}]/iv.exec("\u212A")[0], "single-char member matches Kelvin (spec; V8 null)");
            assert.areEqual("k", /[\q{k}]/iv.exec("k")[0]);
            assert.areEqual("aB", /[\q{ab}]/iv.exec("aB")[0]);
            assert.areEqual("s", /[\q{\u017F}]/iv.exec("s")[0], "long-s member matches s (spec)");
            assert.areEqual("\u017F", /[\q{\u017F}]/iv.exec("\u017F")[0]);
            assert.areEqual("\u212Aa", /^[\q{ka|cd}]+$/iv.exec("\u212Aa")[0], "folding inside alternation members");
            assert.areEqual("ab", /[\q{ab}]/v.exec("ab")[0], "no i: exact");
            assert.areEqual(null, /[\q{ab}]/iv.exec("ba"), "order still matters");
        }
    },
    {
        name: "Speculative v-scan is bounded at the literal terminator",
        body() {
            // The trailing comment is part of the pass-0 speculative scan
            // window; it must not be parsed as part of the literal.
            var trailing = /[[]/u; // trailing comment with \p{Nope}
            assert.areEqual(true, trailing.test("["), "legacy class literal with trailing source");

            compileOk("/[[]/u");
            compileOk("/[[]/");
            compileError("/[\\p{Nope}]/u");

            var nestedLegacy = /[a[b]c]/; // nested '[' is literal in a legacy class
            assert.areEqual(false, nestedLegacy.test("b"), "legacy nested class stays literal");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
