//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2018 RegExp named capture groups, core slice (doc/c2-s9-namedgroups.md):
// (?<name>...) syntax, \k<name> backreferences, the "groups" property on
// match/exec results, and $<name> substitution in String replace/replaceAll.
// Everything here is unconditional (no flag); the -ES6RegExSymbols shapes
// (matchAll, @@replace with custom exec results) live in
// namedgroups-symbols.js.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

var tests = [
    {
        name: "Named group syntax: accepted shapes",
        body() {
            compileOk("/(?<a>a)/");
            compileOk("/(?<a42>a)/");
            compileOk("/(?<_>a)/");
            compileOk("/(?<$>a)/");
            compileOk("/(?<π>a)/");
            compileOk("/(?<_‍>a)/", "raw ZWNJ continue");
            compileOk("/(?<_‌>a)/", "raw ZWJ continue");
            compileOk("/(?<𝑓𝑜𝑥>a)/", "astral name");
            compileOk("/(?<\\u{1d4d1}>a)/", "curly escape, no u flag");
            compileOk("/(?<\\ud835\\udcd1>a)/", "escaped surrogate pair, no u flag");
            compileOk("/(?<\\u{1d4d1}>a)/u", "curly escape, u flag");
            compileOk("/(?<a>a)/u", "u flag");
            compileOk("/(?<a>a)(?<b>b)/", "distinct names");
            compileOk("new RegExp('(?<dy>dy)')", "dynamic pattern");
            compileOk("/\\k<a>(?<a>x)/", "forward reference");
            compileOk("/(?<a>\\k<a>\\w)/", "reference inside its own group");
        }
    },
    {
        name: "Named group syntax: rejected shapes",
        body() {
            compileError("/(?<42a>a)/", "name must start ID_Start");
            compileError("/(?<>a)/", "empty name");
            compileError("/(?<:a>a)/", "punctuator start");
            compileError("/(?<❤>a)/", "non-id start");
            compileError("/(?<$❞>a)/", "non-id continue");
            compileError("/(?<aa)/", "unterminated group specifier");
            compileError("/(?<a>.)\\k<a/", "unterminated reference name");
            compileError("/(?<a>a)(?<a>b)/", "duplicate names");
            compileError("/(?<a>a)\\k<b>/", "dangling reference");
            compileError("/(?<ab>a)\\k<a>/", "dangling prefix");
            compileError("/\\k<a>/u", "dangling reference under u");
            compileError("/(?<a>a)\\k/", "bare \\k when named captures exist");
            compileError("/(?<a>a)\\k1/", "\\k not followed by < in named mode");
            compileError("/(?<a\\uD801>.)/", "escaped lone leading surrogate");
            compileError("/(?<a\\uDCA4>.)/", "escaped lone trailing surrogate");
            compileError("/(?<=a)b/", "lookbehind stays a SyntaxError");
            compileError("/(?<!a)b/", "negative lookbehind stays a SyntaxError");
            // Scripts longer than the deferral threshold scan function
            // bodies without an AST. That scan has to reject these itself;
            // a hit found only when the function is called is an internal error.
            var deferredPad = new Array(1500).join("x=1;");
            compileError("function f(){ /(?<a>a)\\k<b>/; }" + deferredPad, "dangling reference in a deferred function");
            compileError("function f(){ /(?<a>a)[\\k]/; }" + deferredPad, "class \\k in a deferred function");
            compileError("/\\k<a\\(b)/", "unescaped ')' after an escaped '('");
        }
    },
    {
        name: "\\k is an Annex B identity escape without named captures",
        body() {
            assert.isTrue(/\k/.test("k"), "\\k is k");
            assert.isTrue(/\k<a>/.test("k<a>"), "\\k<a> is k then literal <a>");
            var m = /\k<a>/.exec("k<a>");
            assert.areEqual(1, m.length, "one element");
            assert.areEqual("k<a>", m[0]);
            // An escaped slash is not the end of the literal, and an
            // escaped paren is not a group. Both reparse as identity escapes.
            assert.isTrue(/\k<a\/>/.test("k<a/>"), "escaped slash stays in the body");
            assert.isTrue(/\k<a\/>/g.test("k<a/>"), "escaped slash with a flag");
            assert.isTrue(/\k<a\/b>/.test("k<a/b>"), "escaped slash before '>'");
            assert.isTrue(/\k<a\(b\)/.test("k<a(b)"), "escaped parens stay literal");
            assert.isTrue(new RegExp("\\k<a\\(").test("k<a("), "dynamic escaped paren");
            var deferredPad = new Array(1500).join("x=1;");
            assert.isTrue(eval("function f(){ return /(?<a>x)\\k<a>/.test('xx'); }" + deferredPad + "f()"), "named group in a deferred function");
        }
    },
    {
        name: "groups object basics: null prototype, values, undefined for non-participating",
        body() {
            var m = /(?<a>a).|(?<x>x)/.exec("ab");
            assert.areEqual("a", m.groups.a, "participating group");
            assert.areEqual(undefined, m.groups.x, "non-participating group");
            assert.areEqual(null, Object.getPrototypeOf(m.groups), "null prototype");
            assert.areEqual("a", m[1], "capture still positional");
        }
    },
    {
        name: "groups property order follows textual group order",
        body() {
            var groups = /(?<fst>.)|(?<snd>.)/u.exec("abcd").groups;
            assert.areEqual(["fst", "snd"], Object.getOwnPropertyNames(groups));
        }
    },
    {
        name: "groups is undefined but present for patterns without named groups",
        body() {
            var m = /./.exec("a");
            assert.areEqual(undefined, m.groups);
            assert.isTrue(m.hasOwnProperty("groups"), "own groups property");
            var desc = Object.getOwnPropertyDescriptor(m, "groups");
            assert.isTrue(desc.writable && desc.enumerable && desc.configurable, "data property attributes");
        }
    },
    {
        name: "groups properties are created with CreateDataProperty semantics",
        body() {
            var counter = 0;
            Object.defineProperty(Array.prototype, "groups", { set() { counter++; } });
            var m = /(?<x>.)/.exec("a");
            assert.areEqual(0, counter, "no Set through Array.prototype");
            var desc = Object.getOwnPropertyDescriptor(m, "groups");
            assert.isTrue(desc.writable && desc.enumerable && desc.configurable, "groups attributes");
            var protoCounter = 0;
            Object.defineProperty(Object.prototype, "x", { set() { protoCounter++; } });
            var g = /(?<x>.)/.exec("a").groups;
            assert.areEqual(0, protoCounter, "no Set through Object.prototype");
            var gDesc = Object.getOwnPropertyDescriptor(g, "x");
            assert.isTrue(gDesc.writable && gDesc.enumerable && gDesc.configurable, "group value attributes");
        }
    },
    {
        name: "__proto__ as a group name is an ordinary data property",
        body() {
            var groups = /(?<__proto__>.)/.exec("a").groups;
            assert.areEqual("a", groups.__proto__);
            assert.areEqual(null, Object.getPrototypeOf(groups));
        }
    },
    {
        name: "match: non-global result carries groups, global does not",
        body() {
            var m = "bab".match(/(?<a>a)/);
            assert.areEqual("a", m.groups.a);
            var g = "aa".match(/(?<x>a)/g);
            assert.areEqual(["a", "a"], g);
            assert.isFalse(Object.prototype.hasOwnProperty.call(g, "groups"), "global match array has no groups");
        }
    },
    {
        name: "exec/test/search/split interplay",
        body() {
            var re = /(?<c>,)/;
            assert.areEqual(["a", ",", "b"], "a,b".split(re), "named captures stay positional in split");
            assert.areEqual(1, "bab".search(/(?<x>a)/), "search returns the index");
            assert.isTrue(/(?<x>a)/.test("a"), "test");
        }
    },
    {
        name: "named backreference semantics",
        body() {
            var m = "bab".match(/(?<b>.).\k<b>/);
            assert.areEqual("bab", m[0]);
            assert.areEqual("b", m[1]);
            assert.areEqual(null, "baa".match(/(?<b>.).\k<b>/));
            m = "bab".match(/(?<a>\k<a>\w)../);
            assert.areEqual("bab", m[0]);
            assert.areEqual("b", m[1]);
            assert.areEqual("b", "bab".match(/(?<a>\k<a>\w)../).groups.a);
            m = "bab".match(/(?<b>b)\k<a>(?<a>a)\k<b>/);
            assert.areEqual("bab", m[0]);
            assert.areEqual("b", m[1]);
            assert.areEqual("a", m[2]);
            m = /(?<a>a)(?<b>b)\k<a>/.exec("aba");
            assert.areEqual("a", m.groups.a);
            assert.areEqual("b", m.groups.b);
            assert.areEqual(undefined, m.groups.c, "unknown name reads undefined");
            // A backreference to a non-participating group matches the empty string
            m = "b".match(/(?:(?<x>a)|b)\k<x>/);
            assert.areEqual("b", m[0]);
            assert.areEqual(undefined, m[1]);
        }
    },
    {
        name: "numbered and named captures interoperate",
        body() {
            var m = "baba".match(/(.)((?<a>a))\1\2/);
            assert.areEqual("baba", m[0]);
            assert.areEqual("b", m[1]);
            assert.areEqual("a", m[2]);
            m = "baba".match(/(.)(?<a>a)(?<b>\1)(\2)/);
            assert.areEqual("baba", m[0]);
            assert.areEqual(5, m.length);
            assert.areEqual("b", m[1]);
            assert.areEqual("a", m[2]);
            assert.areEqual("b", m[3]);
            assert.areEqual("a", m[4]);
        }
    },
    {
        name: "exotic names resolve and are reachable on the groups object",
        body() {
            var m = "brown".match(/(?<\u{62}rown>brown)/);
            assert.areEqual("brown", m.groups.brown, "curly escape without u flag");
            m = "brown".match(/(?<\ud835\udcd1rown>brown)/);
            assert.areEqual("brown", m.groups.𝓑rown, "surrogate escape name decodes astrally");
            assert.areEqual("a", /(?<π>a)/.exec("bab").groups.π);
            assert.areEqual("a", /(?<$>a)/.exec("bab").groups.$);
        }
    },
    {
        name: "$<name> substitution in replace",
        body() {
            assert.areEqual("a", "a".replace(/(?<x>a)/, "$<x>"), "simple");
            assert.areEqual("", "a".replace(/(?<x>a)/, "$<y>"), "unknown name contributes nothing");
            assert.areEqual("", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/g, "$<thd>"), "unmatched name, global");
            assert.areEqual("cd", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/, "$<thd>"), "unmatched name, non-global");
            assert.areEqual("$<x", "a".replace(/(?<x>a)/, "$<x"), "unclosed stays literal");
            assert.areEqual("cd", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/, "$<42$1>"), "invalid name text is consumed");
            assert.areEqual("cd", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/, "$<>"), "empty name is consumed");
            assert.areEqual("$<x>a", "a".replace(/(?<x>a)/, "$$<x>a"), "$$ escapes");
            assert.areEqual("badc", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/g, "$<snd>$<fst>"), "order");
            assert.areEqual("bacd", "abcd".replace(/(?<fst>.)(?<snd>.)/, "$2$<fst>"), "mixes with $1/$2");
        }
    },
    {
        name: "$<name> stays literal without named captures",
        body() {
            var re = /(.)(.)|(x)/;
            assert.areEqual("$<snd>$<fst>cd", "abcd".replace(re, "$<snd>$<fst>"), "no groups object");
            assert.areEqual("$<sndcd", "abcd".replace(re, "$<snd"), "unclosed");
            assert.areEqual("$<42a>cd", "abcd".replace(re, "$<42$1>"), "digit fallback still applies");
        }
    },
    {
        name: "Array.prototype.groups must not leak into substitution",
        body() {
            Array.prototype.groups = { a: "b" };
            assert.areEqual("$<a>", "a".replace(/a/, "$<a>"), "no named captures");
            assert.areEqual("", "a".replace(/(?<x>a)/, "$<a>"), "named captures: unknown name");
            Array.prototype.groups = undefined;
        }
    },
    {
        name: "$<name> in replaceAll",
        body() {
            assert.areEqual("aaaa", "aa".replaceAll(/(?<x>a)/g, "$<x>$<x>"));
            assert.areEqual("xab", "ab".replaceAll(/(?<x>a)/g, "x$<x>"));
            // string search values have no named captures: $<name> stays literal
            assert.areEqual("$<x>$<x>", "aa".replaceAll("a", "$<x>"));
        }
    },
    {
        name: "replacement functions receive the groups object as the final argument",
        body() {
            var seen = null;
            var result = "abcd".replace(/(?<fst>.)(?<snd>.)/, function (matched, fst, snd, offset, string, groups) {
                assert.areEqual(6, arguments.length, "match + 2 captures + offset + string + groups");
                seen = groups;
                return groups.snd + groups.fst;
            });
            assert.areEqual("a", seen.fst);
            assert.areEqual("b", seen.snd);
            assert.areEqual("bacd", result, "rearranged");
            assert.areEqual("3", "a".replace(/a/, function () { return String(arguments.length); }), "no groups argument without named captures");
            // per-match groups across a global replace
            var collected = "";
            "a1b2".replace(/(?<letter>[ab])|(?<digit>\d)/g, function (m, l, d, o, s, groups) {
                collected += String(groups.letter) + String(groups.digit) + ";";
                return "";
            });
            assert.areEqual("aundefined;undefined1;bundefined;undefined2;", collected, "per-match groups");
        }
    },
    {
        name: "lastIndex and global interplay",
        body() {
            var re = /(?<x>a)/g;
            var m1 = re.exec("aa");
            assert.areEqual(0, m1.index);
            assert.areEqual(1, re.lastIndex);
            var m2 = re.exec("aa");
            assert.areEqual(1, m2.index);
            assert.areEqual("a", m2.groups.x);
            assert.areEqual(null, re.exec("aa"));
            assert.areEqual(0, re.lastIndex, "failed global exec resets lastIndex");
        }
    },
    {
        name: "dynamic RegExp named groups",
        body() {
            var re = new RegExp("(?<dy>dy)(?<an>na)\\k<dy>");
            var m = re.exec("dynadyna");
            assert.areEqual("dynady", m[0]);
            assert.areEqual("dy", m.groups.dy);
            assert.areEqual("na", m.groups.an);
        }
    },
    {
        name: "dynamic construction with resolvable \\k<name> under every flag",
        body() {
            // Fresh dynamic construction must not depend on allocation
            // layout between the pattern and flags strings (the re-scan of
            // the recorded \k span happens after the flags were parsed).
            var flagsList = ["", "i", "g", "m", "s", "y", "u", "ig", "gi", "ims", "gsy", "giu", "imsgy", "igmsuy"];
            for (var i = 0; i < flagsList.length; i++) {
                var re = new RegExp("(?<a>x)\\k<a>", flagsList[i]);
                assert.areEqual("(?<a>x)\\k<a>", re.source, "source under flags '" + flagsList[i] + "'");
                assert.isTrue(re.test("xx"), "backreference resolves under flags '" + flagsList[i] + "'");
            }
            var ug = new RegExp("(?<a>x)\\k<a>", "gu");
            var m = ug.exec("xx");
            assert.areEqual("xx", m[0]);
            assert.areEqual("x", m.groups.a);
            assert.areEqual(2, ug.lastIndex, "global lastIndex advances");
            var warm = [new RegExp("a"), /b/, new RegExp("(?<z>z)"), new RegExp("(?<a>x)\\k<a>", "g")];
            assert.isTrue(warm[3].test("xx"), "construction after other regexes still resolves");
            var compiled = new RegExp("junk");
            compiled.compile("(?<a>x)\\k<a>", "g");
            assert.isTrue(compiled.test("xx"), "compile() path resolves");
            assert.isTrue(/(?<a>x)\k<a>/g.test("xx"), "literal path resolves");
            assert.isTrue(new RegExp("\\k<a>(?<a>x)").test("xx"), "forward reference");
        }
    },
    {
        name: "\\k in a character class: identity only without named captures and u",
        body() {
            // Annex B: [\k] is the identity escape 'k' for a non-u pattern
            // with no named captures; the u flag has no identity escape for
            // 'k' inside classes, and Annex B reserves 'k' once named
            // captures appear (mirroring the bare-\k rule).
            assert.isTrue(new RegExp("[\\k]").test("k"), "constructor, non-u");
            assert.isTrue(/[\k]/.test("k"), "literal, non-u");
            assert.isFalse(new RegExp("[\\k]").test("x"), "identity escape matches only k");
            assert.areEqual(["k", "<", "a", ">"], "k<a>".match(new RegExp("[\\k<a>]", "g")), "literals after identity \\k");
            assert.throws(function () { new RegExp("[\\k]", "u"); }, SyntaxError, "constructor, u flag");
            assert.throws(function () { eval("/[\\k]/u"); }, SyntaxError, "literal, u flag");
            assert.throws(function () { new RegExp("[\\k<a>]", "u"); }, SyntaxError, "\\k<a> shape, u flag");
            assert.throws(function () { new RegExp("(?<a>x)[\\k]"); }, SyntaxError, "named captures, non-u");
            assert.throws(function () { new RegExp("[\\k](?<a>x)"); }, SyntaxError, "named captures after the class, non-u");
            assert.throws(function () { new RegExp("(?<a>x)[\\k]", "u"); }, SyntaxError, "named captures, u flag");
            assert.throws(function () { new RegExp("\\k<a>[\\k](?<a>x)"); }, SyntaxError, "valid reference elsewhere, non-u");
            assert.throws(function () { new RegExp("[^\\k]", "u"); }, SyntaxError, "negated class, u flag");
            assert.throws(function () { new RegExp("[a-\\k]", "u"); }, SyntaxError, "range endpoint, u flag");
            assert.throws(function () { eval("/\\k/u"); }, SyntaxError, "bare \\k under u still throws");
            assert.isTrue(/\k/.test("k"), "bare \\k stays the identity escape without named captures");
            var recompiled = new RegExp("x");
            recompiled.compile("[\\k]");
            assert.isTrue(recompiled.test("k"), "compile() keeps the identity escape");
        }
    },
    {
        name: "dollar is a valid character in \\k<name> references",
        body() {
            // '$' is an identifier character in group names (the same
            // grammar as the (?<name>...) definitions), so the \k span scan
            // must not stop on it.
            assert.areEqual("xaxb", "xaxb".replace(new RegExp("(?<$a>x)\\k<$a>"), "$<$a>$<$a>"), "leading dollar: definition and reference resolve");
            assert.areEqual("xx", new RegExp("(?<a$>x)\\k<a$>", "g").exec("xx")[0], "trailing dollar under flags");
            assert.areEqual("xxyy", "xxyy".replace(new RegExp("(?<a$b>xy)\\k<a$b>"), "$<$a>$<$a>"), "interior dollar");
            assert.areEqual("xx", new RegExp("(?<$>x)\\k<$>").exec("xx")[0], "dollar-only name matches");
            assert.areEqual("x", new RegExp("(?<$>x)\\k<$>").exec("xx").groups.$, "groups property with the dollar-only name");
            assert.throws(function () { new RegExp("(?<a>x)\\k<a$"); }, SyntaxError, "unterminated after dollar still throws");
            assert.isTrue(new RegExp("\\k<a$").test("k<a"), "identity fallback without named captures crosses the dollar");
        }
    },];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
