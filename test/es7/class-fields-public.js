//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: public instance class fields with simple identifier names, computed
// names and optional initializers (doc/c2-s1-public-fields.md,
// doc/c2-s1-computed-instance-super.md). Private/static elements and
// decorators are intentionally not part of this slice.

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
        name: "field own names do not depend on another property reference",
        body() {
            class C {
                uniqueName = 42;
                duplicate = 1;
                duplicate = 2;
                "stringName" = 3;
                0 = 4;
                42;
            }
            const c = new C();
            const names = Object.getOwnPropertyNames(c);
            assert.areEqual("0,42,uniqueName,duplicate,stringName", names.join(","));
            assert.areEqual(4, c[0]);
            assert.areEqual(undefined, c[42]);
            assert.areEqual(42, c["uniqueName"]);
            assert.areEqual(2, c["duplicate"]);
            assert.areEqual(3, c["stringName"]);
        }
    },
    {
        name: "eval and new Function classes keep field initialization",
        body() {
            const evalInstance = eval("(new (class { evalOnly = 10 }))");
            assert.areEqual("evalOnly", Object.getOwnPropertyNames(evalInstance).join(","));
            assert.areEqual(10, evalInstance["evalOnly"]);

            const FunctionCtor = new Function("return new (class { functionOnly = 11 })");
            const fnInstance = FunctionCtor();
            assert.areEqual("functionOnly", Object.getOwnPropertyNames(fnInstance).join(","));
            assert.areEqual(11, fnInstance["functionOnly"]);
        }
    },
    {
        name: "base fields survive captured this, dead eval and eval in constructor arrows",
        body() {
            class Captured {
                x = 1;
                constructor() { const f = () => this; this.f = f; }
            }
            const captured = new Captured();
            assert.areEqual(1, Object.getOwnPropertyDescriptor(captured, "x").value);
            assert.areEqual(captured, captured.f());

            class DeadEval {
                x = 2;
                constructor() { eval("1"); }
            }
            assert.areEqual(2, Object.getOwnPropertyDescriptor(new DeadEval(), "x").value);

            class ArrowEval {
                x = 3;
                constructor() { const f = () => eval("this"); this.f = f; }
            }
            const arrowEval = new ArrowEval();
            assert.areEqual(3, Object.getOwnPropertyDescriptor(arrowEval, "x").value);
            assert.areEqual(arrowEval, arrowEval.f());

            class Base {}
            class DerivedEmpty extends Base {
                x;
                constructor() { const f = () => this; super(); this.f = f; }
            }
            const derivedEmpty = new DerivedEmpty();
            assert.areEqual("x,f", Object.getOwnPropertyNames(derivedEmpty).join(","));
            assert.areEqual(undefined, derivedEmpty.x);
            assert.areEqual(derivedEmpty, derivedEmpty.f());
        }
    },
    {
        name: "class field definition throws when CreateDataProperty fails",
        body() {
            class Frozen { a = Object.freeze(this); b = 2; }
            assert.throws(function () { new Frozen(); }, TypeError, "frozen instance");

            class NonExtensible { a = Object.preventExtensions(this); b = 2; }
            assert.throws(function () { new NonExtensible(); }, TypeError, "non-extensible instance");

            class NonConfigurable {
                a = Object.defineProperty(this, "b", { value: 1, configurable: false });
                b = 2;
            }
            assert.throws(function () { new NonConfigurable(); }, TypeError, "non-configurable existing property");

            class Base {
                constructor() {
                    return new Proxy(this, { defineProperty() { return false; } });
                }
            }
            class Derived extends Base { x = 1; }
            assert.throws(function () { new Derived(); }, TypeError, "proxy define trap");
        }
    },
    {
        name: "arguments static semantics: ordinary functions are boundaries; arrows/shorthand/eval are not",
        body() {
            class Ordinary {
                x = function () { return arguments.length; };
            }
            assert.areEqual(2, new Ordinary().x(1, 2));

            assert.throws(function () { eval("class C { x = ({arguments}); }"); }, SyntaxError, "shorthand");
            assert.throws(function () { eval("class C { x = ({y: arguments}); }"); }, SyntaxError, "destructuring value");
            assert.throws(function () { eval("class C { x = ({arguments} = {arguments: 1}); }"); }, SyntaxError, "destructuring shorthand");
            assert.throws(function () { eval("class C { x = () => arguments; }"); }, SyntaxError, "arrow");
            assert.doesNotThrow(function () { eval("class C { x = { m() { return arguments; } }; }"); }, "method should be allowed");
            assert.throws(function () {
                const C = eval("(class { x = eval('arguments'); })");
                new C();
            }, SyntaxError, "direct eval is a runtime SyntaxError");
        }
    },
    {
        name: "direct eval in initializers rejects arguments but permits valid code",
        body() {
            class Good { x = eval("({arguments: 7}).arguments"); }
            assert.areEqual(7, new Good().x);

            let executed = false;
            class Bad { x = eval("executed = true; arguments;"); }
            assert.throws(function () { new Bad(); }, SyntaxError, "direct eval arguments");
            assert.isFalse(executed);

            class BadArrow { x = eval("executed = true; () => arguments;"); }
            assert.throws(function () { new BadArrow(); }, SyntaxError, "direct eval arguments in arrow");
            assert.isFalse(executed);
        }
    },
    {
        name: "async modifier spellings are not field definitions",
        body() {
            assert.throws(function () { eval("class C { async x = 1 }"); }, SyntaxError, "async x =");
            assert.throws(function () { eval("class C { async x; }"); }, SyntaxError, "async x;");
            assert.throws(function () { eval("class C { async get; }"); }, SyntaxError, "async get;");
            assert.throws(function () { eval("class C { async static = 1; }"); }, SyntaxError, "async static =");

            class AsyncName { async = 1; }
            assert.areEqual("async", Object.getOwnPropertyNames(new AsyncName()).join(","));

            const C = eval("(class { async\nam() { return 3; } })");
            const c = new C();
            assert.areEqual(undefined, c.async);
            assert.areEqual(3, c.am());
        }
    },
    {
        name: "get/set followed by a generator via ASI is a field plus method",
        body() {
            const C = eval("(class { get\n*gen() { yield 1; } set\n*gen2() { yield 2; } })");
            const c = new C();
            assert.areEqual("get,set", Object.getOwnPropertyNames(c).join(","));
            assert.areEqual(1, c.gen().next().value);
            assert.areEqual(2, c.gen2().next().value);
        }
    },
    {
        name: "anonymous initializer functions and classes receive the field name",
        body() {
            class C {
                x = function () {};
                y = () => {};
                z = class {};
            }
            const c = new C();
            assert.areEqual("x,y,z", [c.x.name, c.y.name, c.z.name].join(","));
        }
    },
    {
        name: "super() in a constructor arrow initializes fields only when called",
        body() {
            class Base { constructor() { this.base = 1; } }

            class Uncalled extends Base {
                x = 2;
                constructor() { const f = () => super(); super(); }
            }
            const uncalled = new Uncalled();
            assert.areEqual(2, Object.getOwnPropertyDescriptor(uncalled, "x").value);
            assert.areEqual(1, uncalled.base);

            class Called extends Base {
                x = 3;
                constructor() { (() => super())(); this.y = this.x; }
            }
            const called = new Called();
            assert.areEqual(3, Object.getOwnPropertyDescriptor(called, "x").value);
            assert.areEqual(3, called.y);
            assert.areEqual(1, called.base);

            // super() through eval cannot run field initialization in this
            // slice; it must be a clear SyntaxError, not silently skipped.
            assert.throws(function () {
                const C = eval("(class extends Base { x = 4; constructor() { eval('super()'); } })");
                new C();
            }, SyntaxError, "eval super");
            assert.throws(function () {
                const C = eval("(class extends Base { x = 4; constructor() { (() => eval('super()'))(); } })");
                new C();
            }, SyntaxError, "arrow eval super");
            assert.throws(function () {
                const C = eval("(class extends Base { x = 4; constructor(a = eval('super()')) { } })");
                new C();
            }, SyntaxError, "parameter-default eval super");
        }
    },
    {
        name: "nested classes in eval may call their own super()",
        body() {
            class Base { constructor() { this.base = 1; } }
            class Outer extends Base {
                x = 1;
                constructor() {
                    eval("class E extends Base { constructor() { super(); this.e = 1; } }");
                    eval("var C = class extends Base { constructor() { super(); this.c = 1; } }");
                    eval("class P extends Base { constructor(a = super()) { } }");
                    eval("new (class E2 extends Base { y = 5; constructor() { super(); } })()");
                    super();
                }
            }
            const outer = new Outer();
            assert.areEqual(1, Object.getOwnPropertyDescriptor(outer, "x").value);
            assert.areEqual(1, outer.base);
        }
    },
    {
        name: "super-bearing eval is deterministic across caller contexts",
        body() {
            class Base { constructor() { this.base = 1; } }
            class Ctor extends Base { constructor() { eval("super()"); this.c = 1; } }
            class Method { m() { eval("super()"); } }
            class Fields extends Base { x = 1; constructor() { eval("super()"); } }

            const first = new Ctor();
            assert.areEqual(1, first.base);
            assert.areEqual(1, first.c);
            assert.throws(function () { new Method().m(); }, SyntaxError, "method eval super");
            // This slice deliberately rejects eval super() when the caller
            // carries instance fields rather than initializing them in eval.
            assert.throws(function () { new Fields(); }, SyntaxError, "field-carrying constructor eval super");

            const again = new Ctor();
            assert.areEqual(1, again.base);
            assert.areEqual(1, again.c);
            assert.throws(function () { new Method().m(); }, SyntaxError, "method eval super repeat");
            assert.throws(function () { new Fields(); }, SyntaxError, "field-carrying constructor repeat");
        }
    },
    {
        name: "eval arguments labels are rejected",
        body() {
            assert.throws(function () {
                const C = eval("(class { x = eval('arguments: 1;') })");
                new C();
            }, SyntaxError, "label");
            assert.throws(function () {
                const C = eval("(class { x = eval('arguments: while (true) { break arguments; }') })");
                new C();
            }, SyntaxError, "labelled break");
            assert.throws(function () {
                const C = eval("(class { x = eval('arguments: for (;;) { continue arguments; }') })");
                new C();
            }, SyntaxError, "labelled continue");

            // An ordinary function boundary ends the restriction.
            class C { x = eval("(function () { arguments: for (;;) { break arguments; } })"); }
            assert.isTrue(typeof new C().x === "function");
        }
    },
    {
        name: "await and yield are invalid in field initializers in every surrounding shape",
        body() {
            assert.throws(function () { eval("async function f(){ class C { x = await 1 } }"); }, SyntaxError, "async function class declaration");
            assert.throws(function () { eval("function* g(){ class C { x = yield 1 } }"); }, SyntaxError, "generator class declaration");
            assert.throws(function () { eval("async function f(){ var C = class { x = await 1 }; }"); }, SyntaxError, "async class expression");
            assert.throws(function () { eval("function* g(){ var o = { *m(){ class C { x = yield 1 } } }; }"); }, SyntaxError, "generator method");
            assert.throws(function () { eval("var f = async ()=>{ class C { x = await 1 }; };"); }, SyntaxError, "async arrow");

            // Valid await/yield in genuinely nested async/generator functions.
            class C {
                a = async () => await 1;
                b = function* () { yield 2; };
                c = async function () { return await 3; };
            }
            const c = new C();
            assert.areEqual(2, c.b().next().value);
        }
    },
    {
        name: "eval arguments detection is parser-aware",
        body() {
            // Valid: ordinary function/method boundaries, strings, regexes,
            // comments and property names.
            class Valid {
                a = eval("(function () { return arguments.length; })");
                b = eval("/arguments/");
                c = eval("({ arguments() { return 5; } })");
                d = eval("\"arguments\"");
                e = eval("/* arguments */ 1");
                f = eval("({ arguments: 1 }).arguments");
            }
            const v = new Valid();
            assert.areEqual(2, v.a(1, 2));
            assert.areEqual("arguments", v.b.source);
            assert.areEqual(5, v.c.arguments());
            assert.areEqual("arguments", v.d);
            assert.areEqual(1, v.e);
            assert.areEqual(1, v.f);

            assert.throws(function () { new (eval("(class Bad { x = eval('`${arguments}`'); })"))(); }, SyntaxError, "template");
            assert.throws(function () { new (eval("(class Bad { x = eval('arg\\u0075ments'); })"))(); }, SyntaxError, "escaped identifier");
            assert.throws(function () { new (eval("(class Bad { x = eval(\"eval('arguments')\"); })"))(); }, SyntaxError, "nested eval");
            assert.throws(function () { new (eval("(class Bad { x = eval('true ? arguments : 0'); })"))(); }, SyntaxError, "conditional reference");
        }
    },
    {
        name: "large deferred functions reject await/yield field initializers at parse time",
        body() {
            const prelude = "var a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9,j=10,k=11,l=12,m=13,n=14,o=15,p=16;";
            const awaitSource = "function big(){" + prelude + " async function f(){ class C { x = await 1 } } return [a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p]; } big();";
            const yieldSource = "function big(){" + prelude + " function* g(){ class C { x = yield 1 } } return [a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p]; } big();";
            assert.throws(function () { eval(awaitSource); }, SyntaxError, "deferred await");
            assert.throws(function () { eval(yieldSource); }, SyntaxError, "deferred yield");
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
    },
    {
        name: "computed instance field names evaluate once in textual order",
        body() {
            const log = [];
            function key(name) { log.push(name); return name; }
            class C {
                [key("m1")]() { return 1; }
                a = (log.push("init:a"), 1);
                [key("f1")] = (log.push("init:f1"), 2);
                [key("m2")]() { return 3; }
                b = (log.push("init:b"), 4);
            }
            // Keys evaluate at class definition time, interleaved with
            // methods in textual order; initializers do not run yet.
            assert.areEqual("m1,f1,m2", log.join(","));

            log.length = 0;
            const first = new C();
            assert.areEqual("init:a,init:f1,init:b", log.join(","));
            assert.areEqual(1, first.a);
            assert.areEqual(2, first.f1);
            assert.areEqual(4, first.b);
            assert.areEqual("a,f1,b", Object.keys(first).join(","));

            // Keys are not re-evaluated per construction.
            log.length = 0;
            const second = new C();
            assert.areEqual("init:a,init:f1,init:b", log.join(","));
            assert.areEqual(2, second.f1);
        }
    },
    {
        name: "computed instance field keys: scope, conversions, duplicates and special names",
        body() {
            let count = 0;
            const keyObj = { toString() { count++; return "k"; } };
            class C {
                [keyObj] = 1;
                [keyObj] = 2;
            }
            // ToPropertyKey runs once per element at class definition time.
            assert.areEqual(2, count);
            const c = new C();
            assert.areEqual(1, Object.keys(c).length);
            assert.areEqual(2, c.k);
            assert.areEqual(2, count);

            // Outer bindings are visible; the class name is in its TDZ.
            let v = "w";
            class D {
                [v] = 1;
            }
            assert.areEqual(1, new D().w);
            assert.throws(function () { eval("class Bad { [Bad] = 1; }"); }, ReferenceError, "class name TDZ in key");

            // Symbol and numeric keys.
            const s = Symbol("s");
            class F {
                [s] = 1;
                [42] = 2;
            }
            const f = new F();
            assert.areEqual(1, f[s]);
            assert.areEqual(2, f["42"]);

            // Duplicate computed keys: every initializer runs, last wins.
            let runs = 0;
            class G {
                ["d"] = (runs++, 1);
                ["d"] = (runs++, 2);
            }
            assert.areEqual(2, new G().d);
            assert.areEqual(2, runs);

            // "constructor", "prototype" and "__proto__" define ordinary own
            // properties on the instance.
            class H {
                ["constructor"] = 1;
                ["prototype"] = 2;
                ["__proto__"] = 3;
                ["bare"];
            }
            const h = new H();
            assert.areEqual(1, h.constructor);
            assert.areEqual(2, h.prototype);
            assert.areEqual(3, h["__proto__"]);
            assert.isTrue(Object.prototype.hasOwnProperty.call(h, "__proto__"));
            assert.areEqual(H.prototype, Object.getPrototypeOf(h));
            assert.isTrue(Object.prototype.hasOwnProperty.call(h, "bare"));
            assert.areEqual(undefined, h.bare);

            // Anonymous function and class values take the computed key name.
            class N {
                ["nf"] = function () { };
                ["nc"] = class { };
                ["named"] = function explicit() { };
            }
            const n = new N();
            assert.areEqual("nf", n.nf.name);
            assert.areEqual("nc", n.nc.name);
            assert.areEqual("explicit", n.named.name);

            // A throwing key suppresses every later definition.
            assert.throws(function () {
                eval("class E { [(() => { throw new Error('boom'); })()] = 1; static s = 2; }");
            }, Error, "key throw");
            assert.throws(function () {
                eval("class E2 { [(() => { throw new Error('kboom'); })()] = 1; x = 1; } new E2();");
            }, Error, "key throw suppresses instance fields");
        }
    },
    {
        name: "super reads in instance field initializers",
        body() {
            class Base { }
            Base.prototype.pd = 111;
            Base.prototype.who = function () { return this.tag + "!"; };
            Object.defineProperty(Base.prototype, "g", { get() { return 42; }, configurable: true });
            class Derived extends Base {
                tag = "T";
                x = super.pd;
                y = super.g;
                z = super.who();
                w = (() => super.pd)();
                v = super["pd"];
                missing = super.missing;
                t = this.tag;
            }
            const d = new Derived();
            assert.areEqual(111, d.x);
            assert.areEqual(42, d.y);
            assert.areEqual("T!", d.z);
            assert.areEqual(111, d.w);
            assert.areEqual(111, d.v);
            assert.areEqual(undefined, d.missing);
            assert.areEqual("T", d.t);
            assert.areEqual("tag,x,y,z,w,v,missing,t", Object.keys(d).join(","));

            // Base classes read through Object.prototype.
            class Plain {
                a = super.toString === Object.prototype.toString;
                b = super.hasOwnProperty === Object.prototype.hasOwnProperty;
            }
            const p = new Plain();
            assert.isTrue(p.a);
            assert.isTrue(p.b);

            // super() in an instance field initializer is an early error, as
            // is super in a nested non-arrow function.
            assert.throws(function () { new Function("class D extends Object { x = super(); }"); }, SyntaxError, "super call");
            assert.throws(function () { new Function("class D { x = super(); }"); }, SyntaxError, "super call in base");
            assert.throws(function () { new Function("class D extends Object { x = (function () { return super.foo; })(); }"); }, SyntaxError, "nested function super");
        }
    },
    {
        name: "super assignments and calls in instance field initializers",
        body() {
            class Base {
                set s(v) { this.sv = v * 2; }
            }
            Base.prototype.pd = 111;
            Base.prototype.m = function () { return "m:" + this.tag; };
            class Derived extends Base {
                tag = "T";
                a = (super.s = 21, this.sv);
                b = (super.pd = 9, this.pd);
                c = (super.pd2 = 7, this.pd2);
                r = super.m();
            }
            const d = new Derived();
            // Setters run with the instance as receiver.
            assert.areEqual(42, d.a);
            assert.areEqual(42, d.sv);
            // Plain assignment shadows the prototype value on the instance.
            assert.areEqual(9, d.b);
            assert.areEqual(111, Base.prototype.pd);
            assert.areEqual(7, d.c);
            // Calls observe the instance as receiver.
            assert.areEqual("m:T", d.r);
            // Compound assignment reads through the base and writes the instance.
            class D2 extends Base {
                e = (super.pd += 1, this.pd);
            }
            const d2 = new D2();
            assert.areEqual(112, d2.e);
            assert.areEqual(111, Base.prototype.pd);

            // Base-class assignment creates an own property on the instance.
            class Plain {
                f = (super.zzz = 5, this.zzz);
            }
            assert.areEqual(5, new Plain().f);

            // extends null cannot construct, so fields never run.
            assert.throws(function () { new (class N extends null { q = 5; })(); }, TypeError, "extends null construction");
        }
    },
    {
        name: "computed fields and instance super in deferred and indirect shapes",
        body() {
            function make(tag) {
                return class extends Object {
                    t = tag;
                    [tag] = super.toString === Object.prototype.toString;
                    u = super.hasOwnProperty === Object.prototype.hasOwnProperty;
                };
            }
            const inst = new (make("dyn"))();
            assert.areEqual("dyn", inst.t);
            assert.isTrue(inst.dyn);
            assert.isTrue(inst.u);

            const EvalC = eval("(class { ['e'] = 41; })");
            assert.areEqual(41, new EvalC().e);

            const FunctionC = new Function("return class { static se = 1; ['ie'] = 2; }")();
            assert.areEqual(2, new FunctionC().ie);
            assert.areEqual(1, FunctionC.se);

            // Classes defined inside field initializers keep working keys.
            class Outer {
                inner = (new (class { ["k"] = 3; })()).k;
            }
            assert.areEqual(3, new Outer().inner);
        }
    },
    {
        name: "super in direct eval: eval top level and arrows inherit, nested plain functions are early errors",
        body() {
            class B { y() { return 11; } get g() { return 12; } }
            class D extends B {
                m() { return eval("super.y()"); }
                a() { return eval("(() => super.g)()"); }
                n() { return eval("(function(){ return super.y(); })()"); }
            }
            const d = new D();
            assert.areEqual(11, d.m());
            assert.areEqual(12, d.a());
            assert.throws(() => d.n(), SyntaxError, "nested plain function in eval has no home object");

            // Indirect eval never inherits a super binding.
            const indirect = eval;
            assert.throws(() => indirect("super.y"), SyntaxError, "indirect eval super");

            // Classes defined by eval keep their own super rules.
            const E = eval("(class extends B { q() { return super.y(); } })");
            assert.areEqual(11, new E().q());
            assert.throws(() => eval("(function(){ class C { static x = function(){ return super.y; }; } return C.x(); })()"),
                SyntaxError, "nested plain function in eval-defined static initializer");
            assert.throws(() => eval("(function(){ class C { x = function(){ return super.y; }; } return new C().x(); })()"),
                SyntaxError, "nested plain function in eval-defined instance initializer");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
