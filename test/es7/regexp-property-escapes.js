//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2018 Unicode property escapes \p{...}/\P{...} under the u flag
// (doc/c2-s3-property-escapes.md). Names match exactly and case-sensitively
// (canonical names and aliases only; ES2024 removed loose matching); the
// non-binary forms are General_Category/gc, Script/sc and Script_Extensions/
// scx; Any/Assigned/ASCII are the three RegExp-specific properties. Without
// the u flag \p/\P stay the Annex B identity escapes, and the '{...}' part
// re-parses as literal characters (existing behavior). The v flag and
// property-of-strings stay out of this slice. Every assertion was mirrored
// in Node 26.5.0 first. Property data comes from the pinned ICU 79.1.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

var tests = [
    {
        name: "Syntax: binary property names and aliases accepted",
        body() {
            compileOk("/\\p{Alphabetic}/u");
            compileOk("/\\p{Alpha}/u");
            compileOk("/\\p{ASCII_Hex_Digit}/u");
            compileOk("/\\p{AHex}/u");
            compileOk("/\\p{Any}/u");
            compileOk("/\\p{Assigned}/u");
            compileOk("/\\p{ASCII}/u");
            compileOk("/\\p{Cased}/u");
            compileOk("/\\p{Case_Ignorable}/u");
            compileOk("/\\p{CI}/u");
            compileOk("/\\p{Changes_When_Lowercased}/u");
            compileOk("/\\p{CWL}/u");
            compileOk("/\\p{Dash}/u");
            compileOk("/\\p{Default_Ignorable_Code_Point}/u");
            compileOk("/\\p{DI}/u");
            compileOk("/\\p{Deprecated}/u");
            compileOk("/\\p{Dep}/u");
            compileOk("/\\p{Emoji}/u");
            compileOk("/\\p{Emoji_Component}/u");
            compileOk("/\\p{EComp}/u");
            compileOk("/\\p{Emoji_Modifier}/u");
            compileOk("/\\p{EMod}/u");
            compileOk("/\\p{Emoji_Modifier_Base}/u");
            compileOk("/\\p{EBase}/u");
            compileOk("/\\p{Emoji_Presentation}/u");
            compileOk("/\\p{EPres}/u");
            compileOk("/\\p{Extended_Pictographic}/u");
            compileOk("/\\p{ExtPict}/u");
            compileOk("/\\p{Extender}/u");
            compileOk("/\\p{Ext}/u");
            compileOk("/\\p{Grapheme_Base}/u");
            compileOk("/\\p{Gr_Base}/u");
            compileOk("/\\p{Grapheme_Extend}/u");
            compileOk("/\\p{Gr_Ext}/u");
            compileOk("/\\p{Hex_Digit}/u");
            compileOk("/\\p{Hex}/u");
            compileOk("/\\p{IDS_Binary_Operator}/u");
            compileOk("/\\p{IDSB}/u");
            compileOk("/\\p{IDS_Trinary_Operator}/u");
            compileOk("/\\p{IDST}/u");
            compileOk("/\\p{ID_Continue}/u");
            compileOk("/\\p{IDC}/u");
            compileOk("/\\p{ID_Start}/u");
            compileOk("/\\p{IDS}/u");
            compileOk("/\\p{Ideographic}/u");
            compileOk("/\\p{Ideo}/u");
            compileOk("/\\p{Join_Control}/u");
            compileOk("/\\p{Join_C}/u");
            compileOk("/\\p{Logical_Order_Exception}/u");
            compileOk("/\\p{LOE}/u");
            compileOk("/\\p{Lowercase}/u");
            compileOk("/\\p{Lower}/u");
            compileOk("/\\p{Math}/u");
            compileOk("/\\p{Noncharacter_Code_Point}/u");
            compileOk("/\\p{NChar}/u");
            compileOk("/\\p{Pattern_Syntax}/u");
            compileOk("/\\p{Pat_Syn}/u");
            compileOk("/\\p{Pattern_White_Space}/u");
            compileOk("/\\p{Pat_WS}/u");
            compileOk("/\\p{Quotation_Mark}/u");
            compileOk("/\\p{QMark}/u");
            compileOk("/\\p{Radical}/u");
            compileOk("/\\p{Regional_Indicator}/u");
            compileOk("/\\p{RI}/u");
            compileOk("/\\p{Sentence_Terminal}/u");
            compileOk("/\\p{STerm}/u");
            compileOk("/\\p{Soft_Dotted}/u");
            compileOk("/\\p{SD}/u");
            compileOk("/\\p{Terminal_Punctuation}/u");
            compileOk("/\\p{Term}/u");
            compileOk("/\\p{Unified_Ideograph}/u");
            compileOk("/\\p{UIdeo}/u");
            compileOk("/\\p{Uppercase}/u");
            compileOk("/\\p{Upper}/u");
            compileOk("/\\p{Variation_Selector}/u");
            compileOk("/\\p{VS}/u");
            compileOk("/\\p{White_Space}/u");
            compileOk("/\\p{WSpace}/u");
            compileOk("/\\p{space}/u");
            compileOk("/\\p{XID_Continue}/u");
            compileOk("/\\p{XIDC}/u");
            compileOk("/\\p{XID_Start}/u");
            compileOk("/\\p{XIDS}/u");
            compileOk("/\\P{Lu}/u", "negated form");
        }
    },
    {
        name: "Syntax: property=value and General_Category values accepted",
        body() {
            compileOk("/\\p{General_Category=Uppercase_Letter}/u");
            compileOk("/\\p{gc=Uppercase_Letter}/u");
            compileOk("/\\p{gc=Lu}/u");
            compileOk("/\\p{General_Category=Lu}/u");
            compileOk("/\\p{Lu}/u", "lone gc value");
            compileOk("/\\p{Uppercase_Letter}/u");
            compileOk("/\\p{Letter}/u", "grouped gc value");
            compileOk("/\\p{L}/u");
            compileOk("/\\p{LC}/u");
            compileOk("/\\p{Cased_Letter}/u");
            compileOk("/\\p{Mark}/u");
            compileOk("/\\p{Combining_Mark}/u");
            compileOk("/\\p{M}/u");
            compileOk("/\\p{Number}/u");
            compileOk("/\\p{N}/u");
            compileOk("/\\p{Punctuation}/u");
            compileOk("/\\p{punct}/u");
            compileOk("/\\p{P}/u");
            compileOk("/\\p{Symbol}/u");
            compileOk("/\\p{S}/u");
            compileOk("/\\p{Separator}/u");
            compileOk("/\\p{Z}/u");
            compileOk("/\\p{Other}/u");
            compileOk("/\\p{C}/u");
            compileOk("/\\p{cntrl}/u");
            compileOk("/\\p{digit}/u");
            compileOk("/\\p{Unassigned}/u");
            compileOk("/\\p{Cn}/u");
            compileOk("/\\p{Surrogate}/u");
            compileOk("/\\p{Cs}/u");
        }
    },
    {
        name: "Syntax: Script and Script_Extensions accepted",
        body() {
            compileOk("/\\p{Script=Greek}/u");
            compileOk("/\\p{sc=Greek}/u");
            compileOk("/\\p{sc=Grek}/u", "ISO 15924 alias");
            compileOk("/\\p{sc=Qaai}/u", "legacy Inherited alias");
            compileOk("/\\p{sc=Qaac}/u", "legacy Coptic alias");
            compileOk("/\\p{sc=Copt}/u");
            compileOk("/\\p{Script_Extensions=Greek}/u");
            compileOk("/\\p{scx=Greek}/u");
            compileOk("/\\p{scx=Katakana}/u");
            compileOk("/\\p{sc=Unknown}/u");
            compileOk("/\\p{sc=Zzzz}/u");
            compileOk("/\\p{scx=Unknown}/u", "Unknown is a valid script value");
        }
    },
    {
        name: "Syntax: rejected names and forms (exact matching, u mode)",
        body() {
            compileError("/\\p{lowercase}/u", "loose matching is gone: wrong case");
            compileError("/\\p{ANY}/u");
            compileError("/\\p{ascii}/u");
            compileError("/\\p{wSpace}/u");
            compileError("/\\p{WHITE_SPACE}/u");
            compileError("/\\p{ Lu }/u", "spaces are not stripped");
            compileError("/\\p{White_Space }/u");
            compileError("/\\p{White-Space}/u", "hyphens are not stripped");
            compileError("/\\p{GC=Lu}/u", "wrong-case property alias");
            compileError("/\\p{Sc=Greek}/u");
            compileError("/\\p{Katakana}/u", "script values need Script=/sc=/scx=");
            compileError("/\\p{Greek}/u");
            compileError("/\\p{Common}/u");
            compileError("/\\p{Qaai}/u", "not a binary/gc spelling");
            compileError("/\\p{Script}/u", "property name alone");
            compileError("/\\p{General_Category}/u");
            compileError("/\\p{Script_Extensions}/u");
            compileError("/\\p{gc=Greek}/u", "script value under gc");
            compileError("/\\p{sc=Lu}/u", "gc value under sc");
            compileError("/\\p{scx=Lu}/u", "gc value under scx");
            compileError("/\\p{gc=Bogus}/u", "unknown value");
            compileError("/\\p{sc=Bogus}/u");
            compileError("/\\p{Block=Greek}/u", "not an ES property");
            compileError("/\\p{Age=1.0}/u");
            compileError("/\\p{Lu=Y}/u", "binary property with a value");
            compileError("/\\p{Hyphen}/u", "UCD binary property outside the ES set");
            compileError("/\\p{Other_Alphabetic}/u");
            compileError("/\\p{Grapheme_Link}/u");
            compileError("/\\p{Prepended_Concatenation_Mark}/u");
            compileError("/\\p{}/u", "empty body");
            compileError("/\\p{=}/u");
            compileError("/\\p{Lu=}/u", "empty value");
            compileError("/\\p{=Lu}/u", "empty name");
            compileError("/\\p{a=b=c}/u", "second separator");
            compileError("new RegExp('\\\\p{a=b=c}', 'u')");
            compileError("/\\p{L\\u00fc}/u", "non-ASCII name");
            compileError("/\\p{Any1}/u", "unknown name");
            compileError("/\\p{Lu/u", "unterminated body");
            compileError("new RegExp('\\\\p{Lu', 'u')");
            compileError("/\\p/u", "no body at all in u mode");
            compileError("/\\P/u");
            compileError("/[\\p{Lu}-Z]/u", "property escape as range start");
            compileError("/[A-\\p{Lu}]/u", "property escape as range end");
        }
    },
    {
        name: "Non-u mode keeps the identity-escape behavior",
        body() {
            compileOk("/\\p{Lu}/");
            compileOk("/\\P{Lu}/");
            compileOk("/\\p{Bogus}/");
            compileOk("/[\\p{Lu}]/");
            assert.isTrue(/\p{Lu}/.test("p{Lu}"), "identity 'p' plus literal braces");
            assert.isTrue(/\p{Any}/.test("p{Any}"));
            assert.isTrue(/[\p{Lu}]/.test("u"), "class of p, {, L, u, }");
            assert.isFalse(/\p{Lu}/.test("A"));
            assert.isTrue(/\p/.test("p"), "bare \\p stays an identity escape");
            assert.isTrue(/\P/.test("P"));
        }
    },
    {
        name: "Matching: binary properties and General_Category",
        body() {
            assert.isTrue(/\p{Lu}/u.test("A"));
            assert.isFalse(/\p{Lu}/u.test("a"));
            assert.isTrue(/\p{L}/u.test("a"), "grouped value");
            assert.isTrue(/\p{Nd}/u.test("5"));
            assert.isFalse(/\p{Nd}/u.test("x"));
            assert.isTrue(/\p{punct}/u.test("("), "group alias");
            assert.isTrue(/\p{cntrl}/u.test("\0"), "legacy single-category alias");
            assert.isTrue(/\p{digit}/u.test("7"));
            assert.isTrue(/\p{Alphabetic}/u.test("a"));
            assert.isTrue(/\p{Alpha}/u.test("a"));
            assert.isTrue(/\p{White_Space}/u.test(" "));
            assert.isTrue(/\p{space}/u.test("\t"), "legacy WSpace alias");
            assert.isTrue(/\p{ASCII}/u.test("a"));
            assert.isFalse(/\p{ASCII}/u.test("\u00e9"));
            assert.isTrue(/\p{Math}/u.test("+"));
            assert.isTrue(/\p{Hex}/u.test("F"));
            assert.isTrue(/\p{Emoji}/u.test("#"));
            assert.isFalse(/\p{Extended_Pictographic}/u.test("#"), "keycap is Emoji but not ExtPict");
            assert.isTrue(/\p{ExtPict}/u.test("\u2764"), "heart is ExtPict");
            assert.isTrue(/\p{ID_Start}/u.test("A"));
            assert.isTrue(/\p{IDS}/u.test("A"));
        }
    },
    {
        name: "Matching: Any, Assigned and surrogate code points",
        body() {
            assert.isTrue(/\p{Any}/u.test("a"));
            assert.isTrue(/\p{Any}/u.test("\ud800"), "lone high surrogate is a code point");
            assert.isTrue(/\p{Any}/u.test("\udc00"), "lone low surrogate is a code point");
            assert.isTrue(/^\p{Any}+$/u.test("\ud800\udc00a\u{1f600}"), "full-string run including pairs");
            assert.isTrue(/^\P{Lu}+$/u.test("\u{1d41a}b"), "astral non-Lu run");
            assert.isTrue(/\p{Assigned}/u.test("\ud800"), "Cs is assigned");
            assert.isTrue(/\p{Cs}/u.test("\ud800"));
            assert.isFalse(/\p{Cs}/u.test("a"));
            assert.isTrue(/\p{gc=Cs}/u.test("\udbff"));
            assert.isFalse(/\P{Any}/u.test("x"), "\\P{Any} matches nothing");
        }
    },
    {
        name: "Matching: astral code points and negation",
        body() {
            assert.isTrue(/\p{Lu}/u.test("\u{1d400}"), "math bold capital A");
            assert.isFalse(/\p{Lu}/u.test("\u{1d41a}"), "math bold small a is Ll");
            assert.isTrue(/\p{gc=So}/u.test("\u{1f600}"));
            assert.isTrue(/\p{L}/u.test("\u{10400}"), "deseret capital is Lo");
            assert.isTrue(/\P{Lu}/u.test("a"));
            assert.isFalse(/\P{Lu}/u.test("A"));
            assert.isTrue(/^\P{Lu}+$/u.test("\u{1d41a}b"), "astral non-Lu run");
            assert.isTrue(/\P{ASCII}/u.test("\u00e9"));
        }
    },
    {
        name: "Matching: Script vs Script_Extensions",
        body() {
            assert.isTrue(/\p{Script=Greek}/u.test("\u0370"));
            assert.isTrue(/\p{sc=Grek}/u.test("\u0370"));
            assert.isTrue(/\p{Script=Greek}/u.test("\u{10140}"), "ancient Greek numbers are astral");
            assert.isFalse(/\p{Script=Katakana}/u.test("\u30fc"), "prolonged sound mark is sc=Common");
            assert.isTrue(/\p{Script_Extensions=Katakana}/u.test("\u30fc"), "...but scx contains Katakana");
            assert.isTrue(/\p{scx=Katakana}/u.test("\u30fc"));
            assert.isTrue(/\p{sc=Qaai}/u.test("\u0300"), "legacy alias resolves Inherited");
            assert.isTrue(/\p{sc=Inherited}/u.test("\u0300"), "combining grave is Inherited");
            assert.isFalse(/\p{sc=Greek}/u.test("a"));
        }
    },
    {
        name: "Case-insensitive interaction",
        body() {
            assert.isTrue(/\p{Lu}/iu.test("a"), "closure adds lowercase into Lu");
            assert.isTrue(/[\p{Lu}]/iu.test("a"));
            assert.isTrue(/\P{Lu}/iu.test("A"), "atom negation closes after complementing");
            assert.isFalse(/[^\p{Lu}]/iu.test("A"), "class negation closes before complementing");
            assert.isTrue(/\p{Ll}/iu.test("A"));
            assert.isTrue(/(?i:\p{Lu})/u.test("a"), "scoped modifier i closes the set");
            assert.isFalse(/(?-i:\P{Lu})/iu.test("A"), "scoped removal restores exact set");
        }
    },
    {
        name: "Character classes",
        body() {
            assert.isTrue(/[\p{Lu}\p{Nd}]/u.test("5"), "union");
            assert.isTrue(/[\p{Lu}\p{Nd}]/u.test("A"));
            assert.isFalse(/[\p{Lu}\p{Nd}]/u.test("a"));
            assert.isTrue(/[^\p{Lu}]/u.test("a"), "class negation");
            assert.isFalse(/[^\p{Lu}]/u.test("A"));
            assert.isTrue(/[\P{Lu}a]/u.test("a"), "negated escape inside class");
            assert.isTrue(/[\P{Lu}]/u.test("a"));
            assert.isFalse(/[\P{Lu}]/u.test("A"));
            assert.isTrue(/[\p{Lu}-]/u.test("-"), "trailing dash stays literal");
            assert.isTrue(/[-\p{Lu}]/u.test("A"), "leading dash stays literal");
            assert.isTrue(/[\p{Nd}]/u.test("\u{1d7ce}"), "astral member via arms");
            assert.isTrue(/[^\p{Nd}]/u.test("\u{1f600}"), "astral complement member");
        }
    },
    {
        name: "Composition: quantifiers, groups, anchors, replace/split",
        body() {
            assert.isTrue(/^\p{Lu}+$/u.test("ABC"));
            assert.isFalse(/^\p{Lu}+$/u.test("ABc"));
            assert.isTrue(/^(?:\p{Lu}|\p{Nd})+$/u.test("A5A5"));
            assert.isTrue(/(?<=\p{Lu})b/u.test("Ab"), "lookbehind");
            assert.isTrue(/(?<!\P{Lu})b/u.test("Ab"));
            assert.areEqual("AA", /(?<g>\p{Lu})\k<g>/u.exec("AA")[0], "named group backreference");
            assert.areEqual(["a5", ""], "a5\u0393".split(/\p{Lu}/u));
            assert.areEqual("a<A>5!", "aA5!".replace(/\p{Lu}/gu, "<$&>"));
            assert.areEqual(["\\p{Lu}", "\\P{Lu}", "[\\p{Lu}]"],
                [/\p{Lu}/u.source, /\P{Lu}/u.source, /[\p{Lu}]/u.source], "source round-trip");
        }
    },
    {
        name: "Class \\p/\\P without a body: Annex B identity vs u-mode rejection",
        body() {
            compileOk("/[\\p]/");
            compileOk("/[\\pL]/");
            compileOk("/[\\P]/");
            compileOk("/[\\p]x{2}/");
            compileOk("/[\\pL]x{2}/");
            compileOk("/[\\pxNd}]/", "brace-less short form with a trailing brace character");
            compileOk("/[\\p{Lu]/", "brace opened but never closed before the literal end");
            compileOk("new RegExp('[\\\\p{Lu]')");

            assert.isTrue(/[\p]/.test("p"), "identity 'p' in the class");
            assert.isFalse(/[\p]/.test("L"));
            assert.isTrue(/[\pL]/.test("L"), "identity 'p' followed by literal 'L'");
            assert.isTrue(/[\P]/.test("P"));
            assert.isFalse(/[\P]/.test("p"));
            assert.isTrue(/[\p]x{2}/.test("pxx"), "quantified tail after the class");
            assert.isFalse(/[\p]x{2}/.test("pxy"));
            assert.isTrue(/[\pL]x{2}/.test("Lxx"));
            assert.isTrue(/[\pxNd}]/.test("}"), "brace character is a class member");
            assert.isTrue(/[\pxNd}]/.test("p"));
            assert.isFalse(/[\pxNd}]/.test("Q"));
            assert.isTrue(/[\p{Lu]/.test("{"), "'{Lu' re-parse as literal characters");
            assert.isTrue(new RegExp("[\\p{Lu]").test("p"), "dynamic form matches literals");

            compileError("/[\\p]/u");
            compileError("/[\\pL]/u");
            compileError("/[\\P]/u");
            compileError("/[\\p]x{2}/u");
            compileError("/[\\pL]x{2}/u");
            compileError("new RegExp('[\\\\p]', 'u')");
            compileError("new RegExp('[\\\\pL]x{2}', 'u')");
        }
    },
    {
        name: "Script values without assigned code points are rejected",
        body() {
            var unassigned = ["Hanb", "Kpel", "Loma", "Hans", "Hant", "Zxxx", "Zmth", "Zsym", "Jamo", "Aran", "Hrkt", "Hntl"];
            for (var i = 0; i < unassigned.length; i++) {
                var code = unassigned[i];
                compileError("new RegExp('\\\\p{sc=" + code + "}', 'u')");
                compileError("new RegExp('\\\\p{scx=" + code + "}', 'u')");
                compileError("new RegExp('\\\\p{Script=" + code + "}', 'u')");
                compileError("new RegExp('\\\\p{Script_Extensions=" + code + "}', 'u')");
            }

            var assigned = ["Greek", "Grek", "Qaai", "Qaac", "Copt", "Unknown", "Zzzz", "Inherited", "Zinh", "Katakana", "Hiragana", "Latin"];
            for (var j = 0; j < assigned.length; j++) {
                var ok = assigned[j];
                compileOk("new RegExp('\\\\p{sc=" + ok + "}', 'u')");
                compileOk("new RegExp('\\\\p{scx=" + ok + "}', 'u')");
            }
            assert.isTrue(/\p{sc=Katakana}/u.test("\u30a2"), "assigned script still matches");
            assert.isTrue(/\p{scx=Katakana}/u.test("\u30fc"), "assigned script extensions still match");
            assert.isTrue(/\p{sc=Unknown}/u.test("\u0378"), "Unknown covers unassigned code points");
        }
    },
    {
        name: "Quantified property sets: semantics and linear backtracking",
        body() {
            // Correctness across quantifier kinds and continuations
            assert.isTrue(/^\P{Lu}+$/u.test("a".repeat(500)), "greedy + over a long run");
            assert.isFalse(/^\P{Lu}+z$/u.test("a".repeat(256) + "A"), "failing continuation at n=256");
            assert.isFalse(/^[\P{Lu}]+z$/u.test("a".repeat(256) + "A"), "class form, failing continuation");
            assert.isTrue(/^\P{Lu}+?A/u.test("a".repeat(128) + "A"), "lazy + finds the late member");
            assert.isFalse(/^\P{Lu}+?z/u.test("a".repeat(128) + "A"), "lazy + failing");
            assert.isTrue(/^\P{Lu}*b$/u.test("aaab"), "star");
            assert.isTrue(/^\p{gc=Ll}{3}$/u.test("abc"), "exact count");
            assert.isFalse(/^\P{Lu}{2,5}$/u.test("abcdef"), "bounded over count");
            assert.isTrue(/^\p{Lu}?a$/u.test("a"), "optional");
            assert.isTrue(/^\P{Lu}+\p{Lu}+$/u.test("a".repeat(50) + "A".repeat(50)), "two quantified atoms");
            assert.isTrue(/^\p{Script=Greek}+$/u.test("\u03b1\u03b2\u03b3".repeat(50)), "script +");
            assert.isFalse(/^\p{Script=Greek}+z$/u.test("\u03b1".repeat(128)), "script + failing continuation");
            assert.isTrue(/^[\p{Script=Greek}]+$/u.test("\u03b1\u{10140}"), "class with astral member (ancient Greek numbers)");
            assert.isTrue(/^\p{L}+$/u.test("aA\u{1D41A}".repeat(40)), "gc group + with astral run");
            assert.isFalse(/^\p{L}+z$/u.test("\u{1D41A}".repeat(128)), "astral run failing continuation");
            assert.isTrue(/^\p{scx=Katakana}+$/u.test("\u30a2".repeat(100)), "scx +");
            assert.isTrue(/^\P{Lu}+$/u.test("\u{1D41A}".repeat(200)), "astral complement run");
            assert.isFalse(/^\P{Lu}+z$/u.test("\u{1D41A}".repeat(128)), "astral complement failing continuation");
            assert.isTrue(/^\p{Cs}+$/u.test("\ud800\udbff".repeat(50)), "lone surrogate run");
            assert.isTrue(/^\p{Any}+$/u.test("a\u{1F600}\ud800b".repeat(40)), "any with pairs and lone surrogates");
            assert.isTrue(new RegExp("^(\\P{Lu})+$", "u").test("abc"), "grouped quantified");
            assert.areEqual("c", new RegExp("^(\\P{Lu})+$", "u").exec("abc")[1], "last iteration captured");
            assert.isTrue(/(?<=^\P{Lu}+)A/u.test("aaaA"), "lookbehind over quantified set");
            assert.isTrue(/^\P{Lu}+$/u.test("a".repeat(128)), "iu closure run");
            assert.isFalse(/^\P{Lu}+$/u.test("a".repeat(128) + "A"), "iu closure late member");
            assert.areEqual("A|5!|", "aA5!".replace(/^a+/, "").replace(/\P{Lu}+/u, "|$&|"), "replace with quantified set");

            // Linear-backtracking smoke at sizes that were exponential before
            // (2^26 partitions at n=26; wall-clock stays instant when linear)
            assert.isFalse(new RegExp("^(?:\\P{Lu})+z$", "u").test("a".repeat(1024) + "A"), "atom n=1024");
            assert.isFalse(new RegExp("^[\\P{Lu}]+z$", "u").test("a".repeat(1024) + "A"), "class n=1024");
            assert.isFalse(new RegExp("^\\P{Lu}+z$", "u").test("\u{1D41A}".repeat(512)), "astral n=512");
        }
    },
    {
        name: "Dynamic construction, compile and flags",
        body() {
            assert.isTrue(new RegExp("\\p{Lu}", "u").test("A"));
            assert.throws(function () { new RegExp("\\p{Bogus}", "u"); }, SyntaxError);
            var r = /x/;
            r.compile("\\p{Nd}", "u");
            assert.isTrue(r.test("5"));
            assert.isFalse(/\p{Lu}/u.test(""), "no empty match for a property atom");
            assert.isTrue(/^\P{Lu}*$/u.test(""), "quantified negation can match empty");
            assert.areEqual("a-", "aA".replace(/\p{Lu}/gu, "-"), "global replace");
            assert.areEqual("aA", "aA".replace(/\p{Lu}/yu, "-"), "sticky at lastIndex 0 does not match 'a'");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
