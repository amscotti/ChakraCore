//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// ES2025 10.2.11 FunctionDeclarationInstantiation gives the body of a
// function with a non-simple parameter list its own declarative environment,
// which is the function's variable environment; the parameters live outside
// it. ES2025 19.2.1.3 EvalDeclarationInstantiation step 3.b checks a sloppy
// direct eval's var names only in the environments between the eval's
// lexical environment and its variable environment, so `eval('var <param>')`
// is not a redeclaration: the var is created in the body (variable)
// environment, shadowing the parameter for body reads, while the parameter
// binding itself is untouched. Simple parameters share the variable
// environment with the body, so there the existing binding is reused.

var tests = [
    {
        name: "default parameter: eval var without initializer shadows with undefined",
        body() {
            assert.areEqual(undefined, (function (a = 1) { eval("var a"); return a; })(), "body read sees the new var binding");
        }
    },
    {
        name: "default parameter: eval var with initializer",
        body() {
            assert.areEqual(2, (function (a = 1) { eval("var a = 2"); return a; })(), "body read sees the assigned var binding");
        }
    },
    {
        name: "default and rest parameters together",
        body() {
            var r = (function (a = 1, ...rest) { eval("var a = 2"); return [a, rest.length]; })();
            assert.areEqual(2, r[0], "a");
            assert.areEqual(0, r[1], "rest.length");
        }
    },
    {
        name: "rest parameter name itself",
        body() {
            var r = (function (a = 1, ...rest) { eval("var rest = 4"); return [rest, a]; })();
            assert.areEqual(4, r[0], "rest is shadowed by the eval var");
            assert.areEqual(1, r[1], "a is untouched");
        }
    },
    {
        name: "destructured parameter, with pattern default",
        body() {
            assert.areEqual(undefined, (function ({ a } = {}) { eval("var a"); return a; })(), "no initializer");
            assert.areEqual(9, (function ({ a } = {}) { eval("var a = 9"); return a; })(), "with initializer");
        }
    },
    {
        name: "destructured parameter, no default",
        body() {
            assert.areEqual(undefined, (function ([a]) { eval("var a"); return a; })([5]), "array pattern");
            assert.areEqual(7, (function ([a]) { eval("var a = 7"); return a; })([5]), "array pattern with initializer");
        }
    },
    {
        name: "eval'd function declaration with a parameter name",
        body() {
            assert.areEqual("function", (function (a = 1) { eval("function a(){}"); return typeof a; })(), "typeof");
            assert.areEqual(42, (function (a = 1) { eval("function a(){ return 42; }"); return a(); })(), "callable");
        }
    },
    {
        name: "non-colliding eval declarations still work",
        body() {
            var r = (function ({ a } = {}) { eval("var b = 3"); return [a, b]; })();
            assert.areEqual(undefined, r[0], "parameter a");
            assert.areEqual(3, r[1], "new var b");
        }
    },
    {
        name: "two evals and a post-eval assignment",
        body() {
            assert.areEqual(2, (function (a = 1) { eval("var a = 2"); eval("var a"); return a; })(), "second eval does not reset");
            assert.areEqual(3, (function (a = 1) { eval("var a"); a = 3; return a; })(), "assignment reaches the var binding");
        }
    },
    {
        name: "body closure created after the eval sees the var",
        body() {
            var r = (function (a = 1) { eval("var a"); return [a, (function () { return a; })()]; })();
            assert.areEqual(undefined, r[0], "body read");
            assert.areEqual(undefined, r[1], "closure read");
        }
    },
    {
        name: "method, generator and arrow forms",
        body() {
            assert.areEqual(undefined, ({ m(a = 1) { eval("var a"); return a; } }).m(), "method");
            assert.areEqual(5, (function* g(a = 1) { eval("var a = 5"); yield a; })().next().value, "generator");
            assert.areEqual(undefined, ((a = 1) => { eval("var a"); return a; })(), "arrow");
        }
    },
    {
        name: "eval var arguments in the body shadows the arguments object binding",
        body() {
            assert.areEqual("undefined", (function (a = 1) { eval("var arguments"); return typeof arguments; })(), "typeof after no-init var");
            assert.areEqual(8, (function (a = 1) { eval("var arguments = 8"); return arguments; })(), "with initializer");
        }
    },
    {
        name: "indirect eval does not touch function environments",
        body() {
            assert.areEqual(1, (function (a = 1) { (0, eval)("var indirectEvalProbe = 1"); return a; })(), "parameter untouched");
            assert.areEqual(1, globalThis.indirectEvalProbe, "the var went to the global object");
        }
    },
    {
        name: "simple parameters: existing binding reused, no SyntaxError",
        body() {
            assert.areEqual(7, (function (a) { eval("var a = 7"); return a; })(), "var with initializer assigns the parameter binding");
            assert.areEqual("function", (function (a) { eval("function a(){}"); return typeof a; })(), "eval function declaration");
            assert.areEqual(1, (function (a) { eval("var a"); return a; })(1), "no initializer keeps the parameter value");
        }
    },
    {
        name: "lexical collisions still throw SyntaxError",
        body() {
            assert.throws(function () { (function (a = 1) { let b; eval("var b"); })(); }, SyntaxError, "body let declared before");
            assert.throws(function () { (function (a = 1) { eval("var q"); let q; })(); }, SyntaxError, "body let declared after");
        }
    },
    {
        name: "the walk stops at the variable environment: an outer lexical binding is not checked and is shadowed",
        body() {
            let outer = 1;
            var r = (function (a = 1) { eval("var outer"); return [outer, a]; })();
            assert.areEqual(undefined, r[0], "the eval var shadows the outer binding for body reads");
            assert.areEqual(1, r[1], "parameter untouched");
        }
    },
    {
        name: "parameter binding remains untouched when the scopes split for a parameter closure",
        body() {
            // The parameter closure forces the param/body split before the
            // body is parsed; the eval var must still not disturb it.
            var r = (function (cb = function () { return a; }, a = 1) { eval("var a = 2"); return [a, cb()]; })();
            assert.areEqual(2, r[0], "body read sees the eval var");
            assert.areEqual(1, r[1], "parameter closure sees the parameter");
        }
    },
    {
        name: "parameter-scope eval (split before body parse): body var collisions with eval vars",
        body() {
            assert.areEqual(5, (function (a = eval("")) { var a = 5; return a; })(), "body var only");
            assert.areEqual(5, (function (a = eval("")) { var a = 5; eval("var a"); return a; })(), "body var then eval var keeps the value");
            assert.areEqual(3, (function (a = eval("")) { function a() { return 3; } eval("var a"); return a(); })(), "function declaration then eval var");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
