//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: public static class fields with simple identifier/literal names and
// optional initializers (doc/c2-s1-static-fields.md). Static private fields,
// static blocks, computed names and decorators are intentionally excluded.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "static field declaration shape, own-property attributes, and undefined initial value",
        body() {
            class C {
                static x;
                static y = 1;
            }

            assert.isTrue(Object.prototype.hasOwnProperty.call(C, "x"));
            assert.isTrue(Object.prototype.hasOwnProperty.call(C, "y"));
            assert.areEqual(undefined, C.x);
            assert.areEqual(1, C.y);

            const xDesc = Object.getOwnPropertyDescriptor(C, "x");
            assert.isTrue(xDesc.writable);
            assert.isTrue(xDesc.enumerable);
            assert.isTrue(xDesc.configurable);
            assert.areEqual(undefined, xDesc.value);

            // Static fields live on the constructor, not on instances or the
            // prototype.
            const c = new C();
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "x"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "y"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C.prototype, "x"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C.prototype, "y"));
            assert.areEqual(undefined, c.x);

            delete C.y;
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "y"));
        }
    },
    {
        name: "initializers run in textual order with this bound to the constructor",
        body() {
            const log = [];
            class C {
                static a = (log.push("a"), 1);
                static b = (log.push("b"), this.a + 1);
                static c = (log.push("c"), this.b + 1);
            }

            assert.areEqual("a,b,c", log.join(","));
            assert.areEqual(1, C.a);
            assert.areEqual(2, C.b);
            assert.areEqual(3, C.c);
        }
    },
    {
        name: "all methods and accessors are installed before static initializers run",
        body() {
            class FieldBeforeMethod {
                static field = typeof this.method;
                static method() {}
            }
            assert.areEqual("function", FieldBeforeMethod.field);

            class MethodBeforeField {
                static method() {}
                static field = typeof this.method;
            }
            assert.areEqual("function", MethodBeforeField.field);

            const log = [];
            class Deferred {
                static a = (log.push("a"), 1);
                static m() { log.push("m"); }
                static b = (log.push("b"), 2);
            }
            assert.areEqual("a,b", log.join(","));
            Deferred.m();
            assert.areEqual("a,b,m", log.join(","));
        }
    },
    {
        name: "static fields are applied after methods/accessors, so a later field wins",
        body() {
            class FieldThenMethod {
                static x = 1;
                static x() { return 2; }
            }
            assert.areEqual(1, FieldThenMethod.x);

            class MethodThenField {
                static x() { return 1; }
                static x = 2;
            }
            assert.areEqual(2, MethodThenField.x);

            class FieldThenAccessor {
                static x = 2;
                static get x() { return 3; }
            }
            assert.areEqual(2, FieldThenAccessor.x);
            const desc = Object.getOwnPropertyDescriptor(FieldThenAccessor, "x");
            assert.isTrue(desc.writable);
            assert.isTrue(desc.enumerable);
            assert.isTrue(desc.configurable);
            assert.areEqual(2, desc.value);
        }
    },
    {
        name: "static initializers can call methods declared later and see computed names first",
        body() {
            class CallsLaterMethod {
                static value = this.compute();
                static compute() { return 7; }
            }
            assert.areEqual(7, CallsLaterMethod.value);

            const log = [];
            class ComputedFirst {
                static a = (log.push("fieldA"), 1);
                [(log.push("computedA"), "m")]() {}
                static b = (log.push("fieldB"), 2);
                [(log.push("computedB"), "n")]() {}
            }
            assert.areEqual("computedA,computedB,fieldA,fieldB", log.join(","));
            assert.areEqual(1, ComputedFirst.a);
            assert.areEqual(2, ComputedFirst.b);
        }
    },
    {
        name: "computed-name failure prevents every static initializer",
        body() {
            let ran = false;
            assert.throws(function () {
                eval("class C { static a = (ran = true, 1); [(() => { throw new Error('computed'); })()]() {} }");
            }, Error, "computed name throws");
            assert.isFalse(ran);
        }
    },
    {
        name: "class name is in TDZ during computed names, then bound before static initializers",
        body() {
            assert.throws(function () {
                eval("class C { [C]() {} }");
            }, ReferenceError, "computed name sees the TDZ binding");

            class C {
                static self = C;
            }
            assert.areEqual(C, C.self);

            assert.throws(function () {
                eval("class D { static a = D; [(() => { throw new Error('stop'); })()]() {} }");
            }, Error, "later computed-name failure still suppresses static fields");
        }
    },
    {
        name: "static initializers see private methods and accessors installed by class elements",
        body() {
            class WithPrivateMethod {
                static value = new WithPrivateMethod().#m();
                #m() { return 5; }
            }
            assert.areEqual(5, WithPrivateMethod.value);

            class WithPrivateAccessor {
                static value = new WithPrivateAccessor().#p;
                get #p() { return 6; }
            }
            assert.areEqual(6, WithPrivateAccessor.value);
        }
    },
    {
        name: "duplicate static names run every initializer and the last definition wins",
        body() {
            const log = [];
            class C {
                static x = (log.push("first"), 1);
                static y = 10;
                static x = (log.push("second"), this.x + 1);
            }

            assert.areEqual("first,second", log.join(","));
            assert.areEqual(2, C.x);
            assert.areEqual(10, C.y);
            assert.areEqual(1, Object.keys(C).filter(function (k) { return k === "x"; }).length);
        }
    },
    {
        name: "static and instance fields coexist without leaking to the other receiver",
        body() {
            class C {
                instance = 1;
                static stat = 2;
                getInstance() { return this.instance; }
            }

            const c = new C();
            assert.areEqual(1, c.instance);
            assert.areEqual(2, C.stat);
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "stat"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "instance"));
            assert.isTrue(Object.prototype.hasOwnProperty.call(c, "instance"));
        }
    },
    {
        name: "static fields coexist with methods and accessors",
        body() {
            class C {
                static x = 3;
                static get doubled() { return this.x * 2; }
                static set doubled(v) { this.x = v / 2; }
                static getX() { return this.x; }
                static *values() { yield this.x; }
            }

            assert.areEqual(3, C.getX());
            assert.areEqual(6, C.doubled);
            C.doubled = 20;
            assert.areEqual(10, C.x);
            assert.areEqual(10, C.values().next().value);
        }
    },
    {
        name: "this in static initializers is the constructor, including arrows and eval",
        body() {
            class C {
                static self = this;
                static arrow = () => this;
                static evaluated = eval("this");
            }

            assert.areEqual(C, C.self);
            assert.areEqual(C, C.arrow());
            assert.areEqual(C, C.evaluated);

            C.self = null;
            assert.areEqual(C, C.arrow());
        }
    },
    {
        name: "class name is visible during static initialization; anonymous expressions use the inferred name",
        body() {
            class C {
                static fromName = C;
            }
            assert.areEqual(C, C.fromName);

            const Named = class Inner {
                static fromName = Inner;
            };
            assert.areEqual(Named, Named.fromName);

            const Anonymous = class {
                static self = this;
                static inferredName = this.name;
            };
            assert.areEqual(Anonymous, Anonymous.self);
            assert.areEqual("Anonymous", Anonymous.inferredName);
        }
    },
    {
        name: "inheritance: derived static initializers see the derived constructor and inherited statics",
        body() {
            const log = [];
            class Base {
                static base = (log.push("base"), 4);
                static method() { return 5; }
            }
            class Derived extends Base {
                static derived = (log.push("derived"), this.base + this.method());
                static self = this;
            }

            assert.areEqual("base,derived", log.join(","));
            assert.areEqual(9, Derived.derived);
            assert.areEqual(Derived, Derived.self);
            assert.isFalse(Object.prototype.hasOwnProperty.call(Derived, "base"));
        }
    },
    {
        name: "static field definition ignores inherited setters and read-only statics",
        body() {
            let setterCalled = false;
            class Base {
                static set value(v) { setterCalled = true; }
                static get readOnly() { return 1; }
            }
            class Derived extends Base {
                static value = 9;
                static readOnly = 2;
            }

            assert.isFalse(setterCalled);
            assert.isTrue(Object.prototype.hasOwnProperty.call(Derived, "value"));
            assert.areEqual(9, Derived.value);
            assert.isTrue(Object.prototype.hasOwnProperty.call(Derived, "readOnly"));
            assert.areEqual(2, Derived.readOnly);
        }
    },
    {
        name: "anonymous static initializer functions and classes receive the field name",
        body() {
            class C {
                static f = function () {};
                static g = () => {};
                static h = class {};
            }
            assert.areEqual("f", C.f.name);
            assert.areEqual("g", C.g.name);
            assert.areEqual("h", C.h.name);
            assert.areEqual(0, C.f.length);
            assert.areEqual(0, C.g.length);
        }
    },
    {
        name: "static field initialization can throw or fail CreateDataProperty",
        body() {
            let reached = false;
            assert.throws(function () {
                class C {
                    static a = 1;
                    static b = (function () { throw new Error("stop"); })();
                    static c = (reached = true, 2);
                }
            }, Error, "initializer exception");
            assert.isFalse(reached, "later fields do not run after an abrupt completion");

            assert.throws(function () {
                eval("class Frozen { static a = Object.freeze(this); static b = 1; }");
            }, TypeError, "freezing the constructor makes a later define fail");

            assert.throws(function () {
                eval("class NonConfigurable { static a = Object.defineProperty(this, 'b', { value: 1, configurable: false }); static b = 2; }");
            }, TypeError, "non-configurable existing property");
        }
    },
    {
        name: "static field names accept string and numeric literals",
        body() {
            class C {
                static "stringName" = 3;
                static 0 = 4;
                static 42;
            }

            assert.areEqual(3, C.stringName);
            assert.areEqual(4, C[0]);
            assert.areEqual(undefined, C[42]);
            assert.isTrue(Object.prototype.hasOwnProperty.call(C, "42"));
        }
    },
    {
        name: "static ASI spellings and static as a field name",
        body() {
            const Newline = eval("(class { static\nx = 1\nstatic y })");
            assert.areEqual(1, Newline.x);
            assert.isTrue(Object.prototype.hasOwnProperty.call(Newline, "y"));
            assert.areEqual(undefined, Newline.y);

            const Asmi = eval("(class { static x = 1\nstatic y = 2 })");
            assert.areEqual(1, Asmi.x);
            assert.areEqual(2, Asmi.y);

            // 'static;' and 'static = 1' are instance fields named static.
            const InstanceNamedStatic = eval("(class { static; static = 1 })");
            const instance = new InstanceNamedStatic();
            assert.isTrue(Object.prototype.hasOwnProperty.call(instance, "static"));
            assert.areEqual(1, instance.static);
            assert.isFalse(Object.prototype.hasOwnProperty.call(InstanceNamedStatic, "static"));

            // 'static static' is the static field arm with name 'static'.
            class StaticNamedStatic {
                static static = 3;
            }
            assert.areEqual(3, StaticNamedStatic.static);

            // get/set followed by a generator via ASI are field + method.
            const GetSet = eval("(class { static get\n*a() { yield 1; } static set\n*b() { yield 2; } })");
            assert.isTrue(Object.prototype.hasOwnProperty.call(GetSet, "get"));
            assert.isTrue(Object.prototype.hasOwnProperty.call(GetSet, "set"));
            assert.areEqual(1, GetSet.prototype.a().next().value);
            assert.areEqual(2, GetSet.prototype.b().next().value);
        }
    },
    {
        name: "contextual static field names: async/get/set and static modifiers",
        body() {
            class C {
                static async = 1;
                static get = 2;
                static set = 3;
                static static = 4;
            }
            assert.areEqual(1, C.async);
            assert.areEqual(2, C.get);
            assert.areEqual(3, C.set);
            assert.areEqual(4, C.static);

            assert.throws(function () { eval("class Bad { static async x = 1 }"); }, SyntaxError, "static async field initializer");
        }
    },
    {
        name: "early errors: static prototype/constructor, static blocks, private methods and computed fields",
        body() {
            assert.throws(function () { eval("class C { static prototype; }"); }, SyntaxError, "static prototype");
            assert.throws(function () { eval("class C { static 'prototype' = 1; }"); }, SyntaxError, "string static prototype");
            assert.throws(function () { eval("class C { static constructor; }"); }, SyntaxError, "static constructor");
            assert.throws(function () { eval("class C { static 'constructor' = 1; }"); }, SyntaxError, "string static constructor");
            assert.throws(function () { eval("class C { static {} }"); }, SyntaxError, "static block");
            assert.doesNotThrow(function () { eval("class C { static #x; }"); }, "static private field");
            assert.throws(function () { eval("class C { static #m() {} }"); }, SyntaxError, "static private method");
            assert.throws(function () { eval("class C { static [0] = 1; }"); }, SyntaxError, "computed static field");
            assert.throws(function () { eval("class C { static [0]; }"); }, SyntaxError, "computed static field without initializer");
        }
    },
    {
        name: "static initializer early errors: arguments, super(), await and yield",
        body() {
            assert.throws(function () { eval("class C { static x = arguments; }"); }, SyntaxError, "arguments");
            assert.throws(function () { eval("class C { static 'x' = () => arguments; }"); }, SyntaxError, "arguments in arrow");
            assert.throws(function () { eval("class C { static x = super(); }"); }, SyntaxError, "super call");
            assert.throws(function () { eval("async function f() { class C { static x = await 1 } }"); }, SyntaxError, "await");
            assert.throws(function () { eval("function* g() { class C { static x = yield 1 } }"); }, SyntaxError, "yield");

            class Good {
                static x = function () { return arguments.length; };
            }
            assert.areEqual(2, Good.x(1, 2));
        }
    },
    {
        name: "static fields are initialized for class declarations, expressions, eval and new Function",
        body() {
            const Expr = class { static x = 10; };
            assert.areEqual(10, Expr.x);

            const EvalC = eval("(class { static x = 11; })");
            assert.areEqual(11, EvalC.x);

            const FunctionCtor = new Function("return (class { static x = 12; })");
            const FunctionC = FunctionCtor();
            assert.areEqual(12, FunctionC.x);
        }
    },
    {
        name: "static initializers read super from the constructor prototype with this bound to the constructor",
        body() {
            class Base {
                static foo = 7;
                static who() { return this.name; }
                static count = 0;
                static inc() { this.count++; return this.count; }
            }
            class Derived extends Base {
                static x = super.foo;
                static byIndex = super["foo"];
                static byArrow = (() => super.foo)();
                static who = super.who();
                static first = super.inc();
                static second = super.inc();
            }
            assert.areEqual(7, Derived.x);
            assert.areEqual(7, Derived.byIndex);
            assert.areEqual(7, Derived.byArrow);
            // super method calls observe the derived constructor as this.
            assert.areEqual("Derived", Derived.who);
            assert.areEqual(1, Derived.first);
            assert.areEqual(2, Derived.second);
            assert.areEqual(2, Derived.count);
            assert.areEqual(0, Base.count);

            // Base classes read Function.prototype: missing names are
            // undefined, inherited members resolve.
            class Plain {
                static missing = super.foo;
                static kind = typeof super.hasOwnProperty;
            }
            assert.areEqual(undefined, Plain.missing);
            assert.areEqual("function", Plain.kind);

            // extends null still resolves through Function.prototype.
            class NullParent extends null {
                static missing = super.foo;
            }
            assert.areEqual(undefined, NullParent.missing);

            // Calling a missing super method throws TypeError at runtime.
            assert.throws(function () { eval("class C extends Base { static x = super.nope(); }"); }, TypeError, "missing super method");

            // super() stays an early error in derived static initializers.
            assert.throws(function () { eval("class C extends Base { static x = super(); }"); }, SyntaxError, "derived super call");
        }
    },
    {
        name: "static initializer super assignment follows ordinary super assignment semantics",
        body() {
            class Base {
                static foo = 7;
                static set s(v) { this.sv = v * 2; }
            }
            // A base setter runs with the derived constructor as receiver.
            class ViaSetter extends Base {
                static done = (super.s = 21);
            }
            assert.areEqual(21, ViaSetter.done);
            assert.areEqual(42, ViaSetter.sv);
            assert.areEqual(undefined, Base.sv);

            // A plain super assignment defines an own property on the
            // derived constructor and leaves the base binding alone.
            class OwnProp extends Base {
                static result = (super.foo = 99);
            }
            assert.areEqual(99, OwnProp.result);
            assert.areEqual(99, OwnProp.foo);
            assert.areEqual(7, Base.foo);
        }
    },
    {
        name: "instance field super remains a parse-time SyntaxError",
        body() {
            // Direct code rejects super in instance field initializers; this
            // residual is unchanged by static super support.
            assert.throws(function () { new Function("class B extends Object { constructor() { super(); } y = super.foo; }"); }, SyntaxError, "instance super.prop");
            assert.throws(function () { new Function("class B extends Object { constructor() { super(); } y = super.foo = 1; }"); }, SyntaxError, "instance super assignment");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
