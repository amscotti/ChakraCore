//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt in the project root for license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: private instance methods (`#m(){}`) and private accessors
// (`get #x(){}` / `set #x(v){}`), private static methods and accessors
// (`static #m(){}`, `static get #x(){}` / `static set #x(v){}`), and
// async/generator private methods (`async #m(){}`, `*#m(){}`,
// `async *#m(){}` and their static forms; doc/c2-s1-private-async-gen.md).
// Computed private names and decorators remain out of scope. Static blocks
// are covered in class-static-fields.js (doc/c2-s1-static-blocks.md).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Async completion settles after the test body returns, so promise-valued
// assertions run through the framework's async machinery: the summary waits
// for the loaded snippet, and a mismatch fails the test instead of passing
// silently. There is exactly one such snippet per file: the LoadScript
// wrapper publishes a single global _asyncExit, so concurrent snippets would
// overwrite each other's exit and never complete.
function assertAsyncCompletion(source) {
    testRunner.LoadScript(source, undefined, false, true);
}

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
                assignLiteral() { this.#m = 8; }
                assignString() { this.#m = "x"; }
            }
            const c = new C();
            assert.throws(function () { c.assign(); }, TypeError);
            assert.throws(function () { c.assignLiteral(); }, TypeError);
            assert.throws(function () { c.assignString(); }, TypeError);
        }
    },
    {
        name: "literal, compound, logical and destructuring private stores use writable destinations",
        body() {
            class Fields {
                #n = 0;
                #s = "";
                #b = true;
                #z = 1;
                #u = 1;

                setLiterals() {
                    this.#n = 8;
                    this.#s = "x";
                    this.#b = false;
                    this.#z = null;
                    this.#u = undefined;
                }
                getLiterals() {
                    return [this.#n, this.#s, this.#b, this.#z, this.#u];
                }
                compound() {
                    this.#n = 1;
                    this.#n += 2;
                    this.#n ||= 9;
                    this.#n &&= 7;
                    this.#n ??= 5;
                    return this.#n;
                }
                destructure() {
                    [this.#n] = [11];
                    ({ x: this.#s } = { x: "y" });
                    return [this.#n, this.#s];
                }
            }

            const f = new Fields();
            f.setLiterals();
            const literals = f.getLiterals();
            assert.areEqual(8, literals[0]);
            assert.areEqual("x", literals[1]);
            assert.areEqual(false, literals[2]);
            assert.areEqual(null, literals[3]);
            assert.areEqual(undefined, literals[4]);
            assert.areEqual(7, f.compound());
            const destructured = f.destructure();
            assert.areEqual(11, destructured[0]);
            assert.areEqual("y", destructured[1]);

            class Accessors {
                v = 0;
                set #x(value) { this.v = value; }
                get #x() { return this.v; }

                setNumber() { this.#x = 8; }
                setString() { this.#x = "x"; }
                setBool() { this.#x = false; }
                setNull() { this.#x = null; }
                setUndefined() { this.#x = undefined; }
                read() { return this.#x; }
                compound() {
                    this.#x = 1;
                    this.#x += 2;
                    this.#x ||= 9;
                    this.#x &&= 7;
                    this.#x ??= 5;
                    return this.#x;
                }
                destructure() {
                    [this.#x] = [11];
                    ({ x: this.#x } = { x: 12 });
                    return this.#x;
                }
            }

            const a = new Accessors();
            a.setNumber();
            assert.areEqual(8, a.read());
            a.setString();
            assert.areEqual("x", a.read());
            a.setBool();
            assert.areEqual(false, a.read());
            a.setNull();
            assert.areEqual(null, a.read());
            a.setUndefined();
            assert.areEqual(undefined, a.read());
            assert.areEqual(7, a.compound());
            assert.areEqual(12, a.destructure());
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
            assert.throws(function () { eval("class C { async get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async *get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async *set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async get #m(){} set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #m(){} async set #m(v){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #m(){} set #m(v){} get #m(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #m(){} set #m(v){} set #m(v){} }"); }, SyntaxError);
            assert.doesNotThrow(function () { eval("class C { get #m(){} set #m(v){} }"); });
            assert.throws(function () { eval("class C { #constructor(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #constructor(){} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { m() { this.#missing(); } }"); }, SyntaxError);
            assert.throws(function () { eval("this.#missing"); }, SyntaxError);
            assert.doesNotThrow(function () { eval("class C { static #m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { static get #m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { static set #m(v){} }"); });
            assert.doesNotThrow(function () { eval("class C { async #m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { *#m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { async *#m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { static async #m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { static *#m(){} }"); });
            assert.doesNotThrow(function () { eval("class C { static async *#m(){} }"); });
        }
    },
    {
        name: "static private methods dispatch on the constructor",
        body() {
            class C {
                static #m() { return 42; }
                static call() { return this.#m(); }
                static ref() { return this.#m; }
            }

            assert.areEqual(42, C.call());
            assert.areEqual("#m", C.ref().name);
            // The brand check happens at `this.#m`, not at the call itself,
            // so an extracted function remains callable with any receiver.
            assert.areEqual(42, C.ref().call({}, 0));
        }
    },
    {
        name: "the static private brand belongs to the defining constructor",
        body() {
            class C {
                static #m() { return 1; }
                static call() { return this.#m(); }
            }
            class Other {}
            class D extends C {}

            assert.areEqual(1, C.call());
            assert.throws(function () { C.call.call(new C()); }, TypeError);
            assert.throws(function () { C.call.call(C.prototype); }, TypeError);
            assert.throws(function () { C.call.call(Other); }, TypeError);
            assert.throws(function () { D.call(); }, TypeError);
            assert.throws(function () { C.call.call(D); }, TypeError);

            // A subclass that redeclares the name gets a fresh brand.
            class R2 extends C {
                static #m() { return 2; }
                static call() { return this.#m(); }
            }
            assert.areEqual(2, R2.call());
            assert.areEqual(1, C.call());
        }
    },
    {
        name: "static private accessors share one name and brand",
        body() {
            class C {
                static get #v() { return this._v; }
                static set #v(x) { this._v = x * 2; }
                static getV() { return this.#v; }
                static setV(x) { this.#v = x; }
                static has(o) { return #v in o; }
            }

            C.setV(21);
            assert.areEqual(42, C.getV());
            assert.isTrue(C.has(C));
            assert.isFalse(C.has({}));
            assert.isFalse(C.has(new C()));
            assert.isFalse(C.has(C.prototype));
            assert.throws(function () { C.has(1); }, TypeError);

            assert.throws(function () { eval("class G { static get #v() { return 1; } static w(v) { this.#v = v; } } G.w(1);"); }, TypeError);
            assert.throws(function () { eval("class S { static set #v(x) {} static r() { return this.#v; } } S.r();"); }, TypeError);
            assert.throws(function () { eval("class W { static #m() {} static w(v) { this.#m = v; } } W.w(1);"); }, TypeError);
            assert.doesNotThrow(function () { eval("class P { static get #v() { return 1; } static set #v(x) {} }"); });
        }
    },
    {
        name: "static private methods are hidden from reflection and survive freeze",
        body() {
            class C {
                static #m() { return 1; }
                static call() { return this.#m(); }
                static has(o) { return #m in o; }
            }

            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "#m"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C.prototype, "#m"));
            assert.areEqual(-1, Object.getOwnPropertyNames(C).indexOf("#m"));
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(C, "#m"));
            assert.isFalse(C.has(new C()));

            Object.freeze(C);
            assert.areEqual(1, C.call());
            assert.isTrue(C.has(C));
        }
    },
    {
        name: "static and instance private names are isolated per evaluation",
        body() {
            class C {
                static #m() { return 1; }
                im() { return this.#m; }
            }
            // Instances are never branded with static names.
            assert.throws(function () { new C().im(); }, TypeError);

            // Instance code reaches static names lexically through the class.
            class N {
                static #m() { return 5; }
                im() { return N.#m(); }
            }
            assert.areEqual(5, new N().im());

            // Each class evaluation creates a fresh static brand.
            var C1 = eval("class C1 { static #m() { return 1; } static call() { return this.#m(); } }");
            var C2 = eval("class C2 { static #m() { return 2; } static call() { return this.#m(); } }");
            assert.areEqual(1, C1.call());
            assert.areEqual(2, C2.call());
            assert.throws(function () { C1.call.call(C2); }, TypeError);

            var K = class {
                static #m() { return 9; }
                static call() { return this.#m(); }
            };
            assert.areEqual(9, K.call());
        }
    },
    {
        name: "static private methods support super and #prototype",
        body() {
            class B { static who() { return "base"; } }
            class E extends B {
                static #m() { return super.who(); }
                static call() { return this.#m(); }
            }
            assert.areEqual("base", E.call());

            // A private identifier is never a PropName, so the static
            // "prototype" early error does not apply (matching instances).
            var P = eval("class P { static #prototype() { return 1; } static call() { return this.#prototype(); } }");
            assert.areEqual(1, P.call());
        }
    },
    {
        name: "static private early errors",
        body() {
            assert.throws(function () { eval("class C { static #m() {} static #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m() {} #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #m() {} static #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m() {} static #m = 1; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m = 1; static #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m() {} #m = 1; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #x() {} static get #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static set #x(v) {} static set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #x() {} set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #x() {} static set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #x() {} static #x = 1; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #x() {} #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #constructor() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static get #constructor() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #m() {} static r() { return this.#missing; } }"); }, SyntaxError);
            // ASI still splits a field from a following static member.
            assert.doesNotThrow(function () { eval("class C { static #x\nstatic #m() { return 1; } }"); });
        }
    },
    {
        name: "async private methods have the correct kind, name and length",
        body() {
            class C {
                #v = 40;
                async #m(a, b) { await 1; return this.#v + a + b; }
                call(a, b) { return this.#m(a, b); }
                ref() { return this.#m; }
                static has(o) { return #m in o; }
            }

            const c = new C();
            const fn = c.ref();
            assert.areEqual("#m", fn.name);
            assert.areEqual(2, fn.length);
            assert.areEqual("AsyncFunction", fn.constructor.name);
            assert.isTrue(c.call(0, 0) instanceof Promise);
            assert.isTrue(C.has(c));
            assert.isFalse(C.has({}));
            assert.isFalse(C.has(Object.create(c)));
            assert.throws(function () { C.has(1); }, TypeError);
            // Completion values for this and every other async case below
            // are asserted by the single "async completions" case at the end
            // of this file.
        }
    },
    {
        name: "generator private methods support yield, yield*, return() and throw()",
        body() {
            class C {
                *#g(x, y) { yield x; yield* [x + 1, x + 2]; return x + 3; }
                it(x) { return this.#g(x); }
                ref() { return this.#g; }
                callOn(o, x) { return o.#g(x); }
                static has(o) { return #g in o; }
            }

            const c = new C();
            assert.areEqual("#g", c.ref().name);
            assert.areEqual(2, c.ref().length);
            assert.areEqual("GeneratorFunction", c.ref().constructor.name);
            const first = c.it(1);
            assert.areEqual(1, first.next().value);
            assert.areEqual(2, first.next().value);
            assert.areEqual(3, first.next().value);
            const last = first.next();
            assert.areEqual(4, last.value);
            assert.areEqual(true, last.done);
            assert.areEqual(9, c.it(0).return(9).value);
            assert.areEqual(true, c.it(0).return(9).done);
            try {
                c.it(0).throw(new Error("boom"));
                assert.fail("throw() through a private generator should propagate");
            } catch (e) {
                assert.areEqual("boom", e.message);
            }
            assert.throws(function () { c.callOn({}, 1).next(); }, TypeError);
            assert.throws(function () { c.callOn(Object.create(c), 1).next(); }, TypeError);
            assert.isTrue(C.has(c));
            assert.isFalse(C.has({}));
        }
    },
    {
        name: "async generator private methods support await, yield and for-await",
        body() {
            class C {
                #n = 3;
                async *#a() { for (let i = 0; i < this.#n; i++) { await i; yield i * 2; } }
                it() { return this.#a(); }
                ref() { return this.#a; }
            }

            const c = new C();
            assert.areEqual("#a", c.ref().name);
            assert.areEqual(0, c.ref().length);
            assert.areEqual("AsyncGeneratorFunction", c.ref().constructor.name);
            // The first next() returns a promise synchronously.
            assert.isTrue(c.it().next() instanceof Promise);
        }
    },
    {
        name: "static async/generator private methods dispatch on the constructor",
        body() {
            class C {
                static #v = 40;
                static async #m(x) { await 1; return this.#v + x; }
                static *#g(x) { yield x; yield x + 1; }
                static async *#a(x) { await 1; yield x * 2; }
                static call(x) { return this.#m(x); }
                static git(x) { return [...this.#g(x)]; }
                static ref() { return this.#m; }
                static has(o) { return #m in o; }
            }

            assert.areEqual("#m", C.ref().name);
            assert.areEqual(1, C.ref().length);
            assert.areEqual("AsyncFunction", C.ref().constructor.name);
            assert.isTrue(C.call(0) instanceof Promise);
            assert.areEqual("1,2", C.git(1).join(","));
            assert.isTrue(C.has(C));
            assert.isFalse(C.has({}));
            assert.isFalse(C.has(new C()));
            assert.throws(function () { C.call.call({}); }, TypeError);
        }
    },
    {
        name: "private async/generator methods work in derived classes with super",
        body() {
            class Base {
                constructor() { this.base = 100; }
                val() { return this.base; }
            }
            class C extends Base {
                #x = 1;
                async #m(y) { await 1; return this.val() + this.#x + y; }
                *#g(y) { yield super.val() + y; }
                call(y) { return this.#m(y); }
                git(y) { return [...this.#g(y)]; }
            }

            const c = new C();
            assert.isTrue(c.call(0) instanceof Promise);
            assert.areEqual("100,101", [100, c.git(1)[0]].join(","));
        }
    },
    {
        name: "private async/generator brands survive prototype manipulation and freeze",
        body() {
            class C {
                async *#m() { await 1; yield 7; }
                call() { return this.#m(); }
                ref() { return this.#m; }
            }

            const c = new C();
            const fn = c.ref();
            Object.setPrototypeOf(C.prototype, null);
            Object.freeze(c);
            assert.areEqual("#m", fn.name);
            assert.isTrue(c.call().next() instanceof Promise);
        }
    },
    {
        name: "private async/generator early errors and line-terminator rules",
        body() {
            // Async or generator accessors stay early errors.
            assert.throws(function () { eval("class C { async get #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *get #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async *get #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async *set #x(v) {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #x() {} async set #x(v) {} }"); }, SyntaxError);
            // The async modifier comes before static, never after it.
            assert.throws(function () { eval("class C { async static #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async static #m; }"); }, SyntaxError);
            // Duplicate rules are unchanged across kinds.
            assert.throws(function () { eval("class C { async #m() {} #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *#m() {} async *#m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async #m() {} async #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static *#m() {} static #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { get #x() {} async #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async #m() {} static async #m() {} }"); }, SyntaxError);
            // An async method needs a parameter list; a generator needs a body.
            assert.throws(function () { eval("class C { async #m = 1 }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async #x; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *#m = 1 }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async *#m = 1 }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x *gen() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { async #constructor() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { *#constructor() {} }"); }, SyntaxError);
            // A newline after async keeps `async` an ordinary member: the
            // class below is a field named async plus a sync private method.
            var Split = eval("(class { async\n#m() { return 1; } get() { return this.#m(); } })");
            var split = new Split();
            assert.areEqual(undefined, split.async);
            assert.areEqual(1, split.get());
            // A newline after `*` still starts a generator private method.
            var StarSplit = eval("(class { *\n#m() { yield 2; } it() { return [...this.#m()]; } })");
            assert.areEqual("2", new StarSplit().it().join(","));
            // A star after a field newline starts the next element: a
            // private field plus a public generator.
            var AfterField = eval("(class { #x\n*gen() { yield 3; } pub() { return [...this.gen()]; } })");
            assert.areEqual("3", new AfterField().pub().join(","));
        }
    },
    {
        name: "async private completions settle with the expected values",
        body() {
            // Single chained snippet (see assertAsyncCompletion): each step
            // covers one async case above, and one _asyncExit settles them
            // all. Any mismatch rejects the chain and fails this test.
            assertAsyncCompletion(
                "class CAsyncV { #v = 40; async #m(a, b) { await 1; return this.#v + a + b; } call(a, b) { return this.#m(a, b); } }" +
                "class CAsyncG { #n = 3; async *#a() { for (let i = 0; i < this.#n; i++) { await i; yield i * 2; } } it() { return this.#a(); } }" +
                "class CStaticA { static #v = 40; static async #m(x) { await 1; return this.#v + x; } static call(x) { return this.#m(x); } }" +
                "class CStaticAG { static async *#a(x) { await 1; yield x * 2; } static it(x) { return this.#a(x); } }" +
                "class CDrvB { constructor() { this.base = 100; } val() { return this.base; } }" +
                "class CDrv extends CDrvB { #x = 1; async #m(y) { await 1; return this.val() + this.#x + y; } call(y) { return this.#m(y); } }" +
                "class CManip { async *#m() { await 1; yield 7; } call() { return this.#m(); } }" +
                "function CPump(it, out) { return it.next().then(function (r) { if (r.done) { return out; } out.push(r.value); return CPump(it, out); }); }" +
                "new CAsyncV().call(1, 1).then(function (v) { assert.areEqual(42, v); })" +
                ".then(function () { return CPump(new CAsyncG().it(), []); })" +
                ".then(function (out) { assert.areEqual('0,2,4', out.join(',')); return CStaticA.call(2); })" +
                ".then(function (v) { assert.areEqual(42, v); return CStaticAG.it(21).next(); })" +
                ".then(function (r) { assert.areEqual(42, r.value); assert.areEqual(false, r.done); return new CDrv().call(1); })" +
                ".then(function (v) { assert.areEqual(102, v);" +
                " var inst = new CManip(); Object.setPrototypeOf(CManip.prototype, null); Object.freeze(inst); return inst.call().next(); })" +
                ".then(function (r) { assert.areEqual(7, r.value); assert.areEqual(false, r.done); _asyncExit(); }," +
                " function (e) { assert.fail('async completion failed: ' + (e && e.stack || e)); });");
        }
    },
    {
        // S1 tail: direct eval inherits the caller's private environment.
        // Private references in eval code inside methods, accessors and
        // field initializers resolve to the enclosing class's names with
        // runtime brand checks; indirect eval and undeclared names throw
        // SyntaxError (the latter before any eval statement executes).
        name: "direct eval inherits the caller's private environment",
        body() {
            class C {
                #m = 44;
                getWithEval() { return eval("this.#m"); }
            }
            class D {
                #m = 45;
            }
            const c = new C();
            assert.areEqual(44, c.getWithEval());
            assert.throws(function () { c.getWithEval.call(new D()); }, TypeError, "brand of the enclosing class applies");

            class CM {
                #m() { return "Test262"; }
                callWithEval() { return eval("this.#m()"); }
            }
            class DM {
                #m() { throw new Error("must not run"); }
            }
            const cm = new CM();
            assert.areEqual("Test262", cm.callWithEval());
            assert.throws(function () { cm.callWithEval.call(new DM()); }, TypeError);

            class CA {
                get #v() { return "Test262"; }
                getWithEval() { return eval("this.#v"); }
            }
            assert.areEqual("Test262", new CA().getWithEval());

            class CS {
                #v = 1;
                setWithEval(v) { eval("this.#v = v"); return this.#v; }
            }
            assert.areEqual("Test262", new CS().setWithEval("Test262"));

            class CI {
                #m = 11;
                v = eval("this.#m * 2");
            }
            assert.areEqual(22, new CI().v);

            class CST {
                static #m() { return "Test262"; }
                static callWithEval() { return eval("this.#m()"); }
            }
            assert.areEqual("Test262", CST.callWithEval());

            // Indirect eval never resolves private names.
            class IND {
                #m = 1;
                f() { return (0, eval)("this.#m"); }
            }
            assert.throws(function () { new IND().f(); }, SyntaxError);

            // Undeclared names are early errors: nothing executes first,
            // including inside nested functions.
            let executed = false;
            class UN {
                f() { return eval("executed = true; this.#undeclared;"); }
                g() { return eval("function h() { return this.#undeclared; }"); }
            }
            const un = new UN();
            assert.throws(function () { un.f(); }, SyntaxError);
            assert.throws(function () { un.g(); }, SyntaxError);
            assert.areEqual(false, executed, "eval body did not start executing");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
