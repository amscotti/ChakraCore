//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: public instance class fields with simple identifier names and
// optional initializers (doc/c2-s1-public-fields.md). Private/static/computed
// elements and decorators are intentionally not part of this slice.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "field declaration shape, own-property attributes, and undefined initial value",
        body() {
            class C {
                x;
                y = 1;
            }

            const c = new C();
            assert.isTrue(Object.prototype.hasOwnProperty.call(c, "x"));
            assert.isTrue(Object.prototype.hasOwnProperty.call(c, "y"));
            assert.areEqual(undefined, c.x);
            assert.areEqual(1, c.y);

            const xDesc = Object.getOwnPropertyDescriptor(c, "x");
            assert.isTrue(xDesc.writable);
            assert.isTrue(xDesc.enumerable);
            assert.isTrue(xDesc.configurable);
            assert.areEqual(undefined, xDesc.value);

            const names = Object.keys(c);
            assert.areEqual(2, names.length);
            assert.areEqual("x", names[0]);
            assert.areEqual("y", names[1]);

            delete c.y;
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "y"));
        }
    },
    {
        name: "initializers run in textual order with this bound to the instance",
        body() {
            const log = [];
            class C {
                a = (log.push("a"), 1);
                b = (log.push("b"), this.a + 1);
                c = (log.push("c"), this.b + 1);
            }

            const c = new C();
            assert.areEqual("a,b,c", log.join(","));
            assert.areEqual(1, c.a);
            assert.areEqual(2, c.b);
            assert.areEqual(3, c.c);
        }
    },
    {
        name: "per-construction evaluation and duplicate public names (last definition wins)",
        body() {
            let counter = 0;
            class C {
                x = ++counter;
                y = 10;
                x = ++counter;
            }

            const first = new C();
            const second = new C();
            assert.areEqual(2, first.x);
            assert.areEqual(4, second.x);
            assert.areEqual(10, first.y);
            assert.areEqual(2, Object.keys(first).length);
        }
    },
    {
        name: "this, closures, and later fields follow normal evaluation",
        body() {
            class C {
                a = 5;
                arrow = () => this.a + this.b;
                b = 7;
                later = this.f;
                f = 11;
            }

            const c = new C();
            assert.areEqual(12, c.arrow());
            assert.areEqual(undefined, c.later);
            assert.areEqual(11, c.f);

            c.a = 100;
            assert.areEqual(107, c.arrow());
        }
    },
    {
        name: "fields and methods/accessors coexist; methods see initialized fields",
        body() {
            class C {
                x = 3;
                getX() { return this.x; }
                get doubled() { return this.x * 2; }
                set doubled(v) { this.x = v / 2; }
                *values() { yield this.x; }
            }

            const c = new C();
            assert.areEqual(3, c.getX());
            assert.areEqual(6, c.doubled);
            c.doubled = 20;
            assert.areEqual(10, c.x);
            assert.areEqual(10, c.values().next().value);
        }
    },
    {
        name: "base class fields initialize before parameter defaults and the constructor body",
        body() {
            const log = [];
            class C {
                x = (log.push("field"), 42);
                constructor(a = (log.push("param"), this.x)) {
                    log.push("body", a, this.x);
                }
            }

            new C();
            assert.areEqual("field,param,body", log.slice(0, 3).join(","));
            assert.areEqual(42, log[3]);
            assert.areEqual(42, log[4]);
        }
    },
    {
        name: "derived fields initialize right after super() and before the body resumes",
        body() {
            const log = [];
            class Base {
                base = (log.push("base-field"), 1);
                constructor() { log.push("base-ctor"); }
            }
            class Derived extends Base {
                derived = (log.push("derived-field"), this.base + 1);
                constructor() {
                    log.push("before-super");
                    super();
                    log.push("after-super", this.base, this.derived);
                }
            }

            const d = new Derived();
            assert.areEqual("before-super,base-field,base-ctor,derived-field,after-super", log.slice(0, 5).join(","));
            assert.areEqual(1, log[5]);
            assert.areEqual(2, log[6]);
            assert.areEqual(2, d.derived);
        }
    },
    {
        name: "derived class with no explicit constructor initializes fields after the implicit super",
        body() {
            class Base {
                base = 4;
            }
            class Derived extends Base {
                derived = this.base + 1;
            }

            const d = new Derived();
            assert.areEqual(4, d.base);
            assert.areEqual(5, d.derived);
        }
    },
    {
        name: "field definition uses CreateDataProperty semantics (no inherited setter call)",
        body() {
            let setterCalled = false;
            class Base {
                set value(v) { setterCalled = true; }
            }
            class Derived extends Base {
                value = 9;
            }

            const d = new Derived();
            assert.isFalse(setterCalled);
            assert.isTrue(Object.prototype.hasOwnProperty.call(d, "value"));
            assert.areEqual(9, d.value);
        }
    },
    {
        name: "fields are writable per instance and visible as normal assignments",
        body() {
            class C {
                x = 1;
            }

            const a = new C();
            const b = new C();
            a.x = 8;
            assert.areEqual(8, a.x);
            assert.areEqual(1, b.x);
            a.x += 1;
            assert.areEqual(9, a.x);
        }
    },
    {
        name: "class expressions and contextual names used as field names",
        body() {
            const K = class {
                async;
                get;
                set;
                x = 1;
                getX() { return this.x; }
            };
            const k = new K();
            assert.areEqual(1, k.getX());
            assert.areEqual(undefined, k.async);
            assert.areEqual(undefined, k.get);
            assert.areEqual(undefined, k.set);

            // The spec forbids a public field named "constructor".
            assert.throws(function () { eval("class Bad { constructor; }"); }, SyntaxError, "constructor field is an early error");
            assert.throws(function () { eval("class Bad { 'constructor' = 1; }"); }, SyntaxError, "string constructor field is an early error");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
