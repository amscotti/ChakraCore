//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// RV-02 regression: direct eval must not perform an O(N) stack walk across all
// frames on deep call stacks while preserving class field and derived-constructor
// super() checks.

if (this.WScript && this.WScript.LoadScriptFile) {
    this.WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");
}

function deepDirectEval(n) {
    eval(" ");
    if (n > 0) {
        return deepDirectEval(n - 1);
    }
    return 42;
}

var overflowDepth = 0;
function deepDirectEvalToOverflow() {
    overflowDepth++;
    eval(" ");
    return deepDirectEvalToOverflow();
}

function deepSuperLikeEval(n) {
    try {
        eval("super.prop");
    } catch (e) {
        if (!(e instanceof SyntaxError)) {
            throw e;
        }
    }
    if (n > 0) {
        return deepSuperLikeEval(n - 1);
    }
    return true;
}

var tests = [
    {
        name: "Deep recursive direct eval completes fast and overflows cleanly",
        body: function () {
            assert.strictEqual(42, deepDirectEval(400));
            assert.isTrue(deepSuperLikeEval(300));

            overflowDepth = 0;
            var caught = null;
            try {
                deepDirectEvalToOverflow();
            } catch (e) {
                caught = e;
            }
            assert.isTrue(caught instanceof Error, "expected stack overflow Error");
            assert.strictEqual("Out of stack space", caught.message);
            assert.isTrue(overflowDepth >= 200, "expected deep recursion before overflow");
        }
    },
    {
        name: "Deep recursive direct eval inside class method with super property",
        body: function () {
            class Base {
                value() {
                    return 7;
                }
            }
            class Sub extends Base {
                walk(n) {
                    var v = eval("super.value()");
                    if (n > 0) {
                        return this.walk(n - 1);
                    }
                    return v;
                }
            }
            assert.strictEqual(7, new Sub().walk(400));
        }
    },
    {
        name: "Derived constructor with fields preserves direct and arrow eval super() SyntaxError",
        body: function () {
            class Base {
                constructor() {
                    this.base = 1;
                }
            }

            class Direct extends Base {
                x = 1;
                constructor() {
                    eval("super()");
                }
            }
            assert.throws(function () { new Direct(); }, SyntaxError);

            class Arrow extends Base {
                x = 1;
                constructor() {
                    (() => eval("super()"))();
                }
            }
            assert.throws(function () { new Arrow(); }, SyntaxError);

            class NestedEvalArrow extends Base {
                x = 1;
                constructor() {
                    eval("var s = 'super()'; (() => eval(s))();");
                }
            }
            assert.throws(function () { new NestedEvalArrow(); }, SyntaxError);

            class NestedClassAllowed extends Base {
                x = 2;
                constructor() {
                    var E = eval("(class extends Base { constructor() { super(); this.e = 9; } })");
                    super();
                    this.inner = new E();
                }
            }
            var ok = new NestedClassAllowed();
            assert.strictEqual(2, ok.x);
            assert.strictEqual(9, ok.inner.e);
            assert.strictEqual(1, ok.base);

            class PlainDerived extends Base {
                constructor() {
                    eval("super()");
                }
            }
            assert.strictEqual(1, new PlainDerived().base);

            class FieldArgs {
                x = eval("arguments");
            }
            assert.throws(function () { new FieldArgs(); }, SyntaxError);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
