//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: private static class fields (`static #x;`, `static #x = expr;`).
// Private static methods/accessors are covered in class-private-methods.js
// (doc/c2-s1-private-static-methods.md). Static blocks, computed static
// names and decorators remain out of scope
// (doc/c2-s1-static-private-fields.md).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "static private fields are own storage on the constructor and hidden from reflection",
        body() {
            class C {
                static #x;
                static #y = 1;
                static readX() { return this.#x; }
                static readY() { return this.#y; }
                static writeX(v) { this.#x = v; }
                static hasX(o) { return #x in o; }
            }

            assert.areEqual(undefined, C.readX());
            assert.areEqual(1, C.readY());
            C.writeX(7);
            assert.areEqual(7, C.readX());
            assert.isTrue(C.hasX(C));

            // Private state is not a property of the constructor, the
            // prototype or instances.
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "#x"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "#y"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(C.prototype, "#x"));
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(C, "#x"));

            const names = Object.getOwnPropertyNames(C);
            assert.areEqual(-1, names.indexOf("#x"));
            assert.areEqual(-1, names.indexOf("#y"));
            assert.isFalse(Object.getOwnPropertySymbols(C).some(function (s) {
                return String(s).indexOf("#x") !== -1;
            }));

            const c = new C();
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "#x"));
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "#y"));
            assert.isFalse(Object.keys(c).some(function (key) { return key.indexOf("#") === 0; }));
            assert.isFalse(C.hasX(c));
            assert.isFalse(C.hasX(C.prototype));
            assert.isFalse(C.hasX(Object.create(C)));
        }
    },
    {
        name: "public and private static fields initialize in textual order, after methods and the class name",
        body() {
            const log = [];
            class C {
                static a = (log.push("a"), 1);
                static #p = (log.push("p"), 2);
                static method() { log.push("method"); }
                static b = (log.push("b"), this.a + this.#p);
                static #q = (log.push("q"), this.b + 1);
                static c = (log.push("c"), C === this);
                static self = C;
                static read() { return [this.a, this.#p, this.b, this.#q, this.c, this.self === C]; }
            }

            assert.areEqual("a,p,b,q,c", log.join(","));
            assert.areEqual("1,2,3,4,true,true", C.read().join(","));

            // Methods are installed in the first pass even when they appear
            // after the field that reads them.
            class FieldBeforeMethod {
                static #type = typeof this.method;
                static method() {}
                static read() { return this.#type; }
            }
            assert.areEqual("function", FieldBeforeMethod.read());

            // No initializer defines undefined rather than leaving the brand
            // absent.
            class NoInitializer {
                static #missing;
                static read() { return this.#missing; }
                static has(o) { return #missing in o; }
            }
            assert.isTrue(NoInitializer.has(NoInitializer));
            assert.areEqual(undefined, NoInitializer.read());
        }
    },
    {
        name: "private static initializer `this` is the constructor and every field is added once",
        body() {
            const seen = [];
            class C {
                static #a = (seen.push(this), 1);
                static #b = (seen.push(this), this.#a + 1);
                static arrow = () => this;
                static read() { return this.#a + this.#b; }
            }
            assert.areEqual(C, seen[0]);
            assert.areEqual(C, seen[1]);
            assert.areEqual(3, C.read());
            assert.areEqual(C, C.arrow());

            class D {
                static #a = 1;
                static writeOnce() { this.#a = 2; return this.#a; }
            }
            assert.areEqual(2, D.writeOnce());
        }
    },
    {
        name: "duplicate public static names run every initializer; duplicate private names are early errors",
        body() {
            const log = [];
            class C {
                static x = (log.push("first"), 1);
                static x = (log.push("second"), 2);
            }
            assert.areEqual("first,second", log.join(","));
            assert.areEqual(2, C.x);

            assert.throws(function () { eval("class D { static #x = 1; static #x = 2; }"); }, SyntaxError);
            assert.throws(function () { eval("class D { static #x; static #x = 2; }"); }, SyntaxError);
            assert.throws(function () { eval("class D { static #x; #x; }"); }, SyntaxError);
            assert.throws(function () { eval("class D { #x; static #x; }"); }, SyntaxError);
            assert.throws(function () { eval("class D { static #x; static #x() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class D { static #x() {} static #x; }"); }, SyntaxError);
            assert.throws(function () { eval("class D { static #x; static get #x() { return 1; } }"); }, SyntaxError);
        }
    },
    {
        name: "private static brands are per class evaluation and are not inherited",
        body() {
            class Base {
                static #secret = 1;
                static read() { return this.#secret; }
                static has(o) { return #secret in o; }
            }
            class Derived extends Base {}

            assert.areEqual(1, Base.read());
            assert.areEqual(1, Derived.read.call(Base));
            assert.throws(function () { Derived.read.call(Derived); }, TypeError);
            assert.throws(function () { Base.read.call(Derived); }, TypeError);
            assert.throws(function () { Base.read.call({}); }, TypeError);
            assert.throws(function () { Base.read.call(1); }, TypeError);
            assert.isTrue(Base.has(Base));
            assert.isFalse(Base.has(Derived));
            assert.isFalse(Base.has({}));
            assert.throws(function () { Base.has(1); }, TypeError);

            class DerivedWithOwn extends Base {
                static #own = 2;
                static readOwn() { return this.#own; }
            }
            assert.areEqual(2, DerivedWithOwn.readOwn());
            assert.throws(function () { DerivedWithOwn.read.call(DerivedWithOwn); }, TypeError);

            // Each evaluation of the same class source gets a fresh brand.
            const make = function () {
                return class {
                    static #v = 42;
                    static read() { return this.#v; }
                };
            };
            const A = make();
            const B = make();
            assert.areEqual(42, A.read());
            assert.areEqual(42, B.read());
            assert.throws(function () { A.read.call(B); }, TypeError);
            assert.throws(function () { B.read.call(A); }, TypeError);
        }
    },
    {
        name: "brand checks for C.#x, this.#x, compound assignment, typeof and in",
        body() {
            class C {
                static #n = 1;
                static #f = function (x) { return x + 1; };
                static readCtor() { return C.#n; }
                static readThis() { return this.#n; }
                static add(v) { return this.#n += v; }
                static inc() { return this.#n++; }
                static call(v) { return this.#f(v); }
                static type() { return typeof this.#n; }
                static has(o) { return #n in o; }
            }

            assert.areEqual(1, C.readCtor());
            assert.areEqual(1, C.readThis());
            assert.areEqual(4, C.add(3));
            assert.areEqual(4, C.inc());
            assert.areEqual(5, C.readThis());
            assert.areEqual(4, C.call(3));
            assert.areEqual("number", C.type());
            assert.isTrue(C.has(C));
            assert.isFalse(C.has(C.prototype));

            assert.throws(function () { C.readThis.call(0); }, TypeError);
            assert.throws(function () { C.has("C"); }, TypeError);
        }
    },
    {
        name: "nested classes can shadow or capture outer private static names",
        body() {
            class Outer {
                static #x = 41;
                static readOuter() { return this.#x; }
                static Inner = class {
                    static #x = 2;
                    static readInner() { return this.#x; }
                };
            }

            assert.areEqual(41, Outer.readOuter());
            assert.areEqual(2, Outer.Inner.readInner());
            assert.throws(function () { Outer.Inner.readInner.call(Outer); }, TypeError);
            assert.throws(function () { Outer.readOuter.call(Outer.Inner); }, TypeError);

            const Outer2 = class {
                static #x = 1;
                static read() { return this.#x; }
                static makeInner() {
                    return class {
                        static read(o) { return o.#x; }
                    };
                }
            };
            const Inner2 = Outer2.makeInner();
            assert.areEqual(1, Inner2.read(Outer2));
            assert.throws(function () { Inner2.read(Inner2); }, TypeError);
        }
    },
    {
        name: "field ASI before generator methods works after public and private field names",
        body() {
            class StaticPrivate {
                static #x = 7;
                static #y
                *gen() { yield StaticPrivate.#x; }
                *[Symbol.iterator]() { yield StaticPrivate.#y === undefined; }
            }
            assert.areEqual(7, new StaticPrivate().gen().next().value);
            assert.isTrue([...new StaticPrivate()][0]);

            class InstancePrivate {
                #x = 5;
                #y
                *gen() { yield this.#x; }
                *[Symbol.iterator]() { yield this.#y === undefined; }
            }
            assert.areEqual(5, new InstancePrivate().gen().next().value);
            assert.isTrue([...new InstancePrivate()][0]);

            class StaticPublic {
                static x = 1;
                static y
                *gen() { yield StaticPublic.x; }
                *[Symbol.iterator]() { yield StaticPublic.y === undefined; }
            }
            assert.areEqual(1, new StaticPublic().gen().next().value);
            assert.isTrue([...new StaticPublic()][0]);

            class InstancePublic {
                x = 1;
                y
                *gen() { yield this.x; }
                *[Symbol.iterator]() { yield this.y === undefined; }
            }
            assert.areEqual(1, new InstancePublic().gen().next().value);
            assert.isTrue([...new InstancePublic()][0]);

            // No line terminator after the field name: not field ASI.
            assert.throws(function () { eval("class C { static #x *gen() {} }"); }, SyntaxError);
        }
    },
    {
        name: "frozen constructors keep existing private static state; adding private static storage matches PrivateFieldAdd",
        body() {
            class C {
                static #x = 1;
                static read() { return this.#x; }
                static write(v) { this.#x = v; }
            }
            Object.freeze(C);
            assert.areEqual(1, C.read());
            C.write(2);
            assert.areEqual(2, C.read());

            // Static private fields are private elements, not properties:
            // like Node and the spec, a frozen constructor still accepts
            // its own private static storage.
            const Frozen = eval(
                "(class { static #x = (Object.freeze(this), 1); static read() { return this.#x; } })");
            assert.isTrue(Object.isFrozen(Frozen));
            assert.areEqual(1, Frozen.read());

            const NonExtensible = eval(
                "(class { static #x = (Object.preventExtensions(this), 2); static read() { return this.#x; } })");
            assert.isFalse(Object.isExtensible(NonExtensible));
            assert.areEqual(2, NonExtensible.read());
        }
    },
    {
        name: "wrong receivers and proxies cannot observe private static fields",
        body() {
            class C {
                static #x = 1;
                static read() { return this.#x; }
            }
            const P = new Proxy(C, {});
            assert.areEqual(1, C.read());
            assert.throws(function () { P.read(); }, TypeError);
            assert.throws(function () { C.read.call(P); }, TypeError);
            assert.throws(function () { C.read.call(Object.create(C)); }, TypeError);
        }
    },
    {
        name: "early errors: #constructor, undeclared, out-of-class, forward use, and out-of-scope static private forms",
        body() {
            assert.throws(function () { eval("class C { static #constructor; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #constructor = 1; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #x; }; this.#x;"); }, SyntaxError);
            assert.throws(function () { eval("this.#x;"); }, SyntaxError);
            assert.throws(function () { eval("function f() { return this.#x; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #x; static m() { return this.#y; } }"); }, SyntaxError);

            // A private static field may be referenced before its declaration
            // textually, but the brand is absent until its add runs.
            assert.throws(function () {
                eval("class C { static #x = this.#y; static #y = 1; }");
            }, TypeError);

            // Private static methods/accessors are supported (see
            // class-private-methods.js); static blocks, computed names and
            // async/generator private variants remain SyntaxError.
            assert.doesNotThrow(function () { eval("class C { static #m() {} }"); });
            assert.doesNotThrow(function () { eval("class C { static get #p() { return 1; } }"); });
            assert.doesNotThrow(function () { eval("class C { static set #p(v) {} }"); });
            assert.throws(function () { eval("class C { static * #g() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static async #m() {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static {} }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static ['computed'] = 1; }"); }, SyntaxError);

            // Static initializer early errors match public fields.
            assert.throws(function () { eval("class C { static #x = arguments; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { static #x = super(); }"); }, SyntaxError);
        }
    },
    {
        name: "static private fields can hold functions with the private name and public fields are unaffected",
        body() {
            class C {
                static #field = () => 'Test262';
                static field = function () { return 42; };
                static accessPrivateField() { return this.#field; }
            }
            assert.areEqual("#field", C.accessPrivateField().name);
            assert.areEqual("field", C.field.name);
            assert.isFalse(Object.prototype.hasOwnProperty.call(C, "#field"));
        }
    },
    {
        name: "static field initializers run after static private methods are installed",
        body() {
            // A field declared before the method still observes it, and a
            // field declared after the method may call it: brands install
            // during member evaluation, before any static initializer runs.
            class C {
                static early = C.late(1);
                static #m() { return 40; }
                static late(x) { return this.#m() + x; }
                static #n = 2;
                static useBoth() { return this.#m() + this.#n; }
            }
            assert.areEqual(41, C.early);
            assert.areEqual(41, C.late(1));
            assert.areEqual(42, C.useBoth());

            // Methods see fields initialized before them in textual order.
            var log = [];
            class D {
                static #a = (log.push("a"), 1);
                static #m() { return log.push("m"), this.#a + 1; }
                static #b = (log.push("b"), this.#m() + 1);
                static read() { return [this.#a, this.#b, log.join(",")]; }
            }
            assert.areEqual("1|3|a,b,m", D.read().join("|"));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
