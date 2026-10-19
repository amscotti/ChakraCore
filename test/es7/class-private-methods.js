//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt in the project root for license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: private instance methods (`#m(){}`) and private accessors
// (`get #x(){}` / `set #x(v){}`). Static/private-static members, static
// blocks, computed names and decorators are intentionally out of this slice.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "private method calls dispatch with the receiver as this",
        body() {
            class C {
                #value = 40;
                #add(x) { return this.#value + x; }
                call(x) { return this.#add(x); }
                ref() { return this.#add; }
            }

            const c = new C();
            assert.areEqual(42, c.call(2));
            assert.areEqual(42, c.ref().call(c, 2));
            assert.areEqual("#add", c.ref().name);
            assert.areEqual(1, c.ref().length);
        }
    },
    {
        name: "the method function is created once per class evaluation and is shared",
        body() {
            class C {
                #m() { return 1; }
                ref() { return this.#m; }
            }

            const a = new C();
            const b = new C();
            assert.isTrue(a.ref() === b.ref());
            assert.isTrue(a.ref() === new C().ref());
        }
    },
    {
        name: "private methods are not reflected on instances, prototypes or constructors",
        body() {
            class C {
                #m() { return 1; }
                #field = 1;
                call() { return this.#m(); }
            }

            const c = new C();
            assert.areEqual(1, c.call());
            assert.areEqual(0, Object.getOwnPropertyNames(c).length);
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "#m"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C.prototype, "#m"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "#m"));
            assert.areEqual(-1, Object.getOwnPropertyNames(C.prototype).indexOf("#m"));
            assert.isFalse(Reflect.has(c, "#m"));
        }
    },
    {
        name: "wrong receivers fail the private-method brand check",
        body() {
            class C {
                #m() { return 1; }
                get self() { return this.#m; }
                callOn(o) { return o.#m(); }
            }

            const c = new C();
            const other = {};
            assert.areEqual(1, c.callOn(c));
            assert.throws(function () { c.callOn(other); }, TypeError);
            assert.throws(function () { c.callOn(Object.create(c)); }, TypeError);
            assert.throws(function () { c.callOn(new Proxy(c, {})); }, TypeError);
            assert.throws(function () { c.callOn(1); }, TypeError);

            // A method value can be called with any this; the brand check is on
            // the member access, not on the call itself.
            const m = c.self;
            assert.areEqual(1, m.call(other));
        }
    },
    {
        name: "private accessors get/set with the receiver as this",
        body() {
            const log = [];
            class C {
                #value = 1;
                get #x() { log.push("get"); return this.#value; }
                set #x(v) { log.push("set:" + v); this.#value = v; }
                read() { return this.#x; }
                write(v) { this.#x = v; }
                get self() { return this.#x; }
                set self(v) { this.#x = v; }
            }

            const c = new C();
            assert.areEqual(1, c.read());
            c.write(5);
            assert.areEqual(5, c.read());
            assert.areEqual("get,set:5,get", log.join(","));
            assert.areEqual(5, c.self);
            c.self = 9;
            assert.areEqual(9, c.read());
        }
    },
    {
        name: "missing accessor halves throw TypeError; accessor brand checks apply",
        body() {
            class GetterOnly {
                get #x() { return 1; }
                read() { return this.#x; }
                write(v) { this.#x = v; }
                readOn(o) { return o.#x; }
            }
            class SetterOnly {
                set #x(v) { this.value = v; }
                write(v) { this.#x = v; }
                read() { return this.#x; }
            }

            const g = new GetterOnly();
            assert.areEqual(1, g.read());
            assert.throws(function () { g.write(2); }, TypeError);
            assert.throws(function () { g.readOn({}); }, TypeError);
            assert.throws(function () { g.readOn(Object.create(g)); }, TypeError);

            const s = new SetterOnly();
            s.write(3);
            assert.areEqual(3, s.value);
            assert.throws(function () { s.read(); }, TypeError);
        }
    },
    {
        name: "accessor pairs share one private name and one binding",
        body() {
            class C {
                #x = 0;
                get #p() { return this.#x; }
                set #p(v) { this.#x = v + 1; }
                read() { return this.#p; }
                write(v) { this.#p = v; }
            }
            const c = new C();
            c.write(1);
            assert.areEqual(2, c.read());
        }
    },
    {
        name: "derived instances carry the base class private method brand",
        body() {
            class Base {
                #m() { return "base"; }
                #x = 7;
                baseCall() { return this.#m() + this.#x; }
            }
            class Derived extends Base {
                derivedCall() { return this.baseCall(); }
            }

            const d = new Derived();
            assert.areEqual("base7", d.derivedCall());
            assert.throws(function () { Base.prototype.baseCall.call({}); }, TypeError);
        }
    },
    {
        name: "private methods are installed before field initializers, even when declared later",
        body() {
            class C {
                a = this.#m();
                #m() { return 42; }
                #b = this.#m();
                get b() { return this.#b; }
            }
            const c = new C();
            assert.areEqual(42, c.a);
            assert.areEqual(42, c.b);
        }
    },
    {
        name: "#m in obj is an own-brand check for methods and accessors",
        body() {
            class C {
                #m() {}
                get #g() { return 1; }
                static hasMethod(o) { return #m in o; }
                static hasGetter(o) { return #g in o; }
            }

            const c = new C();
            assert.isTrue(C.hasMethod(c));
            assert.isTrue(C.hasGetter(c));
            assert.isFalse(C.hasMethod({}));
            assert.isFalse(C.hasGetter({}));
            assert.isFalse(C.hasMethod(Object.create(c)));
            assert.isFalse(C.hasMethod(new Proxy(c, {})));
            assert.throws(function () { C.hasMethod(1); }, TypeError);
            assert.throws(function () { C.hasGetter(null); }, TypeError);
        }
    },
    {
        name: "frozen/non-extensible receivers keep working after method installation",
        body() {
            class C {
                #m() { return 3; }
                get #x() { return this.v; }
                set #x(v) { this.v = v; }
                call() { return this.#m(); }
                read() { return this.#x; }
                write(v) { this.#x = v; }
            }
            const c = new C();
            Object.freeze(c);
            assert.areEqual(3, c.call());
            assert.throws(function () { c.write(4); }, TypeError);
            assert.areEqual(undefined, c.read());
        }
    },
    {
        name: "a non-extensible derived receiver installs method brands before fields throw",
        body() {
            class Base { constructor(o) { return o; } }
            class C extends Base {
                #m() { return 1; }
                #x = 1;
                call() { return this.#m(); }
            }
            const o = Object.preventExtensions({});
            assert.throws(function () { new C(o); }, TypeError);
            assert.areEqual(1, C.prototype.call.call(o));
        }
    },
    {
        name: "distinct class evaluations have distinct method/accessor brands",
        body() {
            function make() {
                return class {
                    #m() { return 1; }
                    get #g() { return 2; }
                    callOn(o) { return o.#m(); }
                    getOn(o) { return o.#g; }
                };
            }
            const A = make();
            const B = make();
            const a = new A();
            const b = new B();
            assert.areEqual(1, a.callOn(a));
            assert.areEqual(2, a.getOn(a));
            assert.throws(function () { a.callOn(b); }, TypeError);
            assert.throws(function () { a.getOn(b); }, TypeError);
            assert.isFalse(A.prototype.hasOwnProperty.call(a, "#m"));
        }
    },
    {
        name: "nested classes can shadow private methods and accessors",
        body() {
            class Outer {
                #m() { return "outer"; }
                get #g() { return "outer-get"; }
                outerCall() { return this.#m() + ":" + this.#g; }
                makeInner() {
                    return class Inner {
                        #m() { return "inner"; }
                        get #g() { return "inner-get"; }
                        call() { return this.#m() + ":" + this.#g; }
                    };
                }
            }
            const o = new Outer();
            const Inner = o.makeInner();
            const i = new Inner();
            assert.areEqual("outer:outer-get", o.outerCall());
            assert.areEqual("inner:inner-get", i.call());
            assert.throws(function () { o.outerCall.call(i); }, TypeError);
            assert.throws(function () { i.call.call(o); }, TypeError);
        }
    },
    {
        name: "super() inside an arrow clones the private method brand list",
        body() {
            class Base {
                constructor(v) { this.baseValue = v; }
            }
            class C extends Base {
                #m() { return this.baseValue + 1; }
                #x = this.#m();
                constructor(v) { (() => super(v))(); }
                call() { return this.#m(); }
                get x() { return this.#x; }
            }
            const c = new C(41);
            assert.areEqual(42, c.call());
            assert.areEqual(42, c.x);
        }
    },
    {
        name: "private method values are not writable",
        body() {
            class C {
                #m() {}
                assign() { this.#m = 0; }
            }
            const c = new C();
            assert.throws(function () { c.assign(); }, TypeError);
        }
    },
    {
        name: "private method/accessor early errors",
        body() {
            assert.throws(function () { eval("class C { #m(){} #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #m(){} #m; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #m; #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #m(){} get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #m(){} get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { set #m(v){} set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #constructor(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #constructor(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { m() { this.#missing(); } }"); }, SyntaxError);
            assert.throws(function () { eval("this.#missing"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #m(){} }"); }, SyntaxError);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
