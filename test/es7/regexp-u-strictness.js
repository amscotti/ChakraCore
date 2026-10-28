//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2015 u-mode RegExp strictness (S9 residual): extended pattern characters
// (`{`/`}`/`]`), legacy octal and out-of-bounds decimal escapes, quantifiable
// assertions, malformed `\u{...}`/`\x`/`\u` escape bodies, the surrogate
// code-point semantics of inverted astral classes, the u-mode dot/shorthand
// complements and backreferences, and (S9-b) code-point-aware literal search:
// trail-surrogate literal/set/alternation scans, pair literals and spec
// pair-splitting manual `lastIndex` (ES2026 22.2.7.2 step 13.b), global and
// sticky walks, and search/split/replace/exec-walk advancement. Non-u Annex B
// behavior is pinned unchanged (lone `{`/`}`/`]` literals, octal escapes,
// quantified lookaheads, per-unit astral scanning), and `v` (which implies
// the whole u-mode surface) follows `u`. Every assertion was mirrored in
// Node 26.5.0 and Bun 1.4.3 first; the two oracles agreed on all of them.

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
        name: "Trail-surrogate literal scans never start mid-pair under u/v",
        body() {
            // The literal/prefix/set-scan fast paths advance per code point:
            // a pattern whose first unit is a trail surrogate can no longer
            // match starting on the second half of a complete pair.
            assert.areEqual(null, /\uDE00X/u.exec("\ud83d\ude00X"), "trail 2-char literal vs pair");
            assert.areEqual(null, /\uDE00X/iu.exec("\ud83d\ude00X"), "trail 2-char literal vs pair (i)");
            assert.areEqual(null, /\uDE00X/v.exec("\ud83d\ude00X"), "trail 2-char literal vs pair (v)");
            assert.areEqual(null, /\uDE00XY/u.exec("\ud83d\ude00XY"), "trail 3-char literal vs pair");
            assert.areEqual(null, new RegExp("\\uDE00X", "u").exec("\ud83d\ude00X"), "dynamic trail literal vs pair");
            assert.areEqual(null, /\uDE00X/u.exec("q\ud83d\ude00X"), "trail literal cannot relocate past the pair");
            assert.areEqual(null, /\uDE00X/u.exec("\ud800\udc00\ud83d\ude00X"), "trail literal after a real pair still fails");
            assert.areEqual(0, /\uDE00X/u.exec("\ude00X").index, "trail literal matches a lone trail");
            assert.areEqual(null, /[\uDE00x]a/u.exec("\ud83d\ude00a"), "set-scan trail member + literal");
            assert.areEqual("\ude00a", /[\uDE00x]a/u.exec("\ude00a")[0], "set-scan finds the lone trail");
            assert.areEqual(null, /a|\uDE00/u.exec("\ud83d\ude00"), "alternation first-set trail member");
            assert.areEqual("q", /q|\uDE00X/u.exec("\ud83d\ude00Xq")[0], "alternation picks the later real branch");
            assert.areEqual(3, /q|\uDE00X/u.exec("\ud83d\ude00Xq").index, "alternation match index");
            // single-char program path (previously fixed) stays green
            assert.areEqual(null, new RegExp("\\uDE00", "u").exec("\ud800\ude00"), "single-char trail vs pair");
            // non-u keeps the Annex B per-unit scan exactly
            assert.areEqual("\ude00X", /\uDE00X/.exec("\ud83d\ude00X")[0], "non-u trail literal matches the half");
            assert.areEqual(1, /\uDE00X/.exec("\ud83d\ude00X").index, "non-u match index is the half");
        }
    },
    {
        name: "Full pair literals scan and match whole code points",
        body() {
            assert.areEqual("\ud801\udc00", /\u{10400}/u.exec("\ud801\udc00")[0], "pair literal matches the pair");
            assert.areEqual(0, /\u{10400}/u.exec("\ud801\udc00").index, "pair literal index");
            assert.areEqual(2, /\u{10400}/u.exec("\ud800\udc00\ud801\udc00").index, "pair literal skips the wrong pair");
            assert.areEqual(2, /\u{1F600}x/u.exec("\ud83d\ude00\ud83d\ude00x").index, "pair literal with ASCII suffix");
            assert.areEqual("\ud83d\ude00x", /\u{1F600}x/u.exec("\ud83d\ude00\ud83d\ude00x")[0], "pair literal + suffix consumes both units");
        }
    },
    {
        name: "Manual mid-pair lastIndex snaps to the containing code point",
        body() {
            // ES2026 22.2.7.2 step 13.b: the attempt starts at the code point
            // containing element lastIndex (Node/Bun parity).
            var re;
            re = /\uDC00/gu; re.lastIndex = 1;
            assert.areEqual(null, re.exec("\ud800\udc00"), "trail pattern mid-pair lastIndex fails");
            assert.areEqual(0, re.lastIndex, "failed global exec resets lastIndex");
            re = /\u{10400}/gu; re.lastIndex = 1;
            var m = re.exec("\ud801\udc00\ud801\udc00");
            assert.areEqual(0, m.index, "pair-literal global snaps back to 0");
            assert.areEqual(2, m[0].length, "snapped match consumes the pair");
            assert.areEqual(2, re.lastIndex, "lastIndex is the pair end");
            re = /\u{10400}/yu; re.lastIndex = 1;
            m = re.exec("\ud801\udc00");
            assert.areEqual(0, m.index, "sticky snaps a mid-pair lastIndex too");
            assert.areEqual(2, re.lastIndex, "sticky lastIndex after snapped pair match");
            re = /\u{10400}/gu; re.lastIndex = 2;
            m = re.exec("\ud801\udc00\ud801\udc00");
            assert.areEqual(2, m.index, "boundary lastIndex keeps the second pair");
            assert.areEqual(4, re.lastIndex, "boundary lastIndex advances to the end");
            re = /\u{10400}/gu; re.lastIndex = 3;
            m = re.exec("\ud801\udc00\ud801\udc00");
            assert.areEqual(2, m.index, "lastIndex mid second pair snaps back");
            re = /\uDE00X/gu; re.lastIndex = 1;
            assert.areEqual(null, re.exec("\ud83d\ude00X"), "trail literal mid-pair lastIndex fails");
            re = /\uDC00/yu; re.lastIndex = 1;
            assert.areEqual(null, re.exec("\ud800\udc00"), "sticky trail mid-pair fails");
            assert.areEqual(0, re.lastIndex, "failed sticky exec resets lastIndex");
            re = /X/yu; re.lastIndex = 1;
            m = re.exec("\ud800X");
            assert.areEqual(1, m.index, "non-trail sticky at a non-splitting offset is unchanged");
            re = /()/gu; re.lastIndex = 1;
            m = re.exec("\ud801\udc00");
            assert.areEqual(0, m.index, "empty pattern mid-pair lastIndex snaps");
            assert.areEqual(0, re.lastIndex, "empty match leaves lastIndex at its end");
        }
    },
    {
        name: "Global/sticky walks and search/split/replace stay on code-point boundaries",
        body() {
            // The String.match/search/split/replace drivers and exec-walk
            // loops (the surface matchAll drives) advance per code point.
            assert.areEqual(null, "X\ud83d\ude00Xq".match(/\uDE00X/gu), "match walk finds no in-pair trail literal");
            assert.areEqual(-1, "\u{1F600}".search(/\uDE00/u), "search single-char trail over pair");
            assert.areEqual(-1, "\u{1F600}".search(/\uDE00X/u), "search literal trail over pair");
            assert.areEqual(1, "\ud83d\ude00Xz".split(/\uDE00X/u).length, "split ignores an in-pair separator");
            assert.areEqual(2, "\ud801\udc00a".split(/\u{10400}/u).length, "split on a real pair separator");
            assert.areEqual(3, "\ud801\udc00X\ud801\udc00X".split(/\u{10400}X/u).length, "split finds both pair separators");
            assert.areEqual("\ud83d\ude00Xz", "\ud83d\ude00Xz".replace(/\uDE00X/gu, "Q"), "replace skips an in-pair match");
            assert.areEqual("\ud83d\ude00a", "\ud83d\ude00a".replace(/[\uDE00x]a/gu, "Q"), "replace skips an in-pair set match");
            assert.areEqual("\ud83d\ude00Xz", "\ud83d\ude00Xz".replace(/\uDE00X/gu, function () { return "Q"; }), "replace function skips an in-pair match");
            // empty matches advance past whole pairs (AdvanceStringIndex)
            assert.areEqual(["", ""], "\ud800\udc00".match(/x?/gu), "empty match walk yields one match per code point");
            var seen = [], re = /x?/gu, input = "\ud800\udc00", m;
            while ((m = re.exec(input))) {
                seen.push(m.index);
                if (m[0] === "") { re.lastIndex += 2; }
                if (re.lastIndex > input.length) { break; }
            }
            assert.areEqual("0,2", seen.join(","), "exec-walk empty matches sit on pair boundaries");
            seen = []; re = /\uDE00/gu; input = "\ud800\ude00\ude00";
            while ((m = re.exec(input))) {
                seen.push(m.index);
                if (re.lastIndex > input.length) { break; }
            }
            assert.areEqual("2", seen.join(","), "exec-walk trail pattern skips the pair, finds the lone trail");
            assert.areEqual(1, "\ud800\udc00".split(/(?=x)/u).length, "empty separator split keeps the pair whole");
            // sticky walks never start mid-pair
            var stickySeen = [];
            re = /\uDE00X/yu; input = "\ud83d\ude00X";
            for (var i = 0; i < 4; i++) {
                m = re.exec(input);
                if (m) { stickySeen.push(m.index); }
                if (!m || re.lastIndex === 0) { break; }
            }
            assert.areEqual("", stickySeen.join(","), "sticky walk never matches the in-pair literal");
            // non-u keeps the per-unit walk exactly
            assert.areEqual(["\ude00X"], "\ud83d\ude00Xq".match(/\uDE00X/g), "non-u global walk matches the half");
            assert.areEqual(2, "\ud800\ude00Xz".split(/\uDE00X/).length, "non-u split on the half");
        }
    },
    {
        name: "Anchors and alternation over surrogate literals",
        body() {
            assert.areEqual("a", /a|\uDE00/u.exec("a\ud83d\ude00")[0], "alternation lead branch wins");
            assert.areEqual(0, /a|\uDE00/u.exec("a\ud83d\ude00").index, "alternation lead branch index");
            assert.areEqual(null, /^\uDE00X/u.exec("\ud83d\ude00X"), "anchored trail literal cannot match mid-pair");
            assert.areEqual(2, /X/u.exec("\ud800\udc00X").index, "plain search after a pair lands on the boundary");
            assert.areEqual(0, /\u{10400}$/u.exec("\ud801\udc00").index, "dollar-anchored pair literal");
            assert.areEqual(null, /\b\uDE00/u.exec("x\ud83d\ude00"), "word-boundary trail literal fails");
            assert.areEqual(0, /(?:\u{10400}|q)/u.exec("\ud801\udc00q").index, "alternation with pair literal picks the first branch");
            assert.areEqual(3, /^X/um.exec("\ud800\udc00\nX").index, "multiline anchor after a pair + newline");
        }
    },
    {
        name: "Lone surrogates keep their own code-point matches",
        body() {
            assert.areEqual("\ude00", /\uDE00/u.exec("\ude00")[0], "lone trail direct");
            assert.areEqual(2, /\uDE00/u.exec("\ud800\udc00\ude00").index, "lone trail after a complete pair");
            assert.areEqual("\udc00", /[\uDC00-\uDFFF]/u.exec("a\udc00")[0], "lone trail class scan");
            assert.areEqual(1, /[\uDC00-\uDFFF]/u.exec("a\udc00").index, "lone trail class index");
            assert.areEqual("\ud800", /[\uD800]/u.exec("\ud800x")[0], "lone lead keeps matching");
            assert.areEqual(2, "\ud800\udc00\ude00".split(/\uDE00/u).length, "split on a lone trail");
            assert.areEqual("aQ", "a\ude00".replace(/\uDE00/gu, "Q"), "replace a lone trail");
            assert.areEqual("Qa", "\ud800a".replace(/\uD800/gu, "Q"), "replace a lone lead");
            var seen = [], re = /\uDE00/gu, input = "\ud801\udc00\ud801\udc00\ude00", m;
            while ((m = re.exec(input))) {
                seen.push(m.index);
                if (re.lastIndex > input.length) { break; }
            }
            assert.areEqual("4", seen.join(","), "walk over pairs finds only the lone trail");
            // non-u per-unit semantics preserved end to end
            assert.areEqual("\udc00", /\uDC00/.exec("\ud800\udc00")[0], "non-u single-char per-unit scan");
            assert.areEqual(1, /\uDC00/.exec("\ud800\udc00").index, "non-u index is the trail unit");
        }
    },
    {
        name: "v-mode empty-capable walks advance per code point (matchAll substrate)",
        body() {
            // The @@match/@@replace/@@split drivers and the matchAll iterator
            // derive fullUnicode from flags containing "u" OR "v" (ES2026);
            // v implies the whole u-mode surface, and the `unicode` accessor
            // alone reports false for a v-only regex. Before the S9-b repair
            // the per-unit +1 empty-match advance fought the pair-start snap
            // in Matcher::Match and could loop forever. These shapes run in
            // default mode (the -ES6RegExSymbols matchAll forms are pinned in
            // test/es7/matchall.js); every value mirrors Node 26.5.0.
            var input = "\ud83d\ude00X"; // [pair][X]
            assert.areEqual("\ud83d\ude00,X", input.split(/\uDE00?/gv).join(","), "v empty split keeps the pair whole");
            assert.areEqual("\ud83d\ude00,X", input.split(/\uDE00?/gu).join(","), "u empty split (control)");
            assert.areEqual(",,", input.match(/\uDE00?/gv).join(","), "v empty match yields one per code point");
            assert.areEqual(",,", input.match(/\uDE00?/gu).join(","), "u empty match (control)");
            assert.areEqual(",\ude00,,", input.match(/\uDE00?/g).join(","), "no-flag match keeps the per-unit walk");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replace(/\uDE00?/gv, "Q"), "v empty replace steps whole code points");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replace(/\uDE00?/gu, "Q"), "u empty replace (control)");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replaceAll(/\uDE00?/gv, "Q"), "v replaceAll steps whole code points");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replaceAll(/\uDE00?/gu, "Q"), "u replaceAll (control)");
            assert.areEqual("Q\ud83dQQXQ", input.replace(/\uDE00?/g, "Q"), "no-flag replace keeps the per-unit walk");
            // the exec-level contract those drivers ride on: a manual
            // mid-pair lastIndex snaps to the pair start in v mode too
            var re = /\u{10400}/gv;
            re.lastIndex = 1;
            var m = re.exec("\ud801\udc00\ud801\udc00");
            assert.areEqual(0, m.index, "v pair literal with mid-pair lastIndex snaps back");
            assert.areEqual(2, re.lastIndex, "v lastIndex is the pair end");
            var re2 = /\uDE00?/gv;
            re2.lastIndex = 1;
            var m2 = re2.exec(input);
            assert.areEqual(0, m2.index, "v empty-capable pattern snaps to the pair start");
            assert.areEqual(0, m2[0].length, "v empty-capable match is empty at 0");
            assert.areEqual(0, re2.lastIndex, "v empty match leaves lastIndex at its end");
        }
    },
    {
        name: "v-mode escaped backslash is the escaped-backslash member",
        body() {
            // [\\] under v is a legal escaped ClassSetSyntaxCharacter:
            // literal and dynamic forms, plain classes, set operations and
            // \q bodies all accept it (Node 26.5.0 == Bun 1.4.3).
            var bs = String.fromCharCode(92);
            var esc = bs + bs; // one escaped backslash in a source string
            assert.isTrue(eval("/[" + esc + "]/v").test(bs), "[\\] member");
            assert.isTrue(eval("/[a" + esc + "]/v").test(bs), "[a\\] backslash member");
            assert.isTrue(eval("/[a" + esc + "]/v").test("a"), "[a\\] keeps a");
            assert.isTrue(eval("/[[a]" + "--" + esc + "]/v").test("a"), "[[a]--\\] keeps a");
            assert.isFalse(eval("/[[a]" + "--" + esc + "]/v").test(bs), "[[a]--\\] drops the backslash");
            assert.isTrue(eval("/[" + bs + "q{a" + esc + "b}]/v").test("a" + bs + "b"), "\\q body member");
            assert.areEqual(4, eval("/[" + esc + "]/v").source.length, "source is verbatim");
            assert.isTrue(new RegExp("[" + esc + "]", "v").test(bs), "dynamic [\\]");
            assert.isTrue(new RegExp("[[a]" + "--" + esc + "]", "v").test("a"), "dynamic set op");
            assert.areEqual(4, new RegExp("[" + esc + "]", "v").source.length, "dynamic source verbatim");
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
