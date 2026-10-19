//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// PutValue must use the Reference produced when the assignment target was
// evaluated. Object environment records (with) and declarative environment
// records (eval-introduced var) must not be re-resolved after the RHS runs.

var tests = [
    {
        name: "with: compound assignment stores through the captured object environment record",
        body() {
            var x = 0;
            var scope = {
                get x() {
                    delete this.x;
                    return 2;
                }
            };

            with (scope) {
                x *= 3;
            }

            assert.areEqual(6, scope.x, "scope.x is created on the captured with object");
            assert.areEqual(0, x, "the surrounding function binding is not changed");
        }
    },
    {
        name: "with: simple assignment stores through the captured object environment record",
        body() {
            var x = 0;
            var scope = { x: 1 };

            with (scope) {
                x = (delete scope.x, 2);
            }

            assert.areEqual(2, scope.x, "scope.x is recreated on the captured with object");
            assert.areEqual(0, x, "the surrounding function binding is not changed");
        }
    },
    {
        name: "with: nested object environments keep the innermost reference base",
        body() {
            var outerScope = { x: 0 };
            var innerScope = {
                get x() {
                    delete this.x;
                    return 5;
                }
            };

            with (outerScope) {
                with (innerScope) {
                    x %= 3;
                }
            }

            assert.areEqual(2, innerScope.x, "the inner with object receives the result");
            assert.areEqual(0, outerScope.x, "the outer with object is untouched");
        }
    },
    {
        name: "with: a reference that resolves outside all object environments stays there",
        body() {
            var x = 0;
            var scope = {};

            with (scope) {
                x = (scope.x = 2, 1);
            }

            assert.areEqual(2, scope.x, "the RHS-created property remains 2");
            assert.areEqual(1, x, "the captured static binding receives the assignment");
        }
    },
    {
        name: "with: getters and setters run on the captured base",
        body() {
            var stored = 0;
            var scope = {
                get x() {
                    return 1;
                },
                set x(value) {
                    stored = value;
                }
            };

            with (scope) {
                x += 2;
            }

            assert.areEqual(3, stored, "the with object's setter is called with the compound result");
        }
    },
    {
        name: "with: increment and decrement use the captured object environment record",
        body() {
            var x = 0;
            var scope = {
                get x() {
                    delete this.x;
                    return 5;
                }
            };

            with (scope) {
                x++;
            }

            assert.areEqual(6, scope.x, "post-increment creates the property on the with object");
            assert.areEqual(0, x, "the surrounding binding is not incremented");

            var y = 0;
            var decScope = {
                get y() {
                    delete this.y;
                    return 5;
                }
            };

            with (decScope) {
                --y;
            }

            assert.areEqual(4, decScope.y, "pre-decrement creates the property on the with object");
            assert.areEqual(0, y, "the surrounding binding is not decremented");
        }
    },
    {
        name: "eval: compound assignment keeps the outer declarative binding",
        body() {
            var x = 3;
            var innerX = (function () {
                x *= (eval("var x = 2;"), 4);
                return x;
            })();

            assert.areEqual(2, innerX, "the eval-introduced var keeps its value");
            assert.areEqual(12, x, "the captured outer binding receives the result");
        }
    },
    {
        name: "eval: simple assignment keeps the outer declarative binding",
        body() {
            var x = 0;
            var innerX = (function () {
                x = (eval("var x = 2;"), 1);
                return x;
            })();

            assert.areEqual(2, innerX, "the eval-introduced var keeps its value");
            assert.areEqual(1, x, "the captured outer binding receives the value");
        }
    },
    {
        name: "eval: local with statements are checked before the caller environment",
        body() {
            var scope = { x: "a" };
            eval("with (scope) { x += 'b'; }");
            assert.areEqual("ab", scope.x, "eval-local with object receives the compound result");

            var counter = { x: 1 };
            eval("with (counter) { x++; }");
            assert.areEqual(2, counter.x, "eval-local with object receives the increment");
        }
    },
    {
        name: "eval: unresolvable and non-writable bindings keep their errors",
        body() {
            assert.throws(function () {
                eval("'use strict'; referenceBaseNeverDeclared = 1;");
            }, ReferenceError, "strict unresolvable assignment still throws ReferenceError");

            assert.throws(function () {
                const immutable = 1;
                eval("immutable = 2;");
            }, TypeError, "assignment to a const binding still throws TypeError");

            assert.throws(function () {
                eval("initializedLater = 2;");
                let initializedLater = 1;
            }, ReferenceError, "assignment to a TDZ binding still throws ReferenceError");
        }
    },
    {
        name: "with: compound and logical reads use the captured base once",
        body() {
            var S = Symbol.unscopables;
            var n = 0;
            var o = { x: 2 };
            var x = 10;
            Object.defineProperty(o, S, {
                get() {
                    return ++n === 1 ? {} : { x: true };
                }
            });

            with (o) {
                x += 3;
            }

            assert.areEqual(1, n, "unscopables is read only during reference evaluation");
            assert.areEqual(5, o.x, "the captured base supplies and receives the compound value");
            assert.areEqual(10, x, "the outer binding is untouched");

            n = 0;
            var l = { x: 0 };
            Object.defineProperty(l, S, {
                get() {
                    return ++n === 1 ? {} : { x: true };
                }
            });

            with (l) {
                x ||= (l.rhs = 7);
            }

            assert.areEqual(1, n, "logical assignment does not re-run the unscopables lookup");
            assert.areEqual(7, l.x, "the captured base receives the logical result");
            assert.areEqual(10, x, "the outer binding is still untouched");
        }
    },
    {
        name: "with: unscopable fallback does not re-read unscopables",
        body() {
            var S = Symbol.unscopables;
            var n = 0;
            var o = { x: 2 };
            var x = 10;
            Object.defineProperty(o, S, {
                get() {
                    return n++ === 0 ? { x: true } : {};
                }
            });

            with (o) {
                x++;
            }

            assert.areEqual(1, n, "the with binding is rejected once and the static reference is used");
            assert.areEqual(2, o.x, "the rejected with binding is not written");
            assert.areEqual(11, x, "the outer binding is incremented");
        }
    },
    {
        name: "with: proxy fallback does not re-run HasProperty",
        body() {
            var calls = 0;
            var target = { x: 2 };
            var proxy = new Proxy(target, {
                has(t, key) {
                    if (key === "x") {
                        calls++;
                        return calls !== 1;
                    }
                    return key in t;
                },
                get(t, key) {
                    return t[key];
                },
                set(t, key, value) {
                    t[key] = value;
                    return true;
                }
            });
            var x = 10;

            with (proxy) {
                x++;
            }

            assert.areEqual(1, calls, "the static fallback does not probe the proxy again");
            assert.areEqual(2, target.x, "the rejected proxy binding is not written");
            assert.areEqual(11, x, "the outer binding is incremented");
        }
    },
    {
        name: "with: increment reads and writes the captured base once",
        body() {
            var S = Symbol.unscopables;
            var n = 0;
            var o = { x: 2 };
            var x = 10;
            Object.defineProperty(o, S, {
                get() {
                    return ++n === 1 ? {} : { x: true };
                }
            });

            with (o) {
                x++;
            }

            assert.areEqual(1, n, "increment does not re-run the unscopables lookup");
            assert.areEqual(3, o.x, "the captured base is incremented");
            assert.areEqual(10, x, "the outer binding is untouched");
        }
    },
    {
        name: "with: proxy HasProperty runs for both reference and PutValue, not GetValue",
        body() {
            function makeScope() {
                var hasCalls = 0;
                var target = { x: 1 };
                var proxy = new Proxy(target, {
                    has(t, key) {
                        if (key === "x") {
                            hasCalls++;
                        }
                        return key in t;
                    },
                    get(t, key) {
                        return t[key];
                    },
                    set(t, key, value) {
                        t[key] = value;
                        return true;
                    }
                });
                return { proxy: proxy, target: target, calls: function () { return hasCalls; } };
            }

            var simple = makeScope();
            with (simple.proxy) {
                x = 2;
            }
            assert.areEqual(2, simple.calls(), "simple assignment probes HasProperty at both steps");
            assert.areEqual(2, simple.target.x, "simple assignment stores through the proxy");

            var compound = makeScope();
            with (compound.proxy) {
                x += 2;
            }
            assert.areEqual(3, compound.calls(), "compound probes HasProperty for reference, GetBindingValue and PutValue");
            assert.areEqual(3, compound.target.x, "compound assignment stores through the proxy");
        }
    },
    {
        name: "with: strict GetBindingValue missing binding throws before the RHS",
        body() {
            var calls = 0;
            var target = { x: 1 };
            var proxy = new Proxy(target, {
                has(t, key) {
                    if (key === "x") {
                        calls++;
                        return calls === 1;
                    }
                    return key in t;
                },
                get(t, key) {
                    return t[key];
                }
            });
            var rhsRan = false;

            assert.throws(function () {
                with (proxy) {
                    (function () {
                        "use strict";
                        x += (rhsRan = true, 1);
                    })();
                }
            }, ReferenceError, "strict GetBindingValue throws when HasProperty is false");

            assert.areEqual(2, calls, "reference and GetBindingValue each probe HasProperty");
            assert.isFalse(rhsRan, "the RHS does not run after the GetBindingValue ReferenceError");
        }
    },
    {
        name: "with: function expression name is shadowed by the object environment",
        body() {
            var fn = function name() {
                var seen;
                with ({ name: 5 }) {
                    name = 7;
                    seen = name;
                }
                return seen;
            };
            assert.areEqual(7, fn(), "the with binding receives the assignment");
        }
    },
    {
        name: "with: variable RHS keeps captured-base temps balanced",
        body() {
            var o = { x: 1, y: 2, a: 0, b: 1 };

            with (o) {
                x = y;
            }
            assert.areEqual(2, o.x, "simple assignment with a variable RHS");

            with (o) {
                x += y;
            }
            assert.areEqual(4, o.x, "compound assignment with a variable RHS");

            with (o) {
                a ||= y;
            }
            assert.areEqual(2, o.a, "logical assignment with a variable RHS");

            with (o) {
                b &&= y;
            }
            assert.areEqual(2, o.b, "short-circuit assignment with a variable RHS");

            with (o) {
                x++;
                ++y;
            }
            assert.areEqual(5, o.x, "postfix increment on a captured base");
            assert.areEqual(3, o.y, "prefix increment on a captured base");
        }
    },
    {
        name: "with: typed-array prototype canonical numeric non-index is a no-op",
        body() {
            var typedArray = new Int32Array(10);
            var env = Object.create(typedArray);
            Object.defineProperty(env, "NaN", {
                configurable: true,
                value: 100
            });

            with (env) {
                NaN = (delete env.NaN, 0);
            }

            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(env, "NaN"),
                "the deleted with binding is not recreated");

            var direct = Object.create(new Int32Array(4));
            direct.NaN = 0;
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(direct, "NaN"),
                "direct assignment follows the typed-array prototype [[Set]] no-op");
        }
    },
    {
        name: "with: strict assignment to a deleted binding throws and does not recreate it",
        body() {
            var x = 0;
            var scope = { x: 1 };
            var outcome = "no throw";

            with (scope) {
                (function () {
                    "use strict";
                    try {
                        x = (delete scope.x, 2);
                    } catch (e) {
                        outcome = e.name;
                    }
                })();
            }

            assert.areEqual("ReferenceError", outcome, "SetMutableBinding with S=true throws for a deleted binding");
            assert.isFalse("x" in scope, "the strict assignment does not recreate scope.x");
            assert.areEqual(0, x, "the surrounding binding is not changed");
        }
    },
    {
        name: "with: reference evaluation precedes the RHS side effect",
        body() {
            var order = [];
            var x = 0;
            var scope = {
                get x() {
                    order.push("get");
                    delete this.x;
                    return 2;
                }
            };

            with (scope) {
                x *= (order.push("rhs"), 3);
            }

            assert.areEqual("get,rhs", order.join(","), "GetValue runs before the RHS");
            assert.areEqual(6, scope.x, "the captured with object receives the result");
            assert.areEqual(0, x, "the outer binding is untouched");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
