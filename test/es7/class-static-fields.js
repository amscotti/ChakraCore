//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: public static class fields with simple identifier/literal names,
// computed names and optional initializers (doc/c2-s1-static-fields.md,
// doc/c2-s1-computed-instance-super.md), plus static initialization blocks
// (doc/c2-s1-static-blocks.md). Static private fields are covered in
// class-static-private-fields.js; decorators are intentionally excluded.

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
        name: "early errors: static prototype/constructor (static blocks, private methods, computed fields supported)",
        body() {
            assert.throws(function () { eval("class C { static prototype; }"); }, SyntaxError, "static prototype");
            assert.throws(function () { eval("class C { static 'prototype' = 1; }"); }, SyntaxError, "string static prototype");
            assert.throws(function () { eval("class C { static constructor; }"); }, SyntaxError, "static constructor");
            assert.throws(function () { eval("class C { static 'constructor' = 1; }"); }, SyntaxError, "string static constructor");
            // Static blocks are supported (doc/c2-s1-static-blocks.md).
            assert.doesNotThrow(function () { eval("class C { static {} }"); }, "static block");
            assert.doesNotThrow(function () { eval("class C { static #x; }"); }, "static private field");
            assert.doesNotThrow(function () { eval("class C { static #m() {} }"); }, "static private method");
            // Computed static field names are evaluated, not rejected: only a
            // "prototype" key throws, at class definition time.
            assert.doesNotThrow(function () { eval("class C { static [0] = 1; }"); }, "computed static field");
            assert.doesNotThrow(function () { eval("class C { static [0]; }"); }, "computed static field without initializer");
            assert.throws(function () { eval("class C { static ['prototype'] = 1; }"); }, TypeError, "computed static prototype");
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
            // `await` is reserved in static initializers even outside async
            // contexts: bare references and declarations are early errors,
            // while nested arrows/functions recompute it as an identifier.
            assert.throws(function () { eval("class C { static x = await; }"); }, SyntaxError, "bare await identifier");
            assert.throws(function () { eval("class C { static x = await 1; }"); }, SyntaxError, "await expression at script level");
            assert.doesNotThrow(function () { eval("class C { static x = (function(){ var await = 3; return await; })(); }"); }, "await identifier in nested plain function");
            assert.doesNotThrow(function () { eval("class C { static x = (async () => await 1)(); }"); }, "await in nested async arrow");

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
        name: "instance field super is allowed with the prototype as HomeObject",
        body() {
            // Instance helpers resolve super against the derived prototype
            // with the instance as receiver (see class-fields-public.js for
            // the full matrix); smoke-test both shapes from the static file.
            class Base { }
            Base.prototype.pd = 5;
            class Derived extends Base {
                x = super.pd;
                y = (super.q = 9, this.q);
            }
            const d = new Derived();
            assert.areEqual(5, d.x);
            assert.areEqual(9, d.y);
        }
    },
    {
        name: "computed static field names evaluate once in textual order",
        body() {
            const log = [];
            function key(name) { log.push(name); return name; }
            class C {
                [key("m1")]() { return 1; }
                static a = (log.push("init:a"), 1);
                static [key("s1")] = 2;
                [key("m2")]() { return 3; }
                static [key("s2")] = (log.push("init:s2"), 4);
            }
            // Every key evaluates at class definition time in textual order,
            // interleaved with methods; static initializers run after all
            // members, in their own textual order.
            assert.areEqual("m1,s1,m2,s2,init:a,init:s2", log.join(","));
            assert.areEqual(1, C.a);
            assert.areEqual(2, C.s1);
            assert.areEqual(4, C.s2);
            assert.areEqual("a,s1,s2", Object.keys(C).join(","));

            // ToPropertyKey runs once per element.
            let count = 0;
            const keyObj = { toString() { count++; return "k"; } };
            const D = eval("(class { static [keyObj] = 1; static [keyObj] = 2; })");
            void D;
            assert.areEqual(2, count);
        }
    },
    {
        name: "computed static field keys: scope, duplicates, special names and value naming",
        body() {
            // Outer bindings are visible; the class name is in its TDZ.
            let v = "w";
            class C {
                static [v] = 1;
            }
            assert.areEqual(1, C.w);
            assert.throws(function () { eval("class Bad { static [Bad] = 1; }"); }, ReferenceError, "class name TDZ in key");

            // Symbol keys and duplicate last-wins.
            const s = Symbol("s");
            let runs = 0;
            class D {
                static [s] = 1;
                static ["d"] = (runs++, 10);
                static ["d"] = (runs++, 20);
            }
            assert.areEqual(1, D[s]);
            assert.areEqual(20, D.d);
            assert.areEqual(2, runs);

            // "constructor" defines an own property; bare keys define undefined.
            class E {
                static ["constructor"] = 1;
                static ["bare"];
            }
            assert.areEqual(1, E.constructor);
            assert.isTrue(Object.prototype.hasOwnProperty.call(E, "bare"));
            assert.areEqual(undefined, E.bare);

            // Anonymous function and class values take the computed key name.
            class F {
                static ["nf"] = function () { };
                static ["nc"] = class { };
            }
            assert.areEqual("nf", F.nf.name);
            assert.areEqual("nc", F.nc.name);

            // A throwing key suppresses every later static initializer.
            let ran = false;
            assert.throws(function () {
                eval("class G { static [(() => { throw new Error('boom'); })()] = 1; static s = (ran = true, 2); }");
            }, Error, "key throw");
            assert.isFalse(ran);
        }
    },
    {
        name: "static blocks evaluate in textual order interleaved with static fields",
        body() {
            const log = [];
            class C {
                static a = (log.push("a"), 1);
                static { log.push("b1"); this.fromFirst = this.a; }
                static b = (log.push("b"), this.fromFirst + 1);
                static { log.push("b2"); this.fromSecond = this.b; }
            }

            assert.areEqual("a,b1,b,b2", log.join(","));
            assert.areEqual(1, C.a);
            assert.areEqual(1, C.fromFirst);
            assert.areEqual(2, C.b);
            assert.areEqual(2, C.fromSecond);

            // A block before a field observes the field as absent, matching
            // textual evaluation order.
            const Later = class {
                static { this.early = this.late; }
                static late = 5;
            };
            assert.areEqual(undefined, Later.early);
            assert.areEqual(5, Later.late);

            // Empty blocks and semicolon-adjacent blocks are valid.
            assert.doesNotThrow(function () { eval("class E { static {} }"); });
            const Semi = eval("(class { static { this.x = 1; }; static y = 2; })");
            assert.areEqual(1, Semi.x);
            assert.areEqual(2, Semi.y);

            // `static` followed by a newline and `{` is still a block.
            const Newline = eval("(class { static\n{ this.x = 3; } })");
            assert.areEqual(3, Newline.x);
        }
    },
    {
        name: "static block this is the constructor and methods are installed first",
        body() {
            let seen;
            let arrowSeen;
            class C {
                static {
                    seen = this;
                    arrowSeen = (() => this)();
                    this.installed = 1;
                }
                static method() { return 2; }
            }
            assert.areEqual(C, seen);
            assert.areEqual(C, arrowSeen);
            assert.areEqual(1, C.installed);

            class FieldBeforeMethod {
                static { this.kind = typeof this.method; }
                static method() {}
            }
            assert.areEqual("function", FieldBeforeMethod.kind);

            class WithPrivate {
                static { this.v = new WithPrivate().#m() + this.#sm(); }
                #m() { return 5; }
                static #sm() { return 6; }
                static get #p() { return 7; }
                static { this.w = this.#p; }
            }
            assert.areEqual(11, WithPrivate.v);
            assert.areEqual(7, WithPrivate.w);
        }
    },
    {
        name: "static blocks read super with the constructor as HomeObject",
        body() {
            class Base {
                static foo = 7;
                static who() { return this.name; }
            }
            class Derived extends Base {
                static { this.x = super.foo; }
                static { this.byIndex = super["foo"]; }
                static { this.byArrow = (() => super.foo)(); }
                static { this.who = super.who(); }
                static y = super.foo;
            }
            assert.areEqual(7, Derived.x);
            assert.areEqual(7, Derived.byIndex);
            assert.areEqual(7, Derived.byArrow);
            assert.areEqual("Derived", Derived.who);
            assert.areEqual(7, Derived.y);

            // Base classes resolve through Function.prototype.
            class Plain {
                static { this.missing = super.foo; this.kind = typeof super.hasOwnProperty; }
            }
            assert.areEqual(undefined, Plain.missing);
            assert.areEqual("function", Plain.kind);

            // Super assignment in a block follows ordinary semantics.
            class ViaAssign extends Base {
                static { super.foo = 9; }
            }
            assert.areEqual(9, ViaAssign.foo);
            assert.areEqual(7, Base.foo);

            // super() stays an early error.
            assert.throws(function () { eval("class X extends Base { static { super(); } }"); }, SyntaxError, "super call");
            assert.throws(function () { new Function("class X extends Base { static { super(); } }"); }, SyntaxError, "super call in Function");
            // Note: super inside a nested plain function is rejected in the
            // full parse but shares the field-initializer residuals under
            // deferred parsing (eval defers to runtime; -force:deferparse
            // skips nested bodies until called), so it is covered by
            // node-differential probes instead of this file.
        }
    },
    {
        name: "static blocks have a function-like own scope",
        body() {
            class C {
                static {
                    var v = 1;
                    function f() { return 2; }
                    let l = 3;
                    const k = 4;
                    this.captured = [v, f(), l, k];
                }
            }
            assert.areEqual("1,2,3,4", C.captured.join(","));

            // var/function declarations do not leak out of the block.
            assert.areEqual("undefined", typeof v);
            assert.areEqual("undefined", typeof f);
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "v"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "f"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "l"));

            // Duplicate vars/functions are allowed; duplicate lexicals and
            // lexical/var collisions are early errors.
            const DupVar = eval("(class { static { var a = 1; var a = 2; this.r = a; } })");
            assert.areEqual(2, DupVar.r);
            const DupFn = eval("(class { static { function g() { return 1; } function g() { return 2; } this.r = g(); } })");
            assert.areEqual(2, DupFn.r);
            assert.throws(function () { eval("class X { static { let q = 1; let q = 2; } }"); }, SyntaxError, "duplicate let");
            assert.throws(function () { eval("class X { static { var q = 1; let q = 2; } }"); }, SyntaxError, "var/let collision");
            assert.throws(function () { eval("class X { static { let q = 1; function q() {} } }"); }, SyntaxError, "let/function collision");

            // Each block gets its own scope.
            class Two {
                static { var shared = 1; this.first = shared; }
                static { var shared = 2; this.second = shared; }
            }
            assert.areEqual(1, Two.first);
            assert.areEqual(2, Two.second);
        }
    },
    {
        name: "static block declaration scope attribution agrees under deferred parsing",
        body() {
            // The fast scan must attribute block-level declarations to the
            // block's own function-like scope, exactly like the full parse:
            // intra-block lexical/var collisions are early errors, while
            // block locals never collide with the class name, outer
            // bindings, or enclosing parameters. Error shapes run through
            // eval/new Function (eager full parse); the valid shapes below
            // run directly, so this file's -force:deferparse variant scans
            // them with the deferred scope (doc/c2-s1-static-blocks.md).
            assert.throws(function () { eval("class X { static { let a = 1; var a = 2; } }"); }, SyntaxError, "let/var");
            assert.throws(function () { eval("class X { static { var a = 1; let a = 2; } }"); }, SyntaxError, "var/let");
            assert.throws(function () { eval("class X { static { const a = 1; var a = 2; } }"); }, SyntaxError, "const/var");
            assert.throws(function () { eval("class X { static { var a = 1; const a = 2; } }"); }, SyntaxError, "var/const");
            assert.throws(function () { eval("class X { static { let a = 1; let a = 2; } }"); }, SyntaxError, "duplicate let");
            assert.throws(function () { eval("class X { static { let a = 1; { var a = 2; } } }"); }, SyntaxError, "let/nested var");
            assert.throws(function () { eval("class X { static { { var a = 1; } let a = 2; } }"); }, SyntaxError, "nested var/let");
            assert.throws(function () { eval("class X { static { for (var i = 0; i < 1; i++) {} let i = 2; } }"); }, SyntaxError, "for var/let");
            assert.throws(function () { eval("class X { static { switch (1) { case 1: var s = 5; } let s = 6; } }"); }, SyntaxError, "switch var/let");
            assert.throws(function () { eval("class X { static { try {} catch (e) { var q = 1; } let q = 2; } }"); }, SyntaxError, "catch var/let");
            assert.throws(function () { new Function("class X { static { let a = 1; var a = 2; } }"); }, SyntaxError, "let/var in Function");
            assert.throws(function () { new Function("class X { static { var a = 1; let a = 2; } }"); }, SyntaxError, "var/let in Function");

            // A block let/const/var may shadow the class name.
            class ShadowLet {
                static { let ShadowLet = 5; this.v = ShadowLet; }
            }
            assert.areEqual(5, ShadowLet.v);
            class ShadowConst {
                static { const ShadowConst = 6; this.v = ShadowConst; }
            }
            assert.areEqual(6, ShadowConst.v);
            class ShadowVar {
                static { var ShadowVar = 7; this.v = ShadowVar; }
            }
            assert.areEqual(7, ShadowVar.v);
            class ShadowFn {
                static { function ShadowFn() { return 8; } this.v = ShadowFn(); }
            }
            assert.areEqual(8, ShadowFn.v);

            // A block var is block-scoped: it coexists with outer
            // function-scope let/const/var bindings of the same name.
            let z = 1;
            const y = 10;
            var w = 100;
            class Outer {
                static { var z = 2; var y = 20; var w = 200; this.v = [z, y, w]; }
            }
            assert.areEqual("2,20,200", Outer.v.join(","));
            assert.areEqual(1, z);
            assert.areEqual(10, y);
            assert.areEqual(100, w);

            // Nested blocks, nested functions, and split-parameter enclosing
            // functions keep the same attribution.
            class Nested {
                static {
                    let n = 1;
                    {
                        var m = 2;
                        function h() { var n = 3; return n + m; }
                        this.v = [n, m, h()];
                    }
                }
            }
            assert.areEqual("1,2,5", Nested.v.join(","));
            const split = ([p]) => {
                class SP {
                    static { let p = 4; var q = 5; this.v = [p, q]; }
                }
                return SP.v;
            };
            assert.areEqual("4,5", split([40]).join(","));

            // Uses of outer bindings and separate per-block scopes still work.
            class UsesOuter {
                static { this.v = z + y + w; }
            }
            assert.areEqual(111, UsesOuter.v);
        }
    },
    {
        name: "static block early errors: arguments, await, yield, return, break and continue",
        body() {
            assert.throws(function () { eval("class C { static { console.log(arguments); } }"); }, SyntaxError, "arguments");
            assert.throws(function () { eval("class C { static { let f = () => arguments; } }"); }, SyntaxError, "arguments in arrow");
            assert.throws(function () { eval("async function f() { class C { static { await 1; } } }"); }, SyntaxError, "await");
            assert.throws(function () { eval("class C { static { await; } }"); }, SyntaxError, "bare await identifier at script level");
            assert.throws(function () { eval("class C { static { var await = 1; } }"); }, SyntaxError, "await declaration at script level");
            assert.throws(function () { eval("function* g() { class C { static { yield 1; } } }"); }, SyntaxError, "yield");
            assert.throws(function () { eval("class C { static { return 1; } }"); }, SyntaxError, "return");
            assert.throws(function () { eval("class C { static { if (true) return; } }"); }, SyntaxError, "return in branch");
            assert.throws(function () { eval("class C { static { for (let i = 0; i < 1; i++) { return; } } }"); }, SyntaxError, "return in loop");
            assert.throws(function () { eval("class C { static { break; } }"); }, SyntaxError, "bare break");
            assert.throws(function () { eval("class C { static { continue; } }"); }, SyntaxError, "bare continue");
            assert.throws(function () { eval("outer: for (let i = 0; i < 1; i++) { class C { static { break outer; } } }"); }, SyntaxError, "outer label");

            // new.target evaluates to undefined; nested functions keep their
            // own return and arguments rules.
            class N {
                static { this.hasNewTarget = new.target === undefined; }
            }
            assert.isTrue(N.hasNewTarget);

            const Nested = eval("(class { static { function f() { return arguments.length; } this.r = f(1, 2); } })");
            assert.areEqual(2, Nested.r);

            // Inner loops, labels and nested-function returns are valid.
            const Flow = eval("(class { static { let n = 0; for (let i = 0; i < 3; i++) { if (i === 1) continue; n++; } lbl: { break lbl; } function f() { return 7; } this.r = n + f(); } })");
            assert.areEqual(9, Flow.r);
        }
    },
    {
        name: "static blocks work in base and derived classes and propagate exceptions",
        body() {
            // Anonymous and named class expressions.
            const Anon = class { static { this.x = 10; } };
            assert.areEqual(10, Anon.x);
            const Named = class Inner {
                static x = 5;
                static { this.y = Inner.x + 1; }
            };
            assert.areEqual(6, Named.y);

            class Derived extends Object {
                static { this.z = 3; }
            }
            assert.areEqual(3, Derived.z);

            // An exception propagates out of the class definition and later
            // static elements do not run.
            let reached = false;
            assert.throws(function () {
                eval("class T { static a = 1; static { throw new Error('boom'); } static b = (reached = true, 2); }");
            }, Error, "block exception");
            assert.isFalse(reached);

            // The class binding is not initialized when evaluation throws.
            assert.throws(function () {
                eval("class U { static { throw new Error('stop'); } }");
            }, Error);
            assert.areEqual("undefined", typeof U);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
