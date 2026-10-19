//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Direct eval EvalDeclarationInstantiation: a sloppy `var arguments` in a
// parameter initializer conflicts with the parameter environment's arguments
// binding (the implicit arguments object or a parameter named arguments) and
// must throw SyntaxError. Arrow functions have no implicit arguments binding, so
// the same eval is legal there unless a parameter is named arguments. Direct
// eval in an ordinary body remains legal because there the variable environment
// itself holds the arguments binding.

function assertEvalThrows(fn, message) {
    assert.throws(fn, SyntaxError, message);
}

var tests = [
    {
        name: "function forms: parameter-scope eval var arguments throws SyntaxError",
        body() {
            var globalArguments = globalThis.arguments;

            assertEvalThrows(function () {
                function f(p = eval("var arguments")) {}
                f();
            }, "function declaration without a pre-existing binding");

            assertEvalThrows(function () {
                var f = function (p = eval("var arguments")) {};
                f();
            }, "function expression without a pre-existing binding");

            assertEvalThrows(function () {
                var o = { f(p = eval("var arguments")) {} };
                o.f();
            }, "method without a pre-existing binding");

            assertEvalThrows(function () {
                function f(arguments, p = eval("var arguments")) {}
                f(1);
            }, "preceding parameter named arguments");

            assertEvalThrows(function () {
                function f(p = eval("var arguments"), arguments) {}
                f();
            }, "following parameter named arguments");

            assert.areEqual(globalArguments, globalThis.arguments,
                "parameter-scope eval failures must not create a global binding");
        }
    },
    {
        name: "function body var/let/function arguments bindings still collide with the implicit binding",
        body() {
            assertEvalThrows(function () {
                function f(p = eval("var arguments")) { var arguments; }
                f();
            }, "body var arguments");

            assertEvalThrows(function () {
                function f(p = eval("var arguments")) { let arguments; }
                f();
            }, "body let arguments");

            assertEvalThrows(function () {
                function f(p = eval("var arguments")) { function arguments() {} }
                f();
            }, "body function arguments");
        }
    },
    {
        name: "arrow parameter eval does not alias unrelated body bindings",
        body() {
            var f1 = (p = eval("var arguments = 'p'")) => { var d; return d; };
            assert.areEqual(undefined, f1(), "body var after parameter eval");

            var f2 = (p = eval("var arguments = 'p'")) => {
                function d() { return 1; }
                return d();
            };
            assert.areEqual(1, f2(), "body function declaration after parameter eval");

            var f3 = (arguments, p = eval("1")) => { var d; return d; };
            assert.areEqual(undefined, f3("A"), "body var with a parameter named arguments");

            var f4 = (p = eval("var arguments = 'p'"), q = () => arguments) => {
                var d;
                return [d, q()];
            };
            assert.areEqual(undefined, f4()[0], "body var remains undefined");
            assert.areEqual("p", f4()[1], "parameter closure sees the eval binding");
        }
    },
    {
        name: "arrow body writes update the eval-created parameter binding",
        body() {
            var hadArguments = "arguments" in globalThis;
            var oldArguments = globalThis.arguments;

            var f1 = (p = eval("var arguments = 'p'")) => { arguments = "q"; return arguments; };
            assert.areEqual("q", f1(), "simple assignment updates the parameter binding");

            var f2 = (p = eval("var arguments = 'p'")) => { arguments += "x"; return arguments; };
            assert.areEqual("px", f2(), "compound assignment updates the parameter binding");

            var f3 = (p = eval("var arguments = 1")) => { arguments++; return arguments; };
            assert.areEqual(2, f3(), "increment updates the parameter binding");

            var f4 = (p = eval("var arguments = 'p'")) => {
                var result = delete arguments;
                return [result, arguments];
            };
            var deleted = f4();
            assert.isFalse(deleted[0], "eval-created binding is not configurable");
            assert.areEqual("p", deleted[1], "failed delete preserves the binding value");

            assert.areEqual(hadArguments, "arguments" in globalThis,
                "body writes do not create a global arguments property");
            if (hadArguments) {
                assert.areEqual(oldArguments, globalThis.arguments, "global arguments is unchanged");
            }
        }
    },
    {
        name: "body lexical arguments bindings conflict with eval var declarations",
        body() {
            assertEvalThrows(function () {
                function f() { let arguments = 1; eval("var arguments"); }
                f();
            }, "simple parameter list, let");

            assertEvalThrows(function () {
                function f() { const arguments = 1; eval("var arguments"); }
                f();
            }, "simple parameter list, const");

            assertEvalThrows(function () {
                function f(p = 1) { let arguments = 1; eval("var arguments"); }
                f();
            }, "split parameter scope, let");

            assertEvalThrows(function () {
                function* f() { let arguments = 1; eval("var arguments"); yield 1; }
                f().next();
            }, "generator body, let");

            assertEvalThrows(function () {
                function f() {
                    try { throw 1; } catch (e) { const arguments = 1; eval("var arguments"); }
                }
                f();
            }, "catch block, const");

            assertEvalThrows(function () {
                function f() {
                    with ({ x: 1 }) { let arguments = 1; eval("var arguments"); }
                }
                f();
            }, "with block, let");

            assertEvalThrows(function () {
                function f() { { { const arguments = 1; eval("var arguments"); } } }
                f();
            }, "nested blocks, const");
        }
    },
    {
        name: "known residual: top-level lexical arguments hidden by a catch environment",
        body() {
            // The first frame-display environment is the catch scope, which
            // hides the body's top-level lexical binding. This is a documented
            // under-rejection, not conformance; the block/if/with forms above
            // are detected.
            assert.doesNotThrow(function () {
                function f() {
                    let arguments = 1;
                    try { throw 1; } catch (e) { eval("var arguments"); }
                }
                f();
            }, "known residual: catch scope hides the outer lexical binding");
        }
    },
    {
        name: "arrow parameter redeclarations are legal and last value wins",
        body() {
            var f1 = (p = eval("var arguments; var arguments = 2")) => arguments;
            assert.areEqual(2, f1(), "duplicate var declarations in one eval");

            var f2 = (p = (eval("var arguments"), eval("var arguments = 2"))) => arguments;
            assert.areEqual(2, f2(), "duplicate var declarations in two evals");

            var f3 = (p = eval("function arguments() { return 1; } function arguments() { return 2; }")) => arguments();
            assert.areEqual(2, f3(), "duplicate function declarations, last function wins");
        }
    },
    {
        name: "body eval declares a body binding after a parameter eval",
        body() {
            var f = (p = eval("var arguments = 'p'")) => {
                eval("var arguments");
                return typeof arguments;
            };
            assert.areEqual("undefined", f(), "body eval var declaration shadows the parameter eval binding");
        }
    },
    {
        name: "body eval with a parameter named arguments stops at the body variable environment",
        body() {
            function f1(arguments, p = 1) { eval("var arguments = 'b'"); return arguments; }
            assert.areEqual("b", f1("a"), "default parameter");

            function f2(arguments, p = eval("1")) { eval("var arguments = 'b'"); return arguments; }
            assert.areEqual("b", f2("a"), "default expression with direct eval");

            function f3(arguments, ...rest) { eval("var arguments = 'b'"); return arguments; }
            assert.areEqual("b", f3("a"), "rest parameter");

            function f4(arguments, { p } = { p: 1 }) { eval("var arguments = 'b'"); return arguments; }
            assert.areEqual("b", f4("a"), "destructured default parameter");

            var a1 = (arguments, p = 1) => { eval("var arguments = 'b'"); return arguments; };
            assert.areEqual("b", a1("a"), "arrow default parameter");

            var a2 = (arguments, p = eval("1")) => { eval("var arguments = 'b'"); return arguments; };
            assert.areEqual("b", a2("a"), "arrow default expression with direct eval");

            var a3 = (arguments, ...rest) => { eval("var arguments = 'b'"); return arguments; };
            assert.areEqual("b", a3("a"), "arrow rest parameter");

            var a4 = (arguments, { p } = { p: 1 }) => { eval("var arguments = 'b'"); return arguments; };
            assert.areEqual("b", a4("a"), "arrow destructured default parameter");
        }
    },
    {
        name: "generators and async functions evaluate parameter defaults at call time",
        body() {
            assertEvalThrows(function () {
                function* f(p = eval("var arguments")) {}
                f();
            }, "generator parameter default");

            assertEvalThrows(function () {
                async function* f(p = eval("var arguments")) {}
                f();
            }, "async generator parameter default");

            return (async function () {
                var error = null;
                try {
                    var f = async function (p = eval("var arguments")) {};
                    await f();
                } catch (e) {
                    error = e;
                }
                assert.isTrue(error instanceof SyntaxError,
                    "async function parameter default rejects with SyntaxError");
            }());
        }
    },
    {
        name: "arrow functions: no implicit arguments binding, legal eval declares arguments",
        body() {
            var hadArguments = "arguments" in globalThis;
            var oldArguments = globalThis.arguments;

            var a = (p = eval("var arguments = 'param'")) => {
                assert.areEqual("param", arguments, "eval binding visible in arrow body");
                return arguments;
            };
            assert.areEqual("param", a(), "arrow body observes the eval-created binding");

            var b = (p = eval("var arguments;")) => typeof arguments;
            assert.areEqual("undefined", b(), "uninitialized declaration is visible as undefined");

            var c = (p = eval("var arguments = 'param'"), q = () => arguments) => {
                var arguments = "local";
                assert.areEqual("local", arguments, "body var shadows the eval binding");
                assert.areEqual("param", q(), "parameter closure keeps the eval binding");
            };
            c();

            var d = (p = eval("var arguments = 'param'"), q = () => arguments) => {
                let arguments = "local";
                assert.areEqual("local", arguments, "body let shadows the eval binding");
                assert.areEqual("param", q(), "parameter closure keeps the eval binding");
            };
            d();

            var e = (p = eval("var arguments = 'param'"), q = () => arguments) => {
                function arguments() {}
                assert.areEqual("function", typeof arguments, "body function shadows the eval binding");
                assert.areEqual("param", q(), "parameter closure keeps the eval binding");
            };
            e();

            assertEvalThrows(function () {
                var f = (arguments, p = eval("var arguments")) => {};
                f(1);
            }, "arrow with a parameter named arguments");

            assert.areEqual(hadArguments, "arguments" in globalThis,
                "legal arrow eval declarations stay off the global object");
            if (hadArguments) {
                assert.areEqual(oldArguments, globalThis.arguments, "global arguments is unchanged");
            }
        }
    },
    {
        name: "ordinary function body and indirect eval are not rejected",
        body() {
            (function () {
                eval("var arguments = 'body'");
                assert.areEqual("body", arguments,
                    "direct eval in a function body updates the existing arguments binding");
            }("initial"));

            var indirect = eval;
            var hadArguments = "arguments" in globalThis;
            var oldArguments = globalThis.arguments;
            indirect("var arguments = 'indirect'");
            assert.areEqual("indirect", globalThis.arguments, "indirect eval var arguments is legal");
            if (hadArguments) {
                globalThis.arguments = oldArguments;
            } else {
                delete globalThis.arguments;
            }
        }
    },
    {
        name: "error timing is parameter initialization, not body execution",
        body() {
            var bodyRan = false;
            function f(p = eval("var arguments")) { bodyRan = true; }
            f(1);
            assert.isTrue(bodyRan, "a supplied parameter skips the default and the body runs");

            bodyRan = false;
            assertEvalThrows(function () { f(); }, "default evaluation throws before the body");
            assert.isFalse(bodyRan, "the body does not run after the parameter-scope SyntaxError");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
