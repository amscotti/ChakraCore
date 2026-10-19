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
