//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Focused regressions for the E7 early-error lane (doc/e7-early-error-executions.md):
// parse-negative test262 bodies that ChakraCore executed instead of rejecting
// ($DONOTEVALUATE clusters). Each family below is rejected at parse time through
// eval so the SyntaxError is observable; the positive controls pin the
// preserved web-compat / Annex B behaviors.
//
// 1. Strict call-result assignment targets are ~invalid~ (ES2027 13.15.1):
//    `f() = 1`, `(f()) += 1`, `f()++`, `++f()`, `for (f() in {})`,
//    `for (f() of [])` are parse SyntaxErrors in strict code only; sloppy
//    code keeps the Annex B.3.9 runtime ReferenceError.
// 2. `new C(...) = 1` / `(new C()) = 1` are parse SyntaxErrors in every mode
//    (only the two CallExpression productions are web-compat).
// 3. ImportCall is an ~invalid~ target in every mode: `import('') = 1`,
//    `++import('')`, `import('')++` are parse SyntaxErrors.
// 4. `for (var x = 1 of [])` and the for-await-of form are parse
//    SyntaxErrors; Annex B B.3.5 sloppy `for (var x = 1 in {})` stays legal.
// 5. `\8`/`\9` are rejected in template literals (cooked undefined =>
//    early error in untagged templates); plain templates and `\0` still work.
// 6. A labelled function declaration is not a valid if/else clause
//    (14.7.1 IsLabelledFunction early error); the sole unlabelled
//    Annex B.3.3 form and labelled functions in blocks stay legal.
// 7. `new.t\u0061rget` / `import.m\u0065ta` reject escaped meta properties.
// 8. An async function *expression* named `await` is rejected even at sloppy
//    top level (BindingIdentifier[+Await]); the sloppy declaration form
//    inherits ambient [?Await] and stays legal.
// 9. `{...rest,}` is an invalid object pattern (no `{ BindingRestElement , }`
//    production) for assignment, for-in and for-of destructuring; object
//    *literals* keep the trailing comma after spread.
// 10. `for (yield * '' in {}; ; ) ;` inside a generator is a SyntaxError
//     (`yield *` threads the ambient [~In], leaving `in` to expose the
//     invalid for-in target).
// 11. `{async async}`, `{async foo: 1}` and `{async [x]: 1}` are rejected
//     (`async`-prefixed members must be methods); `{async}`, `{async,}`,
//     `{async: 1}`, `{async\nfoo: 1}` and async methods keep working.

