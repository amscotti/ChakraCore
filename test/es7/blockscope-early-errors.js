//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Block : { StatementList } static-semantics early errors (ES sec-block-static-semantics-early-errors):
// LexicallyDeclaredNames must be duplicate-free, and no lexical name may also occur in the
// block's recursively collected VarDeclaredNames. Function declarations in blocks are lexical for
// this check, and var declarations stop at a function boundary. Annex B still allows valid block
// function declarations, including duplicate ordinary functions in sloppy code.

function assertParseError(source) {
    assert.throws(function () { eval(source); }, SyntaxError, source);
}

function assertParseOK(source) {
    assert.doesNotThrow(function () { eval(source); }, source);
}

var tests = [
    {
        name: "valid block function declarations and legal var/function cases",
        body() {
            assertParseOK("{ function f() {} }");
            assertParseOK("{ function f() {} function f() {} }");
            assertParseOK("{ function* f() {} }");
            assertParseOK("{ async function f() {} }");
            assertParseOK("{ async function* f() {} }");
            assertParseOK("{ class f {} }");
            assertParseOK("{ function f() {} var g; }");

            // Separate sibling blocks do not share a VarDeclaredNames list.
            assertParseOK("{ function f() {} } { var f; }");
            assertParseOK("{ let f; } { var f; }");
            assertParseOK("{ var f; } { let f; }");

            // A nested function is a function boundary for VarDeclaredNames.
            assertParseOK("{ function f() {} function g() { var f; } }");
            assertParseOK("{ let f; function g() { var f; } }");
            assertParseOK("{ let f; function g() { { var f; } } }");
            assertParseOK("{ let f; class C { m() { var f; } } }");

            // Inner lexical declarations may shadow an outer block function.
            assertParseOK("{ function f() {} { function f() {} } }");
            assertParseOK("{ function f() {} { let f; } }");
            assertParseOK("{ let f; { let f; } }");

            // A var in the function body is not in an inner block's list.
            assertParseOK("function x() { { let f; } var f; }");
            assertParseOK("function x() { { let f; } { var f; } }");
        }
    },
    {
        name: "lexical declarations conflict with var declarations in the same block",
        body() {
            assertParseError("{ let f; var f; }");
            assertParseError("{ var f; let f; }");
            assertParseError("{ const f = 0; var f; }");
            assertParseError("{ var f; const f = 0; }");
            assertParseError("{ class f {} var f; }");
            assertParseError("{ var f; class f {} }");
            assertParseError("{ function f() {} var f; }");
            assertParseError("{ var f; function f() {} }");
            assertParseError("{ function* f() {} var f; }");
            assertParseError("{ var f; function* f() {} }");
            assertParseError("{ async function f() {} var f; }");
            assertParseError("{ var f; async function f() {} }");
            assertParseError("{ async function* f() {} var f; }");
            assertParseError("{ var f; async function* f() {} }");
        }
    },
    {
        name: "nested blocks contribute var names to enclosing blocks",
        body() {
            assertParseError("{ { var f; } class f {} }");
            assertParseError("{ class f {} { var f; } }");
            assertParseError("{ function f() {} { var f; } }");
            assertParseError("{ { var f; } function f() {} }");
            assertParseError("{ { { var f; } } let f; }");
            assertParseError("{ let f; if (1) { var f; } }");
            assertParseError("{ let f; while (0) { var f; } }");
            assertParseError("{ let f; try { var f; } catch (e) {} }");
            assertParseError("{ let f; switch (0) { case 0: var f; } }");
            assertParseError("{ let f; with ({}) { var f; } }");
            assertParseError("{ let f; for (;;) { var f; } }");
            assertParseError("{ let f; for (let f of []) { var f; } }");
            assertParseError("{ let f; for (var f of []) {} }");
        }
    },
    {
        name: "function-scope blocks apply the same block rule",
        body() {
            assertParseError("function x() { { class f {} var f; } }");
            assertParseError("function x() { { var f; class f {} } }");
            assertParseError("function x() { { { var f; } class f {} } }");
            assertParseError("function x() { { class f {} { var f; } } }");
            assertParseError("function x() { { let f; var f; } }");
            assertParseError("function x() { { var f; const f = 0; } }");
            assertParseError("function x() { { async function f() {} var f; } }");
            assertParseError("function x() { { var f; async function f() {} } }");
            assertParseError("function x() { { function* f() {} var f; } }");
            assertParseError("function x() { { var f; function* f() {} } }");
        }
    },
    {
        name: "block function declarations are lexical and duplicate-free",
        body() {
            assertParseError("{ function f() {} function* f() {} }");
            assertParseError("{ function f() {} async function f() {} }");
            assertParseError("{ function* f() {} function* f() {} }");
            assertParseError("{ async function f() {} async function f() {} }");
            assertParseError("{ function f() {} class f {} }");
            assertParseError("{ class f {} function f() {} }");
            assertParseError("\"use strict\"; { function f() {} function f() {} }");
            assertParseError("\"use strict\"; { function f() {} async function f() {} }");
        }
    },
    {
        name: "script-level parse applies the same block early errors",
        body() {
            assert.throws(function () {
                WScript.LoadScript("{ class f {} var f; }");
            }, SyntaxError);
            assert.throws(function () {
                WScript.LoadScript("{ { var f; } let f; }");
            }, SyntaxError);
            assert.throws(function () {
                WScript.LoadScript("\"use strict\"; { function f() {} function f() {} }");
            }, SyntaxError);
            assert.doesNotThrow(function () {
                WScript.LoadScript("{ function f() {} }");
            });
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
