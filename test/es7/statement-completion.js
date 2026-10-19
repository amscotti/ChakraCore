//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Statement completion values (ES2015+ UpdateEmpty). Direct eval of a Script returns
// the script's completion value; an empty completion contributes undefined, an
// explicit undefined is a value, and statements that inherit a value keep it.

function evalCompletion(source) {
    return eval(source);
}

var tests = [
    {
        name: "if statement completion (UpdateEmpty with undefined)",
        body() {
            assert.areEqual(undefined, evalCompletion("1; if (false) { }"), "false branch, no else");
            assert.areEqual(undefined, evalCompletion("1; if (false) { 2; }"), "false branch with body");
            assert.areEqual(undefined, evalCompletion("1; if (true) { }"), "true branch, empty body");
            assert.areEqual(2, evalCompletion("1; if (true) { 2; }"), "true branch value");
            assert.areEqual(3, evalCompletion("1; if (false) { 2; } else { 3; }"), "else value");
            assert.areEqual(2, evalCompletion("1; if (true) { 2; } else { 3; }"), "then value");
            assert.areEqual(undefined, evalCompletion("1; if (true) { if (false) { } }"), "nested empty if");
        }
    },
    {
        name: "switch completion (V starts at undefined)",
        body() {
            assert.areEqual(undefined, evalCompletion("1; switch ('a') { }"), "empty switch");
            assert.areEqual(undefined, evalCompletion("1; switch ('z') { case 'a': 2; }"), "no match");
            assert.areEqual(undefined, evalCompletion("1; switch ('a') { case 'a': break; }"), "empty case with break");
            assert.areEqual(2, evalCompletion("1; switch ('a') { case 'a': 2; break; }"), "case value and break");
            assert.areEqual(3, evalCompletion("1; switch ('a') { case 'a': 2; case 'b': 3; }"), "fallthrough value");
            assert.areEqual(undefined, evalCompletion("1; switch ('a') { default: break; }"), "default break");
            assert.areEqual(4, evalCompletion("1; switch ('b') { default: case 'b': 4; }"), "default-first fallthrough");
        }
    },
    {
        name: "for statement completion (V starts at undefined)",
        body() {
            assert.areEqual(undefined, evalCompletion("1; for (var i = 0; i < 1; ++i) { }"), "empty body");
            assert.areEqual(2, evalCompletion("1; for (var i = 0; i < 1; ++i) { 2; }"), "body value");
            assert.areEqual(undefined, evalCompletion("1; for ( ; false; ) { 2; }"), "zero iterations");
            assert.areEqual(2, evalCompletion("1; for (var i = 0; i < 1; ++i) { 2; break; }"), "value before break");
            assert.areEqual(undefined, evalCompletion("1; for (var i = 0; i < 1; ++i) { break; }"), "empty break");
            assert.areEqual(undefined, evalCompletion("1; for (var i = 0; i < 2; ++i) { if (i === 1) break; 7; }"),
                "break through if clears the previous body value");
            assert.areEqual(8, evalCompletion("1; for (var i = 0; i < 2; ++i) { if (i === 1) { 8; break; } 7; }"),
                "value produced on the break iteration is kept");

            // Historical head-init completion scenario.
            assert.areEqual(undefined, evalCompletion("for (count = 0;;) { if (count === 5) break; else count++; }"),
                "for head init expression");
            assert.areEqual(undefined, evalCompletion("for (var count = 0;;) { if (count === 5) break; else count++; }"),
                "for head init var");
        }
    },
    {
        name: "for-in and for-of completion",
        body() {
            assert.areEqual(undefined, evalCompletion("1; for (var x in { a: 1 }) { }"), "for-in empty body");
            assert.areEqual(2, evalCompletion("1; for (var x in { a: 1 }) { 2; }"), "for-in body value");
            assert.areEqual(undefined, evalCompletion("1; for (var x in {}) { 2; }"), "for-in zero iterations");
            assert.areEqual(2, evalCompletion("1; for (var x in { a: 1 }) { 2; break; }"), "for-in value before break");
            assert.areEqual(undefined, evalCompletion("1; for (var x in { a: 1 }) { break; }"), "for-in empty break");

            assert.areEqual(undefined, evalCompletion("1; for (var x of [3]) { }"), "for-of empty body");
            assert.areEqual(4, evalCompletion("1; for (var x of [3]) { 4; }"), "for-of body value");
            assert.areEqual(undefined, evalCompletion("1; for (var x of []) { 4; }"), "for-of zero iterations");
            assert.areEqual(4, evalCompletion("1; for (var x of [3]) { 4; break; }"), "for-of value before break");

            assert.areEqual(6, evalCompletion("1; outer: do { for (var y in { a: 1 }) { 6; continue outer; } } while (false)"),
                "labeled continue keeps the value produced before it");
            assert.areEqual(undefined, evalCompletion("1; outer: do { for (var y in { a: 1 }) { continue outer; } } while (false)"),
                "labeled continue with no value");
        }
    },
    {
        name: "while and do-while completion",
        body() {
            assert.areEqual(undefined, evalCompletion("1; while (false) { 2; }"), "while zero iterations");
            assert.areEqual(undefined, evalCompletion("1; while (true) { break; }"), "while empty break");
            assert.areEqual(2, evalCompletion("1; while (true) { 2; break; }"), "while value before break");
            assert.areEqual(2, evalCompletion("1; var n = 0; while (n++ < 1) { 2; }"), "while body value");

            assert.areEqual(undefined, evalCompletion("1; do { } while (false)"), "do-while empty body");
            assert.areEqual(2, evalCompletion("1; do { 2; } while (false)"), "do-while body value");
            assert.areEqual(undefined, evalCompletion("1; do { break; } while (false)"), "do-while empty break");
            assert.areEqual(2, evalCompletion("1; do { 2; continue; } while (false)"), "do-while value before continue");
        }
    },
    {
        name: "try/catch/finally completion",
        body() {
            assert.areEqual(undefined, evalCompletion("1; try { } catch (e) { }"), "empty try");
            assert.areEqual(2, evalCompletion("1; try { 2; } catch (e) { }"), "try value");
            assert.areEqual(undefined, evalCompletion("1; try { throw 0; } catch (e) { }"), "empty catch");
            assert.areEqual(3, evalCompletion("1; try { throw 0; } catch (e) { 3; }"), "catch value");
            assert.areEqual(undefined, evalCompletion("1; try { 2; throw 0; } catch (e) { }"),
                "try value before throw does not leak through an empty catch");

            assert.areEqual(undefined, evalCompletion("1; try { } finally { }"), "empty try-finally");
            assert.areEqual(2, evalCompletion("1; try { 2; } finally { }"), "try value with empty finally");
            assert.areEqual(undefined, evalCompletion("1; try { } finally { 3; }"),
                "normal finally value is discarded");
            assert.areEqual(2, evalCompletion("1; try { 2; } finally { 3; }"),
                "normal finally does not replace the try value");
            assert.areEqual(2, evalCompletion("1; try { throw 0; } catch (e) { 2; } finally { 3; }"),
                "catch value with normal finally");
            assert.areEqual(42, evalCompletion("99; do { -99; try { 39 } finally { 42; break; } } while (false)"),
                "abrupt finally value is kept");
            assert.areEqual(undefined, evalCompletion("99; do { -99; try { 39 } finally { break; } } while (false)"),
                "abrupt empty finally clears the try value");
        }
    },
    {
        name: "with statement completion",
        body() {
            assert.areEqual(undefined, evalCompletion("1; with ({}) { }"), "empty with body");
            assert.areEqual(2, evalCompletion("1; with ({}) { 2; }"), "with body value");
            assert.areEqual(3, evalCompletion("1; do { 2; with ({}) { 3; break; } 4; } while (false)"),
                "with value before break");
            assert.areEqual(undefined, evalCompletion("1; do { 2; with ({}) { break; } 4; } while (false)"),
                "with empty break clears the previous value");
        }
    },
    {
        name: "empty statements, blocks, declarations and explicit undefined",
        body() {
            assert.areEqual(2, evalCompletion("2;;"), "empty statement preserves the previous value");
            assert.areEqual(1, evalCompletion("1; { }"), "empty block preserves the previous value");
            assert.areEqual(2, evalCompletion("1; { 2; }"), "block value");
            assert.areEqual(undefined, evalCompletion(";"), "only an empty statement");
            assert.areEqual(undefined, evalCompletion("function f() { }"), "only a function declaration");
            assert.areEqual(1, evalCompletion("1; function f() { }"), "function declaration preserves the previous value");
            assert.areEqual(1, evalCompletion("1; var x;"), "var statement preserves the previous value");
            assert.areEqual(undefined, evalCompletion("undefined"), "explicit undefined");
            assert.areEqual(undefined, evalCompletion("1; undefined"), "explicit undefined overwrites the previous value");
            assert.areEqual(undefined, evalCompletion("1; void 0"), "void 0 overwrites the previous value");
            assert.areEqual(null, evalCompletion("1; null"), "null is a value");
            assert.areEqual(0, evalCompletion("1; 0"), "zero is a value");
        }
    },
    {
        name: "labels, break and continue",
        body() {
            assert.areEqual(5, evalCompletion("test262id: { 5; break test262id; 9; }"), "labelled break keeps the value");
            assert.areEqual(2, evalCompletion("test262id: 2;"), "labelled statement value");
            assert.areEqual(undefined, evalCompletion("test262id: if (false) { }"), "labelled empty if");
            assert.areEqual(8, evalCompletion("test262id: { 7; if (true) { 8; break test262id; } }"),
                "labelled break through if with its own value");
            assert.areEqual(undefined, evalCompletion("test262id: { 7; if (true) { break test262id; } }"),
                "labelled break through empty if clears the outer value");
        }
    },
    {
        name: "ordinary function returns are unchanged",
        body() {
            assert.areEqual(undefined, (function () { 1; })(), "implicit return is undefined");
            assert.areEqual(1, (function () { return 1; })(), "explicit return");
            assert.areEqual(undefined, (function () { if (false) { } })(), "if does not affect function return");
            assert.areEqual(2, (function () { try { return 2; } finally { 3; } })(), "finally does not replace return");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