var tests = [
    {
        name: "Strict call-result targets are parse errors, sloppy keeps the runtime ReferenceError",
        body: function () {
            assert.throws(function () { eval('"use strict"; function f(){} f() = 1;'); }, SyntaxError, "strict f() = 1");
            assert.throws(function () { eval('"use strict"; function f(){} (f()) = 1;'); }, SyntaxError, "strict (f()) = 1");
            assert.throws(function () { eval('"use strict"; function f(){} f() += 1;'); }, SyntaxError, "strict f() += 1");
            assert.throws(function () { eval('"use strict"; function f(){} (f()) += 1;'); }, SyntaxError, "strict (f()) += 1");
            assert.throws(function () { eval('"use strict"; function f(){} f()++;'); }, SyntaxError, "strict f()++");
            assert.throws(function () { eval('"use strict"; function f(){} ++f();'); }, SyntaxError, "strict ++f()");
            assert.throws(function () { eval('"use strict"; function f(){} for (f() in {}) ;'); }, SyntaxError, "strict for (f() in {})");
            assert.throws(function () { eval('"use strict"; function f(){} for (f() of []) ;'); }, SyntaxError, "strict for (f() of [])");
            // Sloppy: web-compat target, error surfaces at runtime only.
            assert.doesNotThrow(function () { eval('function f(){return 1;} try { f() = 1; } catch (e) { assert.isTrue(e instanceof ReferenceError, "sloppy f() = 1 runtime error"); }'); }, "sloppy f() = 1 parses");
            assert.doesNotThrow(function () { eval('function f(){return 1;} try { f()++; } catch (e) { assert.isTrue(e instanceof ReferenceError, "sloppy f()++ runtime error"); }'); }, "sloppy f()++ parses");
        }
    },
    {
        name: "new-expression targets are parse errors in every mode",
        body: function () {
            assert.throws(function () { eval('function f(){} new f() = 1;'); }, SyntaxError, "new f() = 1");
            assert.throws(function () { eval('function f(){} (new f()) = 1;'); }, SyntaxError, "(new f()) = 1");
            assert.throws(function () { eval('function f(){} new f() += 1;'); }, SyntaxError, "new f() += 1");
            assert.throws(function () { eval('function f(){} new f()++;'); }, SyntaxError, "new f()++");
            assert.throws(function () { eval('"use strict"; function f(){} new f() = 1;'); }, SyntaxError, "strict new f() = 1");
            assert.doesNotThrow(function () { eval('function f(){} var x = new f; assert.isTrue(x instanceof f, "new f without parens still constructible");'); }, "new f without args");
        }
    },
    {
        name: "ImportCall is an invalid assignment target in every mode",
        body: function () {
            assert.throws(function () { eval('import("") = 1;'); }, SyntaxError, "import('') = 1");
            assert.throws(function () { eval('++import("");'); }, SyntaxError, "++import('')");
            assert.throws(function () { eval('import("")++;'); }, SyntaxError, "import('')++");
            assert.throws(function () { eval('for (import("") of []) ;'); }, SyntaxError, "for (import('') of [])");
            // Call/member chains over an import result stay usable.
            assert.doesNotThrow(function () { eval('var t = typeof (import("").then);'); }, "import result member access parses");
        }
    },
    {
        name: "for-of heads reject binding initializers; sloppy for-in var keeps Annex B B.3.5",
        body: function () {
            assert.throws(function () { eval('for (var x = 1 of []) ;'); }, SyntaxError, "for (var x = 1 of [])");
            assert.throws(function () { eval('for (let x = 1 of []) ;'); }, SyntaxError, "for (let x = 1 of [])");
            assert.throws(function () { eval('async function f() { for await (var x = 1 of []) ; }'); }, SyntaxError, "for await (var x = 1 of [])");
            assert.doesNotThrow(function () { eval('for (var x = 1 in {a:1}) ;'); }, "sloppy for-in var initializer stays legal");
        }
    },
    {
        name: "Template literals reject \\8 and \\9 escapes",
        body: function () {
            assert.throws(function () { eval('`\\8`;'); }, SyntaxError, "template \\8");
            assert.throws(function () { eval('`\\9`;'); }, SyntaxError, "template \\9");
            assert.doesNotThrow(function () { eval('assert.areEqual(0, `\\0`.charCodeAt(0), "template \\0");'); }, "template \\0 stays legal");
            assert.doesNotThrow(function () { eval('assert.areEqual("a2b", `a${1+1}b`, "interpolation");'); }, "plain template stays legal");
        }
    },
    {
        name: "Labelled function declarations are not valid if/else clauses",
        body: function () {
            assert.throws(function () { eval('if (false) label1: label2: function test262() {} else ;'); }, SyntaxError, "if clause");
            assert.throws(function () { eval('if (true) ; else label1: label2: function test262() {}'); }, SyntaxError, "else clause");
            assert.throws(function () { eval('if (false) label1: label2: function test262() {}'); }, SyntaxError, "lone if clause");
            assert.doesNotThrow(function () { eval('if (false) function sole() {}'); }, "sole Annex B.3.3 function stays legal");
            assert.doesNotThrow(function () { eval('if (false) { label1: function blocked() {} }'); }, "labelled function in a block stays legal");
        }
    },
    {
        name: "Meta properties reject escaped keywords",
        body: function () {
            assert.throws(function () { eval('function f() { new.t\\u0061rget; }'); }, SyntaxError, "escaped new.target");
            assert.throws(function () { eval('new.t\\u0061rget;'); }, SyntaxError, "escaped new.target at global scope");
            assert.doesNotThrow(function () { eval('function f() { return new.target === undefined; }'); }, "plain new.target in a function");
            // import.meta is module-only; escape it through module-ish eval is
            // not available here, covered by test262 (import.meta/syntax).
        }
    },
    {
        name: "Async function expressions reject the name await at sloppy top level",
        body: function () {
            assert.throws(function () { eval('(async function await() { });'); }, SyntaxError, "async expression named await");
            assert.throws(function () { eval('(async function* await() { });'); }, SyntaxError, "async generator expression named await");
            assert.doesNotThrow(function () { eval('async function await() { }'); }, "sloppy declaration named await stays legal (ambient [?Await])");
            assert.doesNotThrow(function () { eval('var f = function await() { return 4; }; assert.areEqual(4, f());'); }, "non-async expression named await stays legal");
            assert.doesNotThrow(function () { eval('var f = async function yieldName() { return 3; }; f().then(function (v) { assert.areEqual(3, v); });'); }, "async expression named yield-ish stays legal");
        }
    },
    {
        name: "Object patterns reject rest before trailing comma; literals keep it",
        body: function () {
            assert.throws(function () { eval('var rest; ({...rest,} = {});'); }, SyntaxError, "assignment pattern rest trailing comma");
            assert.throws(function () { eval('var rest; for ({...rest,} in {}) ;'); }, SyntaxError, "for-in pattern rest trailing comma");
            assert.throws(function () { eval('var rest; for ({...rest,} of []) ;'); }, SyntaxError, "for-of pattern rest trailing comma");
            assert.doesNotThrow(function () { eval('var a = [1]; var o = {...a,}; assert.areEqual(1, o[0], "literal spread trailing comma");'); }, "object literal spread trailing comma stays legal");
        }
    },
    {
        name: "yield * threads the ambient [In] in native-for heads",
        body: function () {
            assert.throws(function () { eval('function* g() { for (yield * "" in {}; ; ) ; }'); }, SyntaxError, "yield-star in native-for head");
            assert.doesNotThrow(function () { eval('function* g() { for (var r = yield; r === undefined; r = 1) { break; } }'); }, "plain yield in for-init stays legal");
        }
    },
    {
        name: "async-prefixed object members must be methods",
        body: function () {
            assert.throws(function () { eval('({async async});'); }, SyntaxError, "{async async}");
            assert.throws(function () { eval('({async foo: 1});'); }, SyntaxError, "{async foo: 1}");
            assert.throws(function () { eval('({async [x]: 1});'); }, SyntaxError, "{async [x]: 1}");
            assert.doesNotThrow(function () { eval('var o = {async: 1}; assert.areEqual(1, o.async, "{async: 1}");'); }, "async as plain name");
            assert.doesNotThrow(function () { eval('var async = 7; var o = {async, x: 1}; assert.areEqual(7, o.async, "{async} shorthand");'); }, "{async} shorthand stays legal");
            assert.doesNotThrow(function () { eval('var o = {async foo() { return 5; }}; o.foo();'); }, "async method stays legal");
            assert.doesNotThrow(function () { eval('var o = {async *gen(a) { yield a; }}; o.gen(3);'); }, "async generator method stays legal");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
