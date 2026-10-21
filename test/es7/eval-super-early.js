//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: direct eval SuperProperty follows PerformEval's inMethod context
// (doc/c2-s1-eval-super.md). A direct eval caller with a [[HomeObject]] keeps
// its super binding; global code, plain functions, arrows and indirect eval are
// early SyntaxErrors. Eval-defined functions keep their own function context.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "global direct eval super is an early SyntaxError and computed keys do not run",
        body() {
            var evaluated = false;
            assert.throws(function () {
                eval("super.property");
            }, SyntaxError, "global eval super.property");

            assert.throws(function () {
                eval("super[evaluated = true]");
            }, SyntaxError, "global eval super[expr]");
            assert.isFalse(evaluated, "the property expression must not be evaluated");
        }
    },
    {
        name: "plain function, arrow, and strict direct eval super are early SyntaxErrors",
        body() {
            function plain() {
                return eval("super.x");
            }
            function plainIndex() {
                return eval("super['x']");
            }
            function strictPlain() {
                "use strict";
                return eval("super.x");
            }
            var arrow = function () {
                return (() => eval("super.x"))();
            };

            assert.throws(plain, SyntaxError, "plain function");
            assert.throws(plainIndex, SyntaxError, "plain function computed property");
            assert.throws(strictPlain, SyntaxError, "strict plain function");
            assert.throws(arrow, SyntaxError, "arrow function");
        }
    },
    {
        name: "methods and constructors retain the super binding in direct eval",
        body() {
            class Base {
                get value() { return 41; }
                static get staticValue() { return 42; }
            }
            class Derived extends Base {
                method() { return eval("super.value"); }
                computed() { return eval("super['value']"); }
                nested() { return eval("eval('super.value')"); }
                arrowInEval() { return eval("(() => super.value)()"); }
                arrowCaller() {
                    var call = () => eval("super.value");
                    return call();
                }
                constructor() {
                    super();
                    this.fromConstructor = eval("super.value");
                    this.nestedInConstructor = eval("eval('super.value')");
                    this.arrowInConstructor = eval("(() => super.value)()");
                }
            }

            var d = new Derived();
            assert.areEqual(41, d.method(), "class method");
            assert.areEqual(41, d.computed(), "class method computed property");
            assert.areEqual(41, d.nested(), "nested direct eval");
            assert.areEqual(41, d.arrowInEval(), "arrow inside eval");
            assert.areEqual(41, d.arrowCaller(), "eval inside an arrow in a method");
            assert.areEqual(41, d.fromConstructor, "constructor after super()");
            assert.areEqual(41, d.nestedInConstructor, "nested eval in a constructor");
            assert.areEqual(41, d.arrowInConstructor, "arrow inside eval in a constructor");

            var o = {
                method() { return eval("super.value"); },
                nested() { return eval("eval('super.value')"); },
                mapped() { return this.method(); }
            };
            Object.setPrototypeOf(o, { get value() { return 7; } });
            assert.areEqual(7, o.method(), "object literal method");
            assert.areEqual(7, o.nested(), "object literal nested eval");
            assert.areEqual(7, o.mapped(), "borrowed method call");
        }
    },
    {
        name: "field initializers and classes defined by eval keep their own home object",
        body() {
            class Base {
                get value() { return 11; }
            }
            class WithField extends Base {
                field = eval("super.value");
            }
            assert.areEqual(11, new WithField().field, "direct eval in an instance field initializer");

            var EvalClass = eval("(class extends Base { method() { return super.value; } })");
            assert.areEqual(11, new EvalClass().method(), "class defined by eval");

            assert.throws(function () {
                eval("(function () { return super.value; })");
            }, SyntaxError, "plain function defined by eval");
            assert.throws(function () {
                eval("(() => super.value)");
            }, SyntaxError, "arrow defined by global eval");
        }
    },
    {
        name: "nested plain functions and indirect eval still reject super",
        body() {
            class Base {
                get value() { return 1; }
            }
            class Nested extends Base {
                plainFunctionInEval() {
                    return eval("(function () { return super.value; })()");
                }
            }
            assert.throws(function () {
                new Nested().plainFunctionInEval();
            }, SyntaxError, "plain function nested in eval");

            function plain() {
                return eval("eval('super.value')");
            }
            assert.throws(plain, SyntaxError, "nested eval inside a plain function");

            var indirect = eval;
            assert.throws(function () {
                indirect("super.value");
            }, SyntaxError, "indirect eval");

            class IndirectMethod extends Base {
                method() {
                    return indirect("super.value");
                }
            }
            assert.throws(function () {
                new IndirectMethod().method();
            }, SyntaxError, "indirect eval inside a method");
        }
    },
    {
        name: "strict methods keep the same direct eval super behavior",
        body() {
            class Base {
                get value() { return 23; }
            }
            class Strict extends Base {
                method() {
                    "use strict";
                    return eval("super.value");
                }
            }
            assert.areEqual(23, new Strict().method(), "strict method");

            function strictPlain() {
                "use strict";
                return eval("super['value']");
            }
            assert.throws(strictPlain, SyntaxError, "strict plain function computed property");
        }
    }
];

for (var i = 0; i < tests.length; i++) {
    tests[i].body();
}
WScript.Echo("pass");
