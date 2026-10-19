//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Annex B B.3.2.1 / B.3.3.1 / B.3.3.2 / B.3.3.3: a sloppy block-level
// ordinary function declaration gets a function-scoped synthetic var only when
// replacing the declaration with "var f;" would not introduce an early error in
// the enclosing function, script or eval code. Lexical names in any enclosing
// block, for/switch/catch construct, function body or global/eval scope skip
// the extension. Generators and async functions never receive it.
//
// The negative cases assert that no binding exists after the block; the
// positive controls keep ordinary Annex B hoisting observable.

var tests = [
    {
        name: "function code: enclosing block, for, switch and catch lexical names skip the extension",
        body() {
            (function () {
                assert.throws(function () { annexBFnBlock; }, ReferenceError);
                { let annexBFnBlock = 123; { function annexBFnBlock() {} } }
                assert.throws(function () { annexBFnBlock; }, ReferenceError);
                assert.strictEqual(typeof annexBFnBlock, "undefined");
            }());

            (function () {
                let annexBFnPlain = 123;
                { function annexBFnPlain() {} }
                assert.strictEqual(annexBFnPlain, 123);
            }());

            (function () {
                for (let annexBFnFor; ; ) {
                    { function annexBFnFor() {} }
                    break;
                }
                assert.throws(function () { annexBFnFor; }, ReferenceError);
            }());

            (function () {
                for (let annexBFnForIn in { x: 0 }) {
                    { function annexBFnForIn() {} }
                }
                assert.throws(function () { annexBFnForIn; }, ReferenceError);
            }());

            (function () {
                for (let annexBFnForOf of [0]) {
                    { function annexBFnForOf() {} }
                }
                assert.throws(function () { annexBFnForOf; }, ReferenceError);
            }());

            (function () {
                switch (0) {
                    default:
                        let annexBFnSwitch;
                        { function annexBFnSwitch() {} }
                }
                assert.throws(function () { annexBFnSwitch; }, ReferenceError);
            }());

            (function () {
                try {
                    throw {};
                } catch ({ annexBFnCatch }) {
                    { function annexBFnCatch() {} }
                }
                assert.throws(function () { annexBFnCatch; }, ReferenceError);
            }());

            // B.3.4 keeps a simple catch parameter compatible with var-only
            // bindings, so this is a positive control, not a skip.
            (function () {
                try {
                    throw 1;
                } catch (annexBCatchSimple) {
                    { function annexBCatchSimple() {} }
                }
                assert.strictEqual(typeof annexBCatchSimple, "function");
            }());
        }
    },
    {
        name: "lexical name declared after the block function also skips the extension",
        body() {
            (function () {
                { if (true) function annexBLaterIf() {} }
                let annexBLaterIf = 7;
                assert.strictEqual(annexBLaterIf, 7);
            }());

            (function () {
                {
                    if (true) function annexBLaterBlock() {}
                    let annexBLaterBlock = 9;
                    assert.strictEqual(annexBLaterBlock, 9);
                }
                assert.throws(function () { annexBLaterBlock; }, ReferenceError);
            }());

            (function () {
                {
                    { function annexBLaterClassBlock() {} }
                    class annexBLaterClassBlock {}
                    assert.strictEqual(typeof annexBLaterClassBlock, "function");
                }
                assert.throws(function () { annexBLaterClassBlock; }, ReferenceError);
            }());

            (function () {
                { { function annexBLaterNested() {} } }
                const annexBLaterNested = 8;
                assert.strictEqual(annexBLaterNested, 8);
            }());

            (function () {
                { { function annexBLaterClass() {} } }
                class annexBLaterClass {}
                assert.strictEqual(typeof annexBLaterClass, "function");
            }());
        }
    },
    {
        name: "nested and sibling lexical names",
        body() {
            (function () {
                {
                    let annexBNestedOuter = 1;
                    { { function annexBNestedOuter() {} } }
                    assert.strictEqual(annexBNestedOuter, 1);
                }
            }());

            (function () {
                { function annexBSibling() { return 1; } }
                { let annexBSibling = 2; }
                assert.strictEqual(annexBSibling(), 1);
            }());

            (function () {
                { let annexBOuterShadow = 1; { function annexBOuterShadow() {} } }
                assert.throws(function () { annexBOuterShadow; }, ReferenceError);
            }());

            (function () {
                { function* annexBGenLexical() {} { function annexBGenLexical() {} } }
                assert.throws(function () { annexBGenLexical; }, ReferenceError);
            }());
        }
    },
    {
        name: "nested sloppy block functions, both call orders",
        body() {
            // test262 block-decl-nested-blocks-with-fun-decl.js: the inner
            // declaration is not Annex-B applicable because the enclosing
            // Block's ordinary function is a LexicalDeclaredName.
            function outerDeclaredLast() {
                { { function f() { return 2; } } function f() { return 1; } }
                return f();
            }
            function outerDeclaredFirst() {
                { function f() { return 1; } { function f() { return 2; } } }
                return f();
            }
            assert.strictEqual(outerDeclaredLast(), 1);
            assert.strictEqual(outerDeclaredFirst(), 1);

            // Without an enclosing same-named declaration the ordinary
            // extension still applies, including through nested blocks.
            (function () {
                { { function annexBNestedOnly() { return 3; } } }
                assert.strictEqual(annexBNestedOnly(), 3);
            }());

            // Sibling blocks remain isolated; the later execution wins.
            (function () {
                { function annexBNestedSibling() { return 1; } }
                { function annexBNestedSibling() { return 2; } }
                assert.strictEqual(annexBNestedSibling(), 2);
            }());
        }
    },
    {
        name: "same-named var, function, parameter and arguments bindings are not updated by skipped declarations",
        body() {
            // The extension is skipped because of the enclosing let, and the
            // var binding must keep its own value (the emitted gate prevents
            // GetFuncScopeVarSym from finding it).
            function annexBVarIdentity() {
                var f = "V";
                { let f = "L"; { function f() { return "B"; } } }
                return typeof f + ":" + f;
            }
            assert.strictEqual(annexBVarIdentity(), "string:V");

            function annexBFunctionIdentity() {
                function f() { return "T"; }
                { let f = "L"; { function f() { return "B"; } } }
                return f();
            }
            assert.strictEqual(annexBFunctionIdentity(), "T");

            function annexBParamIdentity(f) {
                { let f = "L"; { function f() {} } }
                return typeof f;
            }
            assert.strictEqual(annexBParamIdentity(7), "number");

            function annexBGenVar() {
                var f = "V";
                { function* f() {} }
                return typeof f + ":" + f;
            }
            function annexBAsyncVar() {
                var f = "V";
                { async function f() {} }
                return typeof f + ":" + f;
            }
            function annexBAsyncGenVar() {
                var f = "V";
                { async function* f() {} }
                return typeof f + ":" + f;
            }
            assert.strictEqual(annexBGenVar(), "string:V");
            assert.strictEqual(annexBAsyncVar(), "string:V");
            assert.strictEqual(annexBAsyncGenVar(), "string:V");
        }
    },
    {
        name: "parameter, rest/default parameter and arguments exclusions",
        body() {
            function annexBParamPlain(f) {
                { function f() {} }
                return typeof f + ":" + f;
            }
            function annexBParamDefault(f = 7) {
                { function f() {} }
                return typeof f + ":" + f;
            }
            function annexBRest(...args) {
                { function args() {} }
                return Array.isArray(args) + ":" + args.length;
            }
            function annexBArguments() {
                { function arguments() {} }
                return arguments.toString();
            }
            function annexBArgumentsRest(..._) {
                { function arguments() {} }
                return arguments.toString();
            }
            assert.strictEqual(annexBParamPlain(7), "number:7");
            assert.strictEqual(annexBParamDefault(), "number:7");
            assert.strictEqual(annexBRest(1, 2), "true:2");
            assert.strictEqual(annexBArguments(), "[object Arguments]");
            assert.strictEqual(annexBArgumentsRest(), "[object Arguments]");

            // A genuinely applicable block function still updates an existing
            // function-scope var/function binding.
            (function () {
                var annexBStillVar = "before";
                { function annexBStillVar() { return "block"; } }
                assert.strictEqual(annexBStillVar(), "block");
            }());
            (function () {
                { function annexBStillFn() { return "block"; } }
                function annexBStillFn() { return "top"; }
                assert.strictEqual(annexBStillFn(), "block");
            }());
        }
    },
    {
        name: "valid Annex B hoisting controls",
        body() {
            (function () {
                { function annexBHoist() { return 1; } }
                assert.strictEqual(annexBHoist(), 1);

                { function annexBHoist2() { return 1; } }
                { function annexBHoist2() { return 2; } }
                assert.strictEqual(annexBHoist2(), 2);

                var annexBHoist3;
                { function annexBHoist3() { return 3; } }
                assert.strictEqual(annexBHoist3(), 3);

                { function annexBHoist4() { return "block"; } }
                function annexBHoist4() { return "top"; }
                assert.strictEqual(annexBHoist4(), "block");

                { function annexBHoist5() { return 5; } function annexBHoist5() { return 6; } }
                assert.strictEqual(annexBHoist5(), 6);
            }());

            (function () {
                "use strict";
                { function annexBStrict() {} }
                assert.strictEqual(typeof annexBStrict, "undefined");
            }());
        }
    },
    {
        name: "generator, async and async-generator block functions never get the extension",
        body() {
            (function () {
                switch (0) { default: function* annexBSwitchGen() {} }
                assert.strictEqual(typeof annexBSwitchGen, "undefined");
                assert.throws(function () { annexBSwitchGen; }, ReferenceError);

                switch (0) { default: async function annexBSwitchAsync() {} }
                assert.strictEqual(typeof annexBSwitchAsync, "undefined");
                assert.throws(function () { annexBSwitchAsync; }, ReferenceError);

                switch (0) { default: async function* annexBSwitchAsyncGen() {} }
                assert.strictEqual(typeof annexBSwitchAsyncGen, "undefined");
                assert.throws(function () { annexBSwitchAsyncGen; }, ReferenceError);
            }());
        }
    },
    {
        name: "direct eval: function and global eval code",
        body() {
            (function () {
                eval("let annexBEvalFnLet = 123; { function annexBEvalFnLet() {} } assert.strictEqual(annexBEvalFnLet, 123);");
                assert.strictEqual(typeof annexBEvalFnLet, "undefined");
            }());

            (function () {
                eval("{ let annexBEvalUnbound = 123; { function annexBEvalUnbound() {} } }");
                assert.throws(function () { annexBEvalUnbound; }, ReferenceError);
            }());

            (function () {
                eval("var annexBEvalVar = 1; { function annexBEvalVar() { return 2; } }");
                assert.strictEqual(annexBEvalVar(), 2);
            }());

            (function () {
                eval("for (let annexBEvalFor; ; ) { { function annexBEvalFor() {} } break; } assert.throws(function () { annexBEvalFor; }, ReferenceError);");
            }());

            (function () {
                eval("switch (0) { default: let annexBEvalSwitch; { function annexBEvalSwitch() {} } } assert.throws(function () { annexBEvalSwitch; }, ReferenceError);");
            }());

            (function () {
                eval("try { throw {}; } catch ({ annexBEvalCatch }) { { function annexBEvalCatch() {} } } assert.throws(function () { annexBEvalCatch; }, ReferenceError);");
            }());

            (function () {
                eval("switch (0) { default: function* annexBEvalGen() {} } assert.strictEqual(typeof annexBEvalGen, 'undefined');");
            }());
        }
    },
    {
        name: "indirect eval and global code",
        body() {
            (0, eval)("let annexBIndirect = 1; { function annexBIndirect() {} }");
            assert.strictEqual(typeof annexBIndirect, "undefined");

            (0, eval)("{ let annexBIndirectUnbound = 1; { function annexBIndirectUnbound() {} } }");
            assert.strictEqual(typeof annexBIndirectUnbound, "undefined");

            (0, eval)("for (let annexBIndirectFor; ; ) { { function annexBIndirectFor() {} } break; }");
            assert.strictEqual(typeof annexBIndirectFor, "undefined");

            WScript.LoadScript(
                "let annexBGlobalLet = 123;" +
                "{ function annexBGlobalLet() {} }" +
                "if (annexBGlobalLet !== 123) { throw new Error('global let binding was changed'); }" +
                "{ let annexBGlobalBlock = 1; { function annexBGlobalBlock() {} } }" +
                "if (typeof annexBGlobalBlock !== 'undefined') { throw new Error('global block extension was created'); }" +
                "{ function annexBGlobalHoist() { return 5; } }" +
                "if (annexBGlobalHoist() !== 5) { throw new Error('global hoisting was not observed'); }" +
                "{ if (true) function annexBGlobalLater() {} }" +
                "let annexBGlobalLater = 7;" +
                "if (annexBGlobalLater !== 7) { throw new Error('later global lexical name did not skip the extension'); }" +
                "switch (0) { default: function* annexBGlobalGen() {} }" +
                "if (typeof annexBGlobalGen !== 'undefined') { throw new Error('global generator extension was created'); }"
            );
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
