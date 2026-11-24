//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "P-1: sloppy class fields/static blocks and large Scope symbol tables",
        body: function () {
            const count = 3000;
            const parts = [];
            for (let i = 0; i < count; i++) {
                parts.push("class C" + i + " { a = " + i + "; static { } m() { return this.a; } }");
            }
            parts.push("return new C" + (count - 1) + "().m();");
            const fn = new Function(parts.join("\n"));
            assert.areEqual(count - 1, fn());

            // Verify Scope hash table (>16 symbols), deduplication, and merged param/body scope.
            const varParts = [];
            const params = [];
            for (let i = 0; i < 40; i++) {
                params.push("p" + i + " = " + i);
                varParts.push("var v" + i + " = " + (i + 100) + ";");
                varParts.push("let l" + i + " = " + (i + 200) + ";");
            }
            varParts.push("var v0 = 999;");
            varParts.push("return p0 + p39 + v0 + v39 + l0 + l39;");
            const bigFn = new Function(params.join(", "), varParts.join("\n"));
            assert.areEqual(0 + 39 + 999 + 139 + 200 + 239, bigFn());

            // Preserve Annex B.3.3 block-function semantics in sloppy mode and block it in strict mode.
            function sloppyAnnexB() {
                var r1 = typeof f;
                {
                    function f() { return 42; }
                }
                return r1 + ":" + (typeof f === "function" ? f() : "none");
            }
            assert.areEqual("undefined:42", sloppyAnnexB());

            function sloppyAnnexBBlockedByLet() {
                let g = 7;
                {
                    function g() { return 9; }
                }
                return g;
            }
            assert.areEqual(7, sloppyAnnexBBlockedByLet());

            function strictNoAnnexB() {
                "use strict";
                var r = typeof h;
                {
                    function h() { return 1; }
                }
                return r + ":" + typeof h;
            }
            assert.areEqual("undefined:undefined", strictNoAnnexB());
        }
    },
    {
        name: "P-2: deep nested class extends with PrependLocalScopes",
        body: function () {
            function runNested(depth, withCaptured) {
                let prefix = withCaptured ? "let cap = 17; with ({ w: 3 }) { " : "";
                let suffix = withCaptured ? " }" : "";
                let open = "class extends (".repeat(depth);
                let base = "class { getBase() { return " + (withCaptured ? "cap + w" : "5") + "; } }";
                let close = ") {}".repeat(depth);
                const code = prefix + "let Leaf = " + open + base + close + "; return new Leaf().getBase();" + suffix;
                return new Function(code)();
            }
            assert.areEqual(5, runNested(400, false));
            assert.areEqual(20, runNested(400, true));
        }
    },
    {
        name: "P-4: new.target allowed at top level of dynamic Function bodies",
        body: function () {
            const F = new Function("return new.target;");
            assert.strictEqual(undefined, F(), "ordinary call returns undefined");
            assert.strictEqual(F, new F(), "construct call returns constructor");

            const FDefault = new Function("a = new.target", "return a;");
            assert.strictEqual(undefined, FDefault(), "default formal returns undefined on call");
            assert.strictEqual(FDefault, new FDefault(), "default formal returns constructor on new");

            const FArrow = new Function("return () => new.target;");
            assert.strictEqual(undefined, FArrow()(), "arrow inside Function captures undefined on call");
            assert.strictEqual(FArrow, new FArrow()(), "arrow inside Function captures constructor on new");

            const FClassField = new Function("class C { x = new.target; static y = new.target; } return [new C().x, C.y];");
            assert.areEqual([undefined, undefined], FClassField());
            assert.areEqual([undefined, undefined], new FClassField());

            const FNested = new Function("function inner() { return new.target; } return [new.target, inner(), new inner() === inner];");
            assert.areEqual([undefined, undefined, true], FNested());
            assert.areEqual([FNested, undefined, true], new FNested());

            const FEval = new Function("return eval('new.target');");
            assert.strictEqual(undefined, FEval());
            assert.strictEqual(FEval, new FEval());

            const GeneratorFunction = (function* () {}).constructor;
            const GF = new GeneratorFunction("yield new.target;");
            assert.strictEqual(undefined, GF().next().value);
            assert.throws(function () { new GF(); }, TypeError);

            const AsyncFunction = (async function () {}).constructor;
            const AF = new AsyncFunction("return typeof new.target;");
            // ValidateSyntax allowed new.target cleanly; check async invocation syntax did not throw.
            assert.isTrue(typeof AF === "function");

            const AsyncGeneratorFunction = (async function* () {}).constructor;
            const AGF = new AsyncGeneratorFunction("yield new.target;");
            assert.isTrue(typeof AGF === "function");

            assert.throws(function () { (0, eval)("new.target"); }, SyntaxError);
            assert.throws(function () { new Function("super()"); }, SyntaxError);
            assert.throws(function () { new Function("super.prop"); }, SyntaxError);
        }
    },
    {
        name: "P-7: reject trailing commas in switch, with, for-in, for-incr, while, do-while",
        body: function () {
            const invalid = [
                "switch (1,) {}",
                "switch (1, 2,) {}",
                "with ({},) {}",
                "var a; for (a in {},) {}",
                "for (var a in {},) {}",
                "for (var i = 0; i < 1; i++,) {}",
                "while (0,) {}",
                "do {} while (0,);",
                "if (0,) {}",
                "for (var x of [1],) {}",
                "(class { static { switch (1,) {} } })",
                "(class { static { with ({},) {} } })",
                "(class { static { for (var a in {},) {} } })",
                "(a = (1,)) => a"
            ];
            for (let i = 0; i < invalid.length; i++) {
                assert.throws(function () { eval(invalid[i]); }, SyntaxError, "Expected SyntaxError for: " + invalid[i]);
            }

            // Valid comma expressions without trailing comma and arrow trailing comma remain accepted.
            const valid = [
                "let r = 0; switch ((1, 2)) { case 2: r = 1; break; } r;",
                "let r = 0; with ((1, { a: 7 })) { r = a; } r;",
                "let c = 0; for (var k in { a: 1 }, { b: 2 }) { c++; } c;",
                "let s = 0; for (var i = 0, j = 0; i < 2, j < 2; i++, j++) { s += i + j; } s;",
                "let s = 0; for (var x of (0, [10, 20])) { s += x; } s;",
                "((a,) => a)(42,);"
            ];
            for (let i = 0; i < valid.length; i++) {
                assert.doesNotThrow(function () { eval(valid[i]); }, "Should remain valid: " + valid[i]);
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
