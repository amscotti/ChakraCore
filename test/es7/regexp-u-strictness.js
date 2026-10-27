//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2015 u-mode RegExp strictness (S9 residual): extended pattern characters
// (`{`/`}`/`]`), legacy octal and out-of-bounds decimal escapes, quantifiable
// assertions, malformed `\u{...}`/`\x`/`\u` escape bodies, and the surrogate
// code-point semantics of inverted astral classes, the u-mode dot/shorthand
// complements and backreferences. Non-u Annex B behavior is pinned unchanged
// (lone `{`/`}`/`]` literals, octal escapes, quantified lookaheads, per-unit
// astral scanning), and `v` (which implies the whole u-mode surface) follows
// `u`. Every assertion was mirrored in Node 26.5.0 and Bun 1.4.3 first; the
// two oracles agreed on all of them.

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

function exec0(re, input) {
    var m = re.exec(input);
    return m === null ? null : m[0];
}

var tests = [
    {
        name: "Extended pattern characters: `{`, `}` and `]` reject under u and v, stay literal without u",
        body() {
            // The named residual: /{/u
            compileError("/{/u");
            compileError("/a{/u");
            compileError("/}/u");
            compileError("/a}/u");
            compileError("/a{}/u");
            compileError("/a{1,2,3}/u");
            dynError("{", "u");
            dynError("a{", "u");
            dynError("}", "u");
            // valid quantifiers still parse
            compileOk("/a{1,2}/u");
            compileOk("/a{2,}/u");
            compileOk("/a{2}/u");
            assert.isTrue(/a{1,2}/u.test("aa"), "valid braced quantifier under u");
            // out-of-order bounds reject in every mode
            compileError("/a{2,1}/u");
            compileError("/a{2,1}/");
            // `]` as an atom rejects under u/v (Annex B deviation under u only)
            compileError("/]/u");
            compileError("/a]/u");
            dynError("]", "u");
            compileError("/]/v");
            // v-class spans keep nested-class `]` content legal
            compileOk("/[a[b]c]/v");
            // Annex B preservation: all of these stay literals without u
            assert.areEqual("{", /{/.exec("{")[0], "non-u { is a literal");
            assert.isTrue(/a{/.test("a{"), "non-u a{ is a literal");
            assert.areEqual("}", /}/.exec("}")[0], "non-u } is a literal");
            assert.isTrue(/a{}/.test("a{}"), "non-u a{} is a literal");
            assert.isTrue(/a{1,2,3}/.test("a{1,2,3}"), "non-u malformed quantifier body is literals");
            assert.areEqual("]", /]/.exec("]")[0], "non-u ] is a literal (Annex B deviation preserved)");
            assert.areEqual("a{1,2", /a{1,2/.exec("a{1,2")[0], "non-u unterminated quantifier body is literals");
            assert.areEqual("{", new RegExp("{").exec("{")[0], "dynamic non-u { is a literal");
        }
    },
    {
        name: "Legacy octal and out-of-bounds decimal escapes reject under u, keep Annex B meaning without u",
        body() {
            // the named residuals: /\1/u and /\8/u
            compileError("/\\1/u");
            compileError("/\\8/u");
            compileError("/\\9/u");
            compileError("/\\123/u");
            compileError("/\\01/u");
            compileError("/\\08/u");
            dynError("\\1", "u");
            dynError("\\8", "u");
            // forward and backward references to existing groups stay legal
            compileOk("/(a)\\1/u");
            compileOk("/\\1(a)/u");
            assert.isTrue(/(a)\1/u.test("aa"), "backreference under u");
            assert.isTrue(/\1(a)/u.test("aa"), "forward backreference under u");
            assert.isTrue(/\2(a)(b)/u.test("ab"), "forward two-group backreference under u");
            // \0 not followed by a decimal digit stays legal in both modes
            assert.isTrue(/\0/u.test("\u0000"), "NUL escape under u");
            assert.isTrue(/[\0]/u.test("\u0000"), "NUL class escape under u");
            // inside classes octal shapes reject under u as well
            compileError("/[\\1]/u");
            compileError("/[\\00]/u");
            compileError("/[\\08]/u");
            // Annex B preservation without u
            assert.isTrue(/\1/.test("\u0001"), "non-u \\1 is the legacy octal escape");
            assert.isTrue(/\8/.test("8"), "non-u \\8 is the identity escape 8");
            assert.isTrue(/\9/.test("9"), "non-u \\9 is the identity escape 9");
            assert.isTrue(/\123/.test("S"), "non-u \\123 is the octal escape 83");
            assert.isTrue(/\01/.test("\u0001"), "non-u \\01 is the octal escape 1");
            assert.isTrue(new RegExp("\\1").test("\u0001"), "dynamic non-u octal escape");
            assert.areEqual("\u0001", /[\1]/.exec("\u0001")[0], "non-u class octal escape");
            assert.isTrue(/\0/.test("\u0000"), "non-u NUL escape");
        }
    },
    {
        name: "Quantifiable assertions reject under u and v, stay quantifiable without u",
        body() {
            // the named residuals: optional and range quantifiers on lookaheads
            compileError("/.(?=.)?/u");
            compileError("/.(?!.)?/u");
            compileError("/.(?=.){2,3}/u");
            compileError("/.(?!.){2,3}/u");
            compileError("/.(?=.)*?/u");
            dynError(".(?=.)?", "u");
            dynError(".(?!.){2,3}", "u");
            // lookbehinds are never quantifiable, in any mode
            compileError("/.(?<=.)?/u");
            compileError("/.(?<!.){2,3}/u");
            compileError("/.(?<=.)?/");
            // plain assertions still work under u
            assert.isTrue(/.(?=.)/u.test("ab"), "plain lookahead under u");
            assert.isTrue(/.(?<!a)/u.test("ba"), "plain lookbehind under u");
            // v implies the u surface
            compileError("/.(?=.)?/v");
            // Annex B preservation without u
            assert.isTrue(/.(?=.)?/.test("ab"), "non-u optional lookahead");
            assert.isTrue(/.(?=.){2,3}/.test("abc"), "non-u ranged lookahead");
            assert.isTrue(/.(?!.)?/.test("ab"), "non-u optional negative lookahead");
            assert.isTrue(/.(?!.){0,2}/.test("abc"), "non-u ranged negative lookahead");
        }
    },
    {
        name: "Malformed \\u and \\x escape bodies reject under u, stay identity escapes without u",
        body() {
            // the named residuals: /\u{1,}/u and /\u{1F_639}/u
            compileError("/\\u{1,}/u");
            compileError("/\\u{1F_639}/u");
            dynError("\\u{1,}", "u");
            // every other malformed body shape rejects as well
            compileError("/\\u{}/u");
            compileError("/\\u{/u");
            compileError("/\\u{zz}/u");
            compileError("/\\u12z/u");
            compileError("/\\uu/u");
            compileError("/\\xzz/u");
            compileError("/\\x/u");
            compileError("/[\\u12z]/u");
            compileError("/[\\xzz]/u");
            // \u{110000} and \u{10FFFF} bounds
            compileError("/\\u{110000}/u");
            assert.isTrue(/\u{10FFFF}/u.test("\u{10FFFF}"), "top code point is inclusive");
            // valid escapes keep working
            assert.isTrue(/\u{1F639}/u.test("\u{1F639}"), "well-formed \\u{...} under u");
            assert.isTrue(/\u0041/u.test("A"), "well-formed \\uXXXX under u");
            assert.isTrue(/[\u{1F639}]/u.test("\u{1F639}"), "well-formed class \\u{...} under u");
            assert.isTrue(/\x41/u.test("A"), "well-formed \\xNN under u");
            // Annex B preservation without u: `u`/`x` identity escape + literals
            assert.isTrue(/\u{1,}/.test("u{1,}"), "non-u malformed \\u{...} is identity + literals");
            assert.isTrue(/\u{1F_639}/.test("u{1F_639}"), "non-u numeric separator body is literals");
            assert.isTrue(/\u12z/.test("u12z"), "non-u short \\uXXXX is identity + literals");
            assert.isTrue(/\xzz/.test("xzz"), "non-u malformed \\xNN is identity + literals");
            assert.isTrue(/[\xzz]/.test("x"), "non-u class malformed \\xNN keeps parsing");
        }
    },
    {
        name: "Inverted astral classes match whole code points: no surrogate-half matches",
        body() {
            // the named residual: all four excluded-astral assertions return null
            assert.areEqual(null, /^[^❤️]$/u.exec("❤️"), "excluded astral+VS16 pair (both BMP units)");
            assert.areEqual(null, /^[^🧡]/u.exec("🧡"), "start-anchored exclusion");
            assert.areEqual(null, /[^💛]$/u.exec("💛"), "end-anchored exclusion");
            assert.areEqual(null, /[^💚]/u.exec("💚"), "unanchored exclusion");
            // search advances per code point: the next real code point is found
            assert.areEqual("x", /[^💚]/u.exec("💚x")[0], "scan resumes after the excluded pair");
            assert.areEqual(null, /[^🧡x]/u.exec("🧡"), "excluded pair with sibling member");
            assert.areEqual("y", /[^🧡x]/u.exec("y")[0], "non-member still matches");
            // lone surrogates are their own code points
            assert.areEqual("\ud83d", /[^💚]/u.exec("\ud83d")[0], "a lone high half is its own member");
            assert.areEqual("a", /[^💚]/u.exec("a\udc9a")[0], "member before a lone low half");
            assert.areEqual("\udc9a", /[\udc00-\udfff]/u.exec("a\udc9a")[0], "lone low surrogate is a member");
            // Positive low-surrogate classes over a complete pair keep the
            // engine's documented per-unit scan start (see the receipt's
            // residuals); negated classes are pair-exact as above.
            // negated BMP-only surrogate classes match the whole astral pair
            assert.areEqual("💚", /[^\ud800-\udbff]/u.exec("\ud83d\udc9a")[0], "astral pair is in the complement");
            // v follows u
            assert.areEqual(null, /[^💚]/v.exec("💚"), "v-mode exclusion");
            assert.areEqual("x", /[^💚]/v.exec("💚x")[0], "v-mode scan resumes after the pair");
            // dynamic construction agrees
            assert.areEqual(null, new RegExp("[^💚]", "u").exec("💚"), "dynamic exclusion");
            // non-u keeps the Annex B per-unit behavior exactly
            assert.areEqual(null, /[^🧡]/.exec("🧡"), "non-u negated class excludes both halves");
            assert.areEqual("\ud83e", /\D/.exec("🧡")[0], "non-u \\D matches the high half");
        }
    },
    {
        name: "u-mode dot and shorthand complements consume whole code points",
        body() {
            // deterministic single-interpretation atoms (no pair/high-half ambiguity)
            assert.areEqual("\ud800\udc00", /./u.exec("\ud800\udc00")[0], "dot matches the pair");
            assert.areEqual("\ud800\udc00\ud800", /.*/u.exec("\ud800\udc00\ud800")[0], "dot run over pair + lone high");
            assert.areEqual("🧡", /\D/u.exec("🧡")[0], "\\D matches the pair");
            assert.areEqual("🧡", /\S/u.exec("🧡")[0], "\\S matches the pair");
            assert.areEqual("🧡", /\W/u.exec("🧡")[0], "\\W matches the pair");
            assert.areEqual("🧡", /./u.exec("🧡")[0], "dot over a real astral char");
            assert.isTrue(/^\S$/u.test("\ud800\udc00"), "anchored \\S over the pair (test262 pin)");
            assert.isTrue(/^.$/u.test("\ud800\udc00"), "anchored dot over the pair (test262 pin)");
            // v follows u
            assert.areEqual("\ud800\udc00", /./v.exec("\ud800\udc00")[0], "v dot matches the pair");
            // non-u keeps per-unit matching exactly
            assert.areEqual("\ud800", /./.exec("\ud800\udc00")[0], "non-u dot matches one unit");
            assert.areEqual("\ud800", /\D/.exec("\ud800\udc00")[0], "non-u \\D matches one unit");
        }
    },
    {
        name: "Backreferences canonicalize over surrogate pairs under u",
        body() {
            // the named residual: the lone trailing high surrogate cannot satisfy
            // a backreference holding the full pair
            assert.isFalse(/(.+).*\1/u.test("\ud800\udc00\ud800"), "pair backreference vs lone high surrogate");
            assert.isTrue(/(.+).*\1/u.test("\ud800\udc00\ud800\udc00"), "two equal pairs satisfy the backreference");
            assert.isTrue(/(\ud800)\1/u.test("\ud800\ud800"), "lone high surrogate captures and repeats");
            assert.isTrue(/(\u{10429})\1/u.test("\u{10429}\u{10429}"), "astral literal pair backreference");
            assert.isFalse(/(\u{10429}a)\1/u.test("\u{10429}a\u{10429}"), "shorter input fails (existing pin)");
            // non-u keeps the per-unit partition exactly
            assert.isTrue(/(.+).*\1/.test("\ud800\udc00\ud800"), "non-u backreference over half-open pair");
        }
    },
    {
        name: "u-mode rejects v nested-class syntax; v and non-u keep their readings",
        body() {
            // Nested classes, set operations and \q are v grammar: plain u
            // rejects (Node/Bun agree), v accepts, non-u keeps the Annex B
            // legacy class reading.
            compileError("/[[a]]/u");
            compileError("/[a[b]c]/u");
            compileError("/[[a]--[b]]/u");
            compileError("/[^[a]]/u");
            compileError("/[[ab][cd]]/u");
            dynError("[[a]]", "u");
            dynError("[a[b]c]", "u");
            dynError("[[a]--[b]]", "u");
            compileOk("/[[a]]/v");
            compileOk("/[a[b]c]/v");
            compileOk("/[[a]--[b]]/v");
            compileOk("/[^[a]]/v");
            assert.isTrue(/[a[b]c]/v.test("b"), "v nested-class member");
            compileOk("/[[a]]/");
            compileOk("/[a[b]c]/");
            assert.isFalse(new RegExp("[[a]]").test("]"), "non-u legacy class does not match ]");
            assert.isTrue(new RegExp("[a[b]").test("["), "non-u legacy class member [");
        }
    },
    {
        name: "v-mode escaped class punctuation: only escapes and ClassSet punctuation",
        body() {
            // An escaped character in a v class must be a CharacterEscape
            // or escaped ClassSet punctuation (syntax characters, reserved
            // double-punctuator components, '-' and '^'). Everything else
            // rejects. eval sources carry ONE backslash before the char.
            var bs = String.fromCharCode(92);
            var rejects = ['"', "'", " ", "k", "e", "_", "3"];
            for (var i = 0; i < rejects.length; i++) {
                (function (c) {
                    assert.throws(function () { eval("/[" + bs + c + "]/v"); }, SyntaxError, "v class reject \\" + c);
                })(rejects[i]);
            }
            var accepts = ["&", "-", "/", "!", "#", "%", "~", "{", "}", "^", "`", "(", ")", "[", "]", "|", "*", "+", ",", ".", ":", ";", "<", "=", ">", "?", "@"];
            for (var j = 0; j < accepts.length; j++) {
                (function (c) {
                    assert.doesNotThrow(function () { eval("/[" + bs + c + "]/v"); }, "v class accept \\" + c);
                })(accepts[j]);
            }
            // valid escapes and semantics keep working
            assert.isTrue(new RegExp("[" + bs + "d]", "v").test("5"));
            assert.isTrue(new RegExp("[" + bs + "b" + bs + "-d]", "v").test("\u0008"));
            assert.isTrue(new RegExp("[a" + bs + "-c]", "v").test("-"), "escaped dash is a literal member");
            assert.isTrue(new RegExp("[" + bs + "^]", "v").test("^"));
            // plain u also rejects the non-identity leftovers (existing rule)
            assert.throws(function () { eval("/[" + bs + "k]/u"); }, SyntaxError, "u class reject \\k");
        }
    },
    {
        name: "Sticky single-char at the input end: no out-of-bounds read",
        body() {
            // exec at lastIndex == length must fail without reading past the
            // input (the terminating NUL of flat strings / the parent's next
            // character of slices).
            var re;
            re = /\0/y; re.lastIndex = 0;
            assert.areEqual(null, re.exec(""), "empty input, lastIndex 0 == length");
            re = /\0/y; re.lastIndex = 2;
            assert.areEqual(null, re.exec("ab"), "flat input at end");
            re = /\0/gy; re.lastIndex = 2;
            assert.areEqual(null, re.exec("ab"), "global sticky at end");
            re = /A/iy; re.lastIndex = 0;
            assert.areEqual(null, re.exec(""), "case-insensitive empty at end");
            var parent = "ab\u0000cd";
            var child = parent.slice(0, 2); // storage ends where the parent keeps \u0000
            assert.areEqual(2, child.length, "slice length control");
            re = /\0/y; re.lastIndex = child.length;
            assert.areEqual(null, re.exec(child), "sliced child at its end");
            // positive controls: the same patterns do match when the char
            // really is at lastIndex
            assert.areEqual("\u0000", /\0/y.exec("\u0000x")[0], "sticky matches at 0");
            assert.areEqual("\u0000", /\0/y.exec("xx\u0000yy".slice(2))[0], "sticky matches sliced start");
            re = /\0/gy;
            assert.areEqual("\u0000", re.exec("\u0000a")[0], "global sticky at 0");
        }
    },
    {
        name: "Atom identity escapes under u reject in every parse mode",
        body() {
            var bs = String.fromCharCode(92);
            var rejects = ["e", "-", " ", "M", "q", "A"];
            for (var i = 0; i < rejects.length; i++) {
                (function (c) {
                    assert.throws(function () { eval("/" + bs + c + "/u"); }, SyntaxError, "identity \\" + c + " under u");
                    assert.throws(function () { new RegExp(bs + c, "u"); }, SyntaxError, "dynamic identity \\" + c);
                })(rejects[i]);
            }
            // SyntaxCharacter, '/', ZWNJ/ZWJ and the pseudo-escapes stay legal
            assert.isTrue(eval("/" + bs + "{/u").test("{"), "\\{ under u");
            assert.isTrue(eval("/" + bs + "}/u").test("}"), "\\} under u");
            assert.isTrue(eval("/" + bs + "u200C/u").test("\u200C"), "ZWNJ under u");
            assert.isTrue(eval("/" + bs + "u200D/u").test("\u200D"), "ZWJ under u");
            assert.isTrue(eval("/" + bs + "d/u").test("5"), "\\d under u");
            assert.isTrue(eval("/" + bs + "b/u").test("ab"), "\\b under u");
            assert.isTrue(new RegExp(bs + "/", "u").test("/"), "\\/ under u");
            // non-u Annex B identity escapes preserved
            assert.isTrue(eval("/" + bs + "e/").test("e"), "non-u \\e");
            assert.isTrue(eval("/" + bs + "-/").test("-"), "non-u \\-");
            assert.isTrue(eval("/" + bs + "M/").test("M"), "non-u \\M");
            // deferred constructor bodies agree (the no-AST scan records the
            // rejection, so the reparse throws a clean SyntaxError)
            assert.throws(function () { (new Function("return /" + bs + "e/u"))(); }, SyntaxError, "deferred \\e");
            assert.throws(function () { (new Function("return /" + bs + "-/u"))(); }, SyntaxError, "deferred \\-");
            assert.throws(function () { (new Function("return /" + bs + " /u"))(); }, SyntaxError, "deferred \\ ");
        }
    },
    {
        name: "Single-char scan advances per code point under u/v",
        body() {
            // A trail-surrogate single-char pattern cannot match inside a
            // complete surrogate pair (the fast path now skips pairs).
            assert.areEqual(null, new RegExp("\\uDE00", "u").exec("\ud800\ude00"), "trail surrogate vs pair");
            assert.areEqual(null, new RegExp("\\uDE00", "iu").exec("\ud800\ude00"), "trail surrogate vs pair (i)");
            assert.areEqual("\ude00", new RegExp("\\uDE00", "u").exec("\ude00")[0], "lone trail direct");
            // plain searches after a pair keep finding the next code point
            assert.areEqual("X", new RegExp("X", "u").exec("\ud800\udc00X")[0], "search after pair");
            assert.areEqual("a", new RegExp("a", "iu").exec("\ud800\udc00a")[0], "i-search after pair");
            assert.areEqual("X", new RegExp("X", "v").exec("\ud800\udc00X")[0], "v search after pair");
            assert.areEqual(null, new RegExp("X", "u").exec("\ud800\udc00"), "no false match in pair");
            // non-u keeps the per-unit scan exactly
            assert.areEqual("\udc00", /\uDC00/.exec("\ud800\udc00")[0], "non-u per-unit scan");
        }
    },
    {
        name: "Deferred parse: the new early errors fire in every parse mode",
        body() {
            // eval and dynamic forms parse at runtime, so the same sources
            // are checked identically under default, -noDeferParse,
            // -CollectGarbage and -force:deferParse launches; under the
            // deferred launches this file's own bodies are NoAST-scanned,
            // where the valid literals above prove the scan does not
            // false-reject.
            var sources = ["/{/u", "/\\1/u", "/\\8/u", "/.(?=.)?/u", "/\\u{1,}/u",
                           "/\\u{1F_639}/u", "/]/u", "/[\\M]/u", "/[\\8]/u", "/[\\q{a}]/u",
                           "/[[a]]/u", "/[a[b]c]/u", "/\\e/u", "/\\-/u", "/\\ /u"];
            for (var i = 0; i < sources.length; i++) {
                (function (src) {
                    assert.throws(function () { eval(src); }, SyntaxError, "eval " + src);
                })(sources[i]);
            }
            var dyn = [["{", "u"], ["\\1", "u"], ["\\8", "u"], [".(?=.)?", "u"],
                       ["\\u{1,}", "u"], ["]", "u"], ["[\\M]", "u"], ["a{", "u"]];
            for (var j = 0; j < dyn.length; j++) {
                (function (p, f) {
                    assert.throws(function () { new RegExp(p, f); }, SyntaxError, "dynamic " + p);
                })(dyn[j][0], dyn[j][1]);
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
