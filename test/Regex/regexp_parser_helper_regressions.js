//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "P-3: VClassPass0 skips already-validated nested bracket spans",
        body: function () {
            const dyn = new RegExp("[[a]".repeat(4000) + "b" + "]".repeat(4000));
            assert.isFalse(dyn.test("a"));
            assert.isTrue(dyn.test("a".repeat(4000) + "b" + "]".repeat(4000)));

            const dynWithSlash = new RegExp("[[a]/".repeat(2000) + "]".repeat(2000));
            assert.isFalse(dynWithSlash.test("x"));

            const litBalanced = eval("/" + "[[a]".repeat(1500) + "b" + "]".repeat(1500) + "/");
            assert.isFalse(litBalanced.test("a"));

            assert.throws(function () {
                eval("/" + "[[a]".repeat(1500) + "[/u");
            }, SyntaxError);
            assert.throws(function () {
                eval("/" + "[[a]".repeat(1500) + "/v");
            }, SyntaxError);

            // Keep /v nested set operations and invalid bare ']' detection intact.
            assert.isTrue(/[[a][b]]/v.test("a"));
            assert.isTrue(/[[a][b]]/v.test("b"));
            assert.isTrue(/[[a-c]--[b]]/v.test("a"));
            assert.isFalse(/[[a-c]--[b]]/v.test("b"));
            assert.isTrue(/[\q{ab|c}]/v.test("ab"));
            assert.throws(function () { eval("/[]a]/v"); }, SyntaxError);
            assert.throws(function () { eval("/[[a]]]/v"); }, SyntaxError);
        }
    },
    {
        name: "P-5: Unicode property escape compilation caching across u/ui/v/vi",
        body: function () {
            const specs = [
                ["\\p{Letter}", "u", "A", "1"],
                ["\\P{Letter}", "u", "1", "A"],
                ["\\p{Script=Greek}", "u", "\u03B1", "a"],
                ["\\p{Script=Greek}", "ui", "\u03B1", "a"],
                ["\\p{Script_Extensions=Common}", "u", "1", "a"],
                ["\\p{ASCII}", "u", "z", "\u00E9"],
                ["\\p{Any}", "u", "\uD83D\uDE00", ""],
                ["\\p{Assigned}", "u", "A", "\u0378"],
                ["[\\p{Decimal_Number}]", "u", "7", "a"],
                ["[\\p{Letter}&&\\p{Script=Greek}]", "v", "\u03A9", "Z"],
                ["[\\p{Basic_Emoji}]", "v", "\u231A", "A"]
            ];
            for (let round = 0; round < 5; round++) {
                for (let i = 0; i < specs.length; i++) {
                    const [pat, flags, hit, miss] = specs[i];
                    const re = new RegExp("^" + pat + "$" + (i >= 0 ? "" : ""), flags);
                    assert.isTrue(re.test(hit), pat + "/" + flags + " hit");
                    if (miss.length > 0) {
                        assert.isFalse(re.test(miss), pat + "/" + flags + " miss");
                    }
                }
            }
            // Also verify repeated compilation of distinct patterns hitting cached properties.
            for (let i = 0; i < 200; i++) {
                const re = new RegExp("\\p{Letter}\\P{Script=Latin}" + i, (i & 1) ? "ui" : "u");
                assert.isTrue(re.test("a\u03B1" + i));
            }
        }
    },
    {
        name: "P-6: O(N) groups and indices.groups construction with duplicate names",
        body: function () {
            // <= threshold (fast path) and > threshold (hashed path) duplicate names.
            const small = /(?:(?<a>x)|(?<a>y))(?<b>z)|(?<a>w)/.exec("yz");
            assert.areEqual(["a", "b"], Object.keys(small.groups));
            assert.areEqual("y", small.groups.a);
            assert.areEqual("z", small.groups.b);
            assert.areEqual(null, Object.getPrototypeOf(small.groups));

            const N = 1500;
            const parts = [];
            for (let i = 0; i < N; i++) {
                parts.push("(?:(?<g" + i + ">a)|(?<g" + i + ">))");
            }
            parts.push("(?:(?<__proto__>)|(?<__proto__>p))$");
            const pat = parts.join("");
            const input = "a".repeat(N) + "p";
            const re = new RegExp(pat, "d");
            const m = re.exec(input);
            assert.isTrue(m !== null);
            assert.areEqual(null, Object.getPrototypeOf(m.groups));
            assert.areEqual("p", m.groups["__proto__"]);
            assert.areEqual("a", m.groups.g0);
            assert.areEqual("a", m.groups["g" + (N - 1)]);

            assert.areEqual(null, Object.getPrototypeOf(m.indices.groups));
            assert.isTrue(m.indices.groups["__proto__"] === m.indices[N * 2 + 2]);
            assert.isTrue(m.indices.groups.g0 === m.indices[1]);
            assert.isTrue(m.indices.groups["g" + (N - 1)] === m.indices[(N - 1) * 2 + 1]);
            assert.areEqual([N, N + 1], m.indices.groups["__proto__"]);

            const keys = Object.keys(m.groups);
            assert.areEqual(N + 1, keys.length);
            assert.areEqual("g0", keys[0]);
            assert.areEqual("g" + (N - 1), keys[N - 1]);
            assert.areEqual("__proto__", keys[N]);

            // Also verify String.prototype.replace with $<name> on >8 named captures.
            const replaced = input.replace(re, "$<__proto__>:$<g0>:$<g" + (N - 1) + ">");
            assert.areEqual("p:a:a", replaced);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
