//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2025 scoped RegExp modifiers (?ims-ims:Disjunction)
// (doc/c2-s9-modifiers.md). The modifier group is non-capturing, its
// add/remove flags apply only to the modified disjunction (nested groups
// update the current state), and the top-level flags remain the base.
// The `v`/property-of-strings interactions need S3 and stay out of this
// file. Every assertion was mirrored in Node 26.5.0 first. Symbols-flag
// surfaces (matchAll etc.) stay in the -symbols files' territory; this
// file runs on the default launch.

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
            compileOk("/(?i:a)/");
            compileOk("/(?-i:a)/");
            compileOk("/(?im-s:a)/");
            compileOk("/(?i-:a)/", "empty remove flags");
            compileOk("/(?-m:a)/");
            compileOk("/(?ims:a)/");
            compileOk("/(?i:a|b)/");
            compileOk("/(?i:)/", "empty body");
            compileOk("/(?i:(?m:(?-s:a)))/", "nested");
            compileOk("/(?:a)/", "plain non-capturing group still works");
            compileOk("new RegExp('(?i:a)')");
            compileOk("new RegExp('(?im-s:a)', 'g')");
            compileOk("/(?i:a)/.compile('(?-i:b)')");

            compileError("/(?-:a)/", "both flag lists empty");
            compileError("/(?i)/", "missing colon");
            compileError("/(?im)/", "missing colon after flags");
            compileError("/(?i:a/", "unterminated body");
            compileError("/(?i-i:a)/", "flag in both lists");
            compileError("/(?i-a:a)/", "flag in both lists, single add");
            compileError("/(?ims-m:a)/", "flag in both lists, multi add");
            compileError("/(?ii:a)/", "duplicate add flag");
            compileError("/(?-ii:a)/", "duplicate remove flag");
            compileError("/(?s-s:a)/", "duplicate across lists");
            compileError("/(?d:a)/", "d is not a modifier");
            compileError("/(?g:a)/", "g is not a modifier");
            compileError("/(?u:a)/", "u is not a modifier");
            compileError("/(?y:a)/", "y is not a modifier");
            compileError("/(?I:a)/", "uppercase I is not a modifier");
            compileError("/(?i+ :a)/", "invalid text after flags");
            compileError("/(?-x:a)/", "invalid remove flag");
            compileError("new RegExp('(?-:a)')");
            compileError("new RegExp('(?i)')");
            compileError("new RegExp('(?i-i:a)')");
        }
    },
    {
        name: "Modifier groups are non-capturing and do not consume group numbers",
        body() {
            var m = "ab".match(/(?i:(a))(b)/);
            assert.areEqual(3, m.length, "overall match plus two captures visible");
            assert.areEqual("a", m[1], "modified group's capture");
            assert.areEqual("b", m[2], "following capture keeps number 2");

            m = "abc".match(/(a)(?i:(b))(c)/);
            assert.areEqual("a|b|c", m[1] + "|" + m[2] + "|" + m[3], "numbering across modifiers");
            assert.areEqual(undefined, /(?i:a)/.exec("a")[1], "modifier group itself captures nothing");

            assert.areEqual(true, /(?i:(a))\1/.test("aa"), "backreference to capture 1");
            assert.areEqual(false, /(?i:(a))\1/.test("aA"), "reference outside modifier is still case-sensitive");
            assert.areEqual(true, /(?i:(a)\1)/.test("aA"), "reference inside modifier ignores case");

            var dup = /(?<x>a)(?i:(b))(?<y>c)/.exec("abc");
            assert.areEqual("a|b|c", dup.groups.x + "|" + dup[2] + "|" + dup.groups.y, "named groups unaffected");
        }
    },
    {
        name: "Adding and removing i: literals, classes, escapes, captures",
        body() {
            assert.isTrue(/(?i:a)b/.test("Ab"), "modified literal ignores case");
            assert.isFalse(/(?i:a)b/.test("AB"), "sibling literal is case-sensitive");
            assert.isTrue(/(?i:ab)/.test("AB"), "multi-character literal inside modifier");
            assert.isTrue(/(?i:[ab])c/.test("Ac"), "character class inside modifier");
            assert.isTrue(/(?i:\x61)b/.test("Ab"), "hex escape inside modifier");
            assert.isTrue(/(?i:\u0061)b/.test("Ab"), "unicode escape inside modifier");
            assert.isTrue(/(?i:\u{0061})b/u.test("Ab"), "code point escape inside modifier");

            assert.isFalse(/(?-i:fo)o/i.test("FOO"), "removed i on the modified prefix");
            assert.isTrue(/(?-i:fo)o/i.test("foo"), "removed i, lowercase input");
            assert.isTrue(/(?-i:fo)o/i.test("foO"), "sibling keeps top-level i");
            assert.isFalse(/(?-i:[a-c])/i.test("A"), "class inside removed i");

            assert.areEqual("A", /(?i:(a))/.exec("A")[1], "captured text is the input text");
            assert.areEqual("A", /(?i:(?<x>a))/.exec("A").groups.x, "named capture is the input text");
        }
    },
    {
        name: "Adding and removing i: word characters and boundaries under u",
        body() {
            assert.isTrue(/(?i:\w)/ui.test("\u212a"), "long-s/Kelvin are word chars under added iu");
            assert.isFalse(/(?-i:\w)/ui.test("\u212a"), "removed i restores ASCII word chars");
            assert.isTrue(/(?-i:\W)/ui.test("\u212a"), "removed i makes Kelvin a non-word char");
            assert.isTrue(/(?i:\b)/u.test("\u017f"), "boundary after long s under added iu");
            assert.isFalse(/(?-i:\b)/ui.test("\u017f"), "boundary after long s with i removed");
            assert.isTrue(/(?i:Z\B)/u.test("Z\u212a"), "non-boundary between Z and Kelvin under added iu");
            assert.isFalse(/(?-i:Z\B)/ui.test("Z\u212a"), "removed i makes it a boundary");
            assert.isTrue(/(?i:\w+)/ui.test("\u017fA"), "quantified added-iu word run");
        }
    },
    {
        name: "Adding and removing s (dotAll)",
        body() {
            assert.isTrue(/(?s:.)/.test("\n"), "dot matches newline under added s");
            assert.isFalse(/(?-s:.)/s.test("\n"), "dot stops matching newline under removed s");
            assert.isTrue(/a(?s:.)c/.test("a\nc"), "added s inside a pattern");
            assert.isFalse(/a(?-s:.)c/s.test("a\nc"), "removed s inside an s pattern");
            assert.isFalse(/a(?-s:.)c/.test("a\nc"), "removed s without top-level s");
            assert.isTrue(/(?s:.)|./.test("\n"), "added s applies to its alternative");
            assert.isFalse(/(?s:.(?-s:.))/.test("\n\n"), "nested removed s wins inside added s");
            assert.isTrue(/^(?s:.)$/.test("\n"), "anchored added-s dot");
            assert.isTrue(/(?s:^.$)/.test("\r"), "carriage return with added s");
        }
    },
    {
        name: "Adding and removing m (multiline anchors)",
        body() {
            assert.isTrue(/(?m:es$)/.test("es\ns"), "added m lets $ match before a newline");
            assert.isFalse(/es$/.test("es\ns"), "top-level $ stays non-multiline");
            assert.isFalse(/(?-m:es$)/m.test("es\ns"), "removed m on a top-level m pattern");
            assert.isTrue(/^a\n(?m:^b$)\nc$/.test("a\nb\nc"), "anchors inside added m");
            assert.isFalse(/^a\n(?m:^b$)\nc$/.test("\na\nb\nc"), "^ outside the modifier stays at start");
            assert.isFalse(/^a\n(?m:^b$)\nc$/.test("a\nb\nc\n"), "$ outside the modifier stays at end");
            assert.isTrue(/(?m:^b$)/.test("a\nb\nc"), "unanchored search with added m");
            assert.isFalse(/(?m:^b$)/.test("a\nbc"), "added m does not make ^ match mid-line");
            assert.isTrue(/(?-m:^b$)/m.test("b"), "removed m still accepts the input start");
            assert.isFalse(/(?-m:^b$)/m.test("a\nb"), "removed m rejects a post-newline start");
        }
    },
    {
        name: "The properties still report only TopLevel flags",
        body() {
            assert.isFalse(/(?i:a)/.ignoreCase, "ignoreCase property");
            assert.isFalse(/(?m:a)/.multiline, "multiline property");
            assert.isFalse(/(?s:a)/.dotAll, "dotAll property");
            assert.isTrue(/(?-i:a)/i.ignoreCase, "top-level i remains reported");
            assert.areEqual("/(?-i:a)/i", /(?-i:a)/i.toString(), "removed modifier does not clear the top-level flag");
            assert.areEqual("(?i:a)b", /(?i:a)b/.source, "source keeps modifier syntax");
            assert.areEqual("/(?i:a)b/g", /(?i:a)b/g.toString(), "toString round-trip");
            assert.isTrue(/(?s:a)/s.dotAll, "top-level s remains reported");
        }
    },
    {
        name: "Nested add/remove updates the current state",
        body() {
            assert.isTrue(/(?m:^(?-i:a)$)/i.test("a\n"), "nested remove i inside add m");
            assert.isFalse(/(?m:^(?-i:a)$)/i.test("A\n"), "nested removal applies inside its group");
            assert.isFalse(/(?s:(?-s:^.$))/.test("\n"), "removed s inside added s");
            assert.isFalse(/(?s:(?-s:^.$))/s.test("\n"), "top-level s does not override nested removal");
            assert.isFalse(/(?s-:(?-s:^.$))/.test("\n"), "arithmetic add-s with empty remove");
            assert.isTrue(/(?s:(?-s:a).)/.test("a\n"), "outer added s applies after the inner group");
            assert.isTrue(/(?i:(?-i:a)b)/.test("ab"), "nested removal then sibling");
            assert.isFalse(/(?i:(?-i:a)b)/.test("Ab"), "nested removal blocks outer add for a");
            assert.isTrue(/(?i:(?-i:a)b)/.test("aB"), "outer add still applies to b");
            assert.isTrue(/(?i:a(?-i:b)c)/.test("aBc") === false, "inner removal leaves outer add around it");
            assert.isTrue(/(?i:a(?-i:b)c)/.test("abc"), "all lowercase");
        }
    },
    {
        name: "Sibling and outer alternatives are isolated",
        body() {
            assert.isTrue(/^(?i:a)b$/.test("Ab"), "modifier applies to its group");
            assert.isFalse(/^(?i:a)b$/.test("AB"), "following literal is outside");
            assert.isFalse(/^(?i:a)|b$/.test("B"), "later alternative is outside");
            assert.isTrue(/^(?i:a)|b$/.test("b"), "later alternative still matches case-sensitively");
            assert.isTrue(/^(?i:a|b)c$/.test("Ac"), "disjunction body is inside");
            assert.isTrue(/^(?i:a|b)c$/.test("Bc"), "second body alternative is inside");
            assert.isFalse(/^(?i:a|b)c$/.test("AB"), "trailing literal is outside");
            assert.isTrue(/^a(?i:b)$/.test("aB"), "modifier at the end");
            assert.isFalse(/^a(?i:b)$/.test("AB"), "leading literal is outside");
            assert.isFalse(/(?i:a)(?-i:b)/.test("AB"), "two sibling groups do not share state");
            assert.isTrue(/(?i:a)(?-i:b)/.test("Ab"), "two sibling groups use their own states");
            assert.isTrue(/(?i:a)(?-i:b)/i.test("AB") === false, "second sibling removes top-level i");
            assert.isTrue(/(?i:a)(?-i:b)/i.test("Ab"), "first sibling adds i, second removes it");
        }
    },
    {
        name: "Quantifiers on and inside modified groups",
        body() {
            assert.isTrue(/(?i:a)+/.test("AAA"), "plus on modified group");
            assert.isTrue(/(?i:a)*b/.test("AAAb"), "star on modified group");
            assert.isTrue(/(?i:a){2,3}/.test("AA"), "braces on modified group");
            assert.isTrue(/(?i:a){2,3}?/.test("AA"), "lazy braces");
            assert.isTrue(/(?i:a)?/.test("A"), "question on modified group");
            assert.isTrue(/(?i:a*)?/.test("AAA"), "quantifier inside modified group");
            assert.isTrue(/(?i:(ab)+)/.test("ABAB"), "quantified body inside group");
            assert.isTrue(/(?s:.){3}/.test("\n\r\n"), "added-s dot quantified");
            assert.isFalse(/(?-s:.){3}/s.test("\n\r\n"), "removed-s dot quantified");
            assert.isTrue(/(?m:^a$)+/.test("a"), "quantified multiline anchor group");
        }
    },
    {
        name: "Lookahead and lookbehind bodies observe the local flags",
        body() {
            assert.isTrue(/(?i:(?=a)a)/.test("A"), "lookahead body and following atom under added i");
            assert.isTrue(/(?i:(?=a))a/.test("a"), "lookahead assertion alone matches before a");
            assert.isFalse(/(?i:(?=a))a/.test("A"), "lookahead does not change the following atom outside");
            assert.isTrue(/(?i:(?<=a)b)/.test("aB"), "lookbehind body and following atom under added i");
            assert.isTrue(/(?i:(?<=a)b)/.test("ab"), "lookbehind lowercase");
            assert.isTrue(/(?i:(?<=a)b)/.test("Ab"), "lookbehind body under added i matches the prior capital");
            assert.isFalse(/(?i:(?<=a)b)/.test("Bb"), "lookbehind body still requires an a/A before b");
            assert.isTrue(/(?m:(?<=^a)b)/.test("x\nab"), "added m is frame-aware inside lookbehind");
            assert.isFalse(/(?-m:(?<=^a)b)/m.test("x\nab"), "removed m inside lookbehind");
            assert.isTrue(/(?s:(?<=.)b)/.test("xb"), "added s inside lookbehind body");
            assert.isFalse(/(?-s:(?<=a.)b)/s.test("a\nb"), "removed s inside lookbehind body");
            assert.isTrue(/(?i:(?!(?-i:a))[a-z])/.test("A"), "nested removal inside a lookahead");
        }
    },
    {
        name: "Literal, constructor and compile parity",
        body() {
            var dyn = new RegExp("(?i:a)b");
            assert.isTrue(dyn.test("Ab"), "dynamic constructor add i");
            assert.isFalse(dyn.test("AB"), "dynamic constructor sibling isolation");

            var lit = /(?-i:a)/i;
            var fromLit = new RegExp(lit);
            assert.isFalse(fromLit.test("A"), "regex-from-regex keeps the modifier");
            assert.isTrue(fromLit.test("a"), "regex-from-regex matches lowercase");

            var re = /x/;
            re.compile("(?i:a)");
            assert.isTrue(re.test("A"), "compile installs the modifier pattern");

            re = /(?m:^a$)/;
            re.compile("(?-m:^a$)", "m");
            assert.isFalse(re.test("b\na"), "compile with top-level m plus removed m");
            assert.isTrue(re.test("a"), "compile removed-m still matches at start");

            assert.areEqual("(?i:a)", new RegExp("(?i:a)").source, "constructor source");
            assert.areEqual("/(?i:a)/g", new RegExp("(?i:a)", "g").toString(), "constructor flags");
        }
    },
    {
        name: "Modifiers inside lookbehinds remain correctly mirrored",
        body() {
            assert.isTrue(/x(?<=(?i:ab))c/.test("xABc") === false, "lookbehind is left of the match");
            assert.isTrue(/xABc/.test("xABc"), "sanity input");
            assert.isTrue(/(?<=(?i:ab))c/.test("ABc"), "added i inside lookbehind literal");
            assert.isTrue(/(?<=(?i:ab))c/.test("abc"), "added i inside lookbehind literal lowercase");
            assert.isTrue(/(?<=(?i:a.))c/.test("Axc"), "added i plus dot in lookbehind");
            assert.isTrue(/(?m:(?<=^b))c/.test("\nbc"), "added m on anchor inside lookbehind");
            assert.isFalse(/(?-m:(?<=^b))c/m.test("\nbc"), "removed m on anchor inside lookbehind");
        }
    },
    {
        name: "Scoped modifiers preserve group boundaries and sticky semantics",
        body() {
            var sticky = /(?i:a)/y;
            assert.isTrue(sticky.exec("xA") === null, "sticky pattern must not search later offsets");
            assert.areEqual(0, sticky.lastIndex, "failed sticky match resets lastIndex");
            assert.isFalse(/(?i:a)/y.test("xA"), "sticky test must not search later offsets");

            assert.isTrue(new RegExp("(?i:[\\W])", "u").exec("\u017f") === null,
                "added i inside u-mode non-word class excludes LONG S");
            assert.isTrue(new RegExp("(?i:[^\\W])", "u").exec("S") !== null,
                "added i inside negated non-word class includes S");
            assert.isTrue(new RegExp("[\\W]", "iu").exec("\u017f") === null,
                "top-level iu non-word class excludes LONG S");
        }
    },
    {
        name: "Dynamic construction of every accepted form",
        body() {
            var sources = [
                "(?i:a)", "(?-i:a)", "(?m:a)", "(?-m:a)", "(?s:a)", "(?-s:a)",
                "(?im:a)", "(?is:a)", "(?ms:a)", "(?ims:a)", "(?im-s:a)",
                "(?i-:a)", "(?m-:a)", "(?s-:a)", "(?im-:a)", "(?i:(?m:(?-s:a)))",
                "(?i:a|b)", "(?i:(a)\\1)"
            ];
            for (var i = 0; i < sources.length; i++) {
                assert.doesNotThrow(function () { new RegExp(sources[i]); }, sources[i]);
            }
            assert.isTrue(new RegExp("(?im-s:^a.$)").test("A1"), "combined add/remove dynamic");
            assert.isFalse(new RegExp("(?-i:a)", "i").test("A"), "removed i via constructor with top-level i");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
