//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2025 RegExp duplicate named capture groups (doc/c2-s9-namedgroups.md
// sub-batch 3): the same name may be bound by several capture groups when
// a disjunction separates them (never two on one alternative path); the
// last group with the name to participate supplies the groups/indices
// value (undefined when none did), $<name> resolves the same way, and
// \k<name> matches the participating group's text or the empty string
// when no group with the name participated. Every assertion was mirrored
// in Node 26.5.0 first. The -ES6RegExSymbols shapes stay in
// namedgroups-symbols.js territory (matchAll etc.).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function compileOk(source, message) {
    assert.doesNotThrow(function () { eval(source); }, message || source);
}

function compileError(source, message) {
    assert.throws(function () { eval(source); }, SyntaxError, message || source);
}

var tests = [
    {
        name: "Duplicate names: separated alternatives parse, same path does not",
        body() {
            compileOk("/(?<x>a)|(?<x>b)/", "top-level alternatives");
            compileOk("/(?<x>a)|(?<x>b)|(?<x>c)/", "three alternatives");
            compileOk("/(?<x>a)|b(?<x>c)/", "name after a prefix in the later alternative");
            compileOk("/(?<x>a)|(?:b|(?<x>c))/", "inner disjunction in the later alternative");
            compileOk("/((?<x>a)|(?<x>b))/", "inside a capturing group");
            compileOk("/(?<a>(?<x>a))|(?<x>b)/", "distinct duplicate pairs across alternatives");
            compileOk("/(?:(?<x>a)|(?<x>b))\\k<x>/", "backreference to a duplicate name");
            compileOk("/(?:(?<x>a)|(?<x>b)){2}\\k<x>/", "duplicate groups inside a loop");
            compileOk("/(?<x>)|(?<x>y)/", "empty-capturing duplicates");
            compileOk("/(?<x>a)|(?<x>b)/u", "u flag");
            compileOk("/(?:(?<\ud835\udcb3>a)|(?<\ud835\udcb3>b))\\k<\ud835\udcb3>/", "astral names");
            compileError("/(?<x>a)(?<x>b)/", "same alternative");
            compileError("/(?<x>a(?<x>b))/", "nested in the same name's group");
            compileError("/(?:(?<x>a)|(?<x>b))(?<x>c)/", "after the disjunction");
            compileError("/(?:(?<x>a)|(?<x>b))(?:(?<x>c)|d)/", "two disjunctions sequentially");
            compileError("/(?<x>a)(?:|(?<x>b))/", "optional disjunction after");
            compileError("/(?:(?<x>a)|(?<x>b))((?<x>z))?/", "optional group after");
        }
    },
    {
        name: "Duplicate names: last participating group wins, undefined when none",
        body() {
            assert.areEqual("b", /(?<x>a)|(?<x>b)/.exec("bab").groups.x, "later alternative participates");
            assert.areEqual("a", /(?<x>a)|(?<x>b)/.exec("a").groups.x, "earlier alternative participates, later one must not clobber with undefined");
            var viaThird = /x(?<a>y)|z(?<a>w)|q/.exec("q");
            assert.isTrue(Object.prototype.hasOwnProperty.call(viaThird.groups, "a"), "property exists when neither duplicate participated");
            assert.isUndefined(viaThird.groups.a, "value undefined when neither participated");
            var mixed = /(a)|(?<x>b)|(?<x>c)/.exec("c");
            assert.areEqual("c", mixed.groups.x, "named duplicates beside an unnamed group");
            assert.areEqual(undefined, mixed[1], "unnamed group undefined");
            var quantified = /(?:(?<x>a)|(?<x>b))+/.exec("ab");
            assert.areEqual("b", quantified.groups.x, "loop resets body groups each iteration");
            var nested = /(?:(?<x>a)|(?<x>b))c(?:(?<y>d)|(?<y>e))/.exec("bce");
            assert.areEqual("b", nested.groups.x, "x from the second alternative");
            assert.areEqual("e", nested.groups.y, "y from its second alternative");
        }
    },
    {
        name: "groups object: order, descriptors, null prototype",
        body() {
            var re = /(?<y>a)(?<x>a)|(?<x>b)(?<y>b)/;
            assert.areEqual("y,x", Object.keys(re.exec("aa").groups).join(","), "first textual occurrence order");
            assert.areEqual("y,x", Object.keys(re.exec("bb").groups).join(","), "order independent of which alternative matched");
            var groups = /(?<x>a)|(?<x>b)/.exec("a").groups;
            assert.areEqual(null, Object.getPrototypeOf(groups), "null prototype");
            var desc = Object.getOwnPropertyDescriptor(groups, "x");
            assert.isTrue(desc.writable && desc.enumerable && desc.configurable, "data property defaults");
            assert.areEqual("a", desc.value, "descriptor value");
        }
    },
    {
        name: "backreferences to duplicate names bind the participating group",
        body() {
            assert.areEqual("aa", /(?:(?<x>a)|(?<x>b))\k<x>/.exec("aa")[0], "first group's value");
            assert.areEqual("bb", /(?:(?<x>a)|(?<x>b))\k<x>/.exec("bb")[0], "second group's value");
            assert.areEqual(null, /(?:(?<x>a)|(?<x>b))\k<x>/.exec("abab"), "must not cross-bind");
            assert.areEqual("zz", /^(?:(?<a>x)|(?<a>y)|(?<a>z))\k<a>$/.exec("zz")[0], "three-way duplicate");
            assert.areEqual("xx", /^(?:(?<a>x)|(?<a>y)|z)\k<a>$/.exec("xx")[0], "named branch binds");
            var viaZ = /^(?:(?<a>x)|(?<a>y)|z)\k<a>$/.exec("z");
            assert.areEqual("z", viaZ[0], "no participant: reference matches empty");
            assert.isUndefined(viaZ.groups.a, "and the name is undefined");
            assert.areEqual(null, /^(?:(?<a>x)|(?<a>y)|z)\k<a>$/.exec("zz"), "empty reference still anchors");
            var looped = /(?:(?:(?<x>a)|(?<x>b))\k<x>){2}/.exec("aabb");
            assert.areEqual("b", looped.groups.x, "loop iteration two rebinds the name");
            assert.areEqual(null, /(?:(?:(?<x>a)|(?<x>b))\k<x>){2}/.exec("abab"), "iterations must agree");
            // The loop-entry capture reset (spec RepeatMatcher step 2c) is
            // what lets the reference go empty after a non-group branch:
            var reset = /^(?:(?<a>x)|(?<a>y)|z){2}\k<a>$/.exec("xz");
            assert.areEqual("xz", reset[0], "group-less iteration clears the binding");
            assert.areEqual(undefined, reset[1], "capture cleared by the second iteration");
            assert.areEqual("zy", /(?<a>x)|(?:zy\k<a>)/.exec("zy")[0], "reference in the other alternative");
            assert.areEqual("x", /\k<a>(?<a>x)|(?<a>y)/.exec("xx")[0], "forward reference to a duplicate name");
            assert.areEqual("y", /\k<a>(?<a>x)|(?<a>y)/.exec("y")[0], "forward reference matches empty in the second alternative");
            assert.areEqual(null, /(?:(?<a>x)|(?<a>y))+\k<a>/.exec("xyx"), "participating group's value must match, no empty fallback");
            assert.areEqual("xyy", /(?:(?<a>x)|(?<a>y))+\k<a>/.exec("xyy")[0], "second participant still live at the reference");
        }
    },
    {
        name: "exec/match results and $<name> substitution",
        body() {
            assert.areEqual("b,,b", /(?<x>a)|(?<x>b)/.exec("bab").slice(0, 3).join(","), "positional captures beside duplicates");
            assert.areEqual("b,b,", "bab".match(/(?<x>b)|(?<x>a)/).slice(0, 3).join(","), "non-global match");
            assert.areEqual("[a]b", "ab".replace(/(?<x>a)|(?<x>b)/, "[$<x>]"), "$<name> first alternative");
            assert.areEqual("[b]a", "ba".replace(/(?<x>a)|(?<x>b)/, "[$<x>]"), "$<name> second alternative");
            assert.areEqual("[a][a][]b", "ab".replace(/(?<x>a)|(?<x>b)/, "[$<x>][$1][$2]"), "$<name> beside numbered");
            assert.areEqual("[a][b]", "ab".replace(/(?<x>a)|(?<x>b)/g, "[$<x>]"), "global replace");
            assert.areEqual("[a][b]x[a][b]", "abxab".replaceAll(/(?<x>a)|(?<x>b)/g, "[$<x>]"), "replaceAll");
            assert.areEqual("[B]", "B".replace(/(?<x>a)|(?<x>b)/i, "[$<x>]"), "case-insensitive value");
            assert.areEqual("[]", "q".replace(/x(?<a>y)|z(?<a>w)|q/, "[$<a>]"), "non-participating name substitutes the empty string");
        }
    },
    {
        name: "replace functions receive the resolved groups object",
        body() {
            var seen = "";
            "ba".replace(/(?<x>a)|(?<x>b)/, function (matched, a, b, offset, str, groups) {
                seen += [matched, a, b, groups.x].join("|");
                return "-";
            });
            assert.areEqual("b||b|b", seen, "six arguments, resolved duplicate");
        }
    },
    {
        name: "search, split, test stay positional",
        body() {
            assert.areEqual(1, "xab".search(/(?<x>a)|(?<x>b)/), "search index");
            assert.areEqual("x,a,", "xab".split(/(?<x>a)|(?<x>b)/).slice(0, 3).join(","), "split holes for the inactive duplicate");
            assert.isTrue(/(?<x>a)|(?<x>b)/.test("bab"), "test");
            assert.areEqual("a,b", [..."ab".replace(/(?<x>a)|(?<x>b)/g, function (m) { return m; })].join(","), "replace-function identity");
        }
    },
    {
        name: "d flag: indices.groups uses the same resolution",
        body() {
            var indices = "..ab".match(/(?<x>a)|(?<x>b)/d).indices;
            assert.areEqual("2,3", indices.groups.x.join(","), "first alternative pair");
            indices = "..ba".match(/(?<x>a)|(?<x>b)/d).indices;
            assert.areEqual("2,3", indices.groups.x.join(","), "second alternative pair");
            var viaThird = "q".match(/x(?<a>y)|z(?<a>w)|q/d).indices;
            assert.isTrue(Object.prototype.hasOwnProperty.call(viaThird.groups, "a"), "indices.groups property exists");
            assert.isUndefined(viaThird.groups.a, "undefined when neither participated");
            var mixed = "aabb".match(/(?:(?:(?<x>a)|(?<x>b))\k<x>){2}/d).indices;
            assert.areEqual("2,3", mixed.groups.x.join(","), "looped duplicate's last iteration pair");
            var firstAlt = /(?<x>a)|(?<x>b)/d.exec("a");
            assert.isTrue(firstAlt.indices[1] === firstAlt.indices.groups.x, "first alternative: groups.x is indices[1]");
            var secondAlt = /(?<x>a)|(?<x>b)/d.exec("b");
            assert.isTrue(secondAlt.indices[2] === secondAlt.indices.groups.x, "second alternative: groups.x is indices[2]");
            secondAlt.indices.groups.x[0] = 9;
            assert.areEqual(9, secondAlt.indices[2][0], "mutating the named pair mutates indices[i]");
        }
    },
    {
        name: "constructor, compile and dynamic parity",
        body() {
            assert.areEqual("(?<x>a)|(?<x>b)", new RegExp("(?<x>a)|(?<x>b)").source, "constructor source");
            var recompiled = new RegExp("x");
            recompiled.compile("(?<x>b)|(?<x>c)");
            assert.areEqual("c", recompiled.exec("c").groups.x, "compile() with duplicates");
            var dynamic = new RegExp("(?:(?<x>a)|(?<x>b))\\k<x>", "g");
            assert.areEqual("bb", dynamic.exec("bb")[0], "dynamic pattern with flags");
            assert.areEqual("B", /(?<x>a)|(?<x>b)/i.exec("B").groups.x, "case-insensitive exec captures the input text");
        }
    },
    {
        name: "dangling and mixed reference errors still hold",
        body() {
            assert.throws(function () { new RegExp("(?<x>a)|(?<x>b)\\k<y>"); }, SyntaxError, "dangling reference beside duplicates");
            assert.throws(function () { new RegExp("(?<x>a)|(?<x>b)\\k"); }, SyntaxError, "bare \\k with duplicates");
            assert.throws(function () { new RegExp("(?<x>a)|(?<x>b)[\\k<x>]"); }, SyntaxError, "class \\k with duplicates");
            assert.isTrue(new RegExp("\\k<x>|a").test("k<x>"), "identity escape without named captures crosses the <");
        }
    },
    {
        name: "duplicate-name pattern finalizes after its last reference is dropped",
        body() {
            // Program::FreeBody must stride over MatchNamedGroup's inline id
            // list. Group ids 1 and 6 make the short stride look like
            // JumpIfNotSet and free a forged character set. Eval so the
            // RegExp dies with the eval frame; a local in this function can
            // stay visible to the stack scan. The dynamic-regex map drops the
            // pattern on GC only after an inline cache has been used.
            eval([
                "(function () { var o = { a: 1 }; return o.a; })();",
                "var re = new RegExp('(?<a>a)(?<b>b)(?<c>c)(?<d>d)(?<e>e)|(?<a>f)\\\\k<a>X');",
                "if (re.exec('ffX')[0] !== 'ffX' || re.exec('ffX').groups.a !== 'f') throw new Error('exec');",
                "re = null;",
                "for (var i = 0; i < 40; i++) new RegExp('flood-dup-' + i);",
                "/other/.exec('other');"
            ].join("\n"));
            CollectGarbage();
        }
    },];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
