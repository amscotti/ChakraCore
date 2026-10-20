//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-S1: private instance fields (`#x`, `#x = init`) and `#x in obj` brand
// checks. Private methods/accessors, static/private-static fields, static
// blocks, computed names and decorators are intentionally out of this slice.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "private fields are per-instance, hidden from reflection, and follow declaration order",
        body() {
            const log = [];
            class C {
                a = (log.push("a"), 1);
                #p = (log.push("p"), this.a + 1);
                b = (log.push("b"), this.#p + 1);
                #q;
                get() { return this.#p; }
                getQ() { return this.#q; }
            }

            const c = new C();
            assert.areEqual("a,p,b", log.join(","));
            assert.areEqual(2, c.get());
            assert.areEqual(undefined, c.getQ());
            assert.areEqual("a,b", Object.getOwnPropertyNames(c).join(","));
            assert.areEqual("a,b", Object.keys(c).join(","));
            assert.areEqual(0, Object.getOwnPropertySymbols(c).length);
            assert.isFalse(Object.prototype.hasOwnProperty.call(c, "#p"));
        }
    },
    {
        name: "get/set brand checks reject wrong receivers with TypeError",
        body() {
            class C {
                #x = 1;
                get() { return this.#x; }
                set(v) { this.#x = v; }
            }

            const c = new C();
            assert.areEqual(1, c.get());
            c.set(7);
            assert.areEqual(7, c.get());

            assert.throws(function () { const get = c.get; get(); }, TypeError);
            assert.throws(function () { const set = c.set; set(1); }, TypeError);
            assert.throws(function () { ({ get: c.get }).get(); }, TypeError);
            assert.throws(function () { ({ set: c.set }).set(1); }, TypeError);
        }
    },
    {
        name: "#x in obj is an own-brand check only",
        body() {
            class C {
                #x = 1;
                static has(o) { return #x in o; }
            }

            const c = new C();
            assert.isTrue(C.has(c));
            assert.isFalse(C.has({}));
            assert.isFalse(C.has(Object.create(c)));
            assert.throws(function () { C.has(1); }, TypeError);
            assert.throws(function () { C.has(null); }, TypeError);

            const proxy = new Proxy(c, {});
            assert.isFalse(C.has(proxy));
        }
    },
    {
        name: "class evaluations get distinct brands",
        body() {
            function make() {
                return class {
                    #x = 1;
                    get() { return this.#x; }
                    has(o) { return #x in o; }
                };
            }

            const A = make();
            const B = make();
            const a = new A();
            const b = new B();
            assert.isTrue(a.has(a));
            assert.isTrue(b.has(b));
            assert.isFalse(a.has(b));
            assert.isFalse(b.has(a));
            assert.throws(function () { const get = A.prototype.get; const o = { get }; o.get(); }, TypeError);
        }
    },
    {
        name: "nested classes can read outer private names and can shadow them",
        body() {
            class Outer {
                #x = 10;
                make() {
                    return class Inner {
                        #y = 20;
                        sum(o) { return o.#x + this.#y; }
                        hasOuter(o) { return #x in o; }
                    };
                }
            }

            class Shadow {
                #x = 99;
                read(o) { return o.#x; }
            }

            const outer = new Outer();
            const Inner = outer.make();
            const inner = new Inner();
            assert.areEqual(30, inner.sum(outer));
            assert.isTrue(inner.hasOuter(outer));
            assert.areEqual(99, new Shadow().read(new Shadow()));
        }
    },
    {
        name: "base and derived placement, initializer this, and nonextensible behavior",
        body() {
            const log = [];
            class Base {
                #x = 2;
                constructor() { log.push("base"); }
                getX() { return this.#x; }
                hasX(o) { return #x in o; }
            }
            class Derived extends Base {
                #y = this.getX() + 1;
                constructor() {
                    super();
                    log.push(this.#y);
                }
                getY() { return this.#y; }
            }

            const d = new Derived();
            assert.areEqual("base,3", log.join(","));
            assert.areEqual(2, d.getX());
            assert.areEqual(3, d.getY());
            assert.isTrue(d.hasX(d));

            class NonExtensible {
                #v = (Object.preventExtensions(this), 1);
            }
            assert.throws(function () { new NonExtensible(); }, TypeError);

            class Trojan {
                constructor(o) { return o; }
            }
            class AddToFrozen extends Trojan {
                #v = 1;
                constructor(o) { super(o); }
            }
            assert.throws(function () { new AddToFrozen(Object.preventExtensions({})); }, TypeError);

            class ReadFrozen {
                #v = 1;
                get() { return this.#v; }
            }
            const frozen = new ReadFrozen();
            Object.preventExtensions(frozen);
            assert.areEqual(1, frozen.get());
        }
    },
    {
        name: "private fields can hold functions, participate in compound assignment, and support typeof",
        body() {
            class C {
                #f = function (x) { return x + 1; };
                #n = 1;
                call() { return this.#f(2); }
                add(v) { return this.#n += v; }
                inc() { return this.#n++; }
                read() { return this.#n; }
                type() { return typeof this.#n; }
            }

            const c = new C();
            assert.areEqual(3, c.call());
            assert.areEqual(3, c.add(2));
            assert.areEqual(3, c.inc());
            assert.areEqual(4, c.read());
            assert.areEqual("number", c.type());
        }
    },
    {
        name: "anonymous private-field initializers receive the private name with '#'",
        body() {
            class C {
                #foo = function () {};
                #bar = () => {};
                #Baz = class {};
                names() { return [this.#foo.name, this.#bar.name, this.#Baz.name]; }
            }
            assert.areEqual("#foo,#bar,#Baz", new C().names().join(","));
        }
    },
    {
        name: "#x in function, arrow, generator, async, class and object values",
        body() {
            class C {
                #x;
                static isIn(v) { return #x in v; }
            }
            assert.isFalse(C.isIn(function () {}));
            assert.isFalse(C.isIn(() => {}));
            assert.isFalse(C.isIn(function* () {}));
            assert.isFalse(C.isIn(async function () {}));
            assert.isFalse(C.isIn(class {}));
            assert.isFalse(C.isIn({}));
        }
    },
    {
        name: "arrow super initializes private fields without creating public properties",
        body() {
            class B {
                constructor() { this.b = 1; }
            }
            class D extends B {
                #x = 42;
                constructor() { (() => super())(); }
                getX() { return this.#x; }
            }
            const d = new D();
            assert.areEqual("b", Object.keys(d).join(","));
            assert.areEqual(undefined, d.x);
            assert.areEqual(42, d.getX());
            assert.throws(function () { D.prototype.getX.call({}); }, TypeError);
        }
    },
    {
        name: "parenthesized in expressions are valid on the right of #x in",
        body() {
            assert.doesNotThrow(function () {
                eval("class C { #x; static f(o, p) { return #x in (o in p); } }");
            });
            assert.doesNotThrow(function () {
                eval("class C { #x; static f(o, p) { return #x in ((o in p)); } }");
            });
            assert.doesNotThrow(function () {
                eval("class C { #x; static f(o, p) { return #x in (o in p) in o; } }");
            });
            assert.doesNotThrow(function () {
                // Node accepts this as (#x in o) in p; only a bare
                // PrivateIdentifier right operand is invalid.
                eval("class C { #x; static f(o, p) { return #x in o in p; } }");
            });
            assert.throws(function () {
                eval("class C { #x; static f() { return #x in #x in this; } }");
            }, SyntaxError);

            class C {
                #x;
                static f(o, p) { return #x in (o in p); }
            }
            assert.throws(function () { C.f({}, {}); }, TypeError);
        }
    },
    {
        name: "deferred fast scan still reports private-name syntax errors",
        body() {
            assert.throws(function () { eval("function f() { return this.#x; }"); }, SyntaxError);
            assert.throws(function () { eval("function f() { class C { #x; #x; } }"); }, SyntaxError);
            assert.throws(function () { eval("function f() { class Outer { #x; class Inner { #y; #y; } } }"); }, SyntaxError);
            assert.throws(function () { Function("return this.#x"); }, SyntaxError);

            var prefix = "var n = 0;";
            for (var i = 0; i < 300; i++) {
                prefix += " n += 1;";
            }
            assert.throws(function () {
                eval("function f() { " + prefix + " return this.#x; }");
            }, SyntaxError);

            assert.doesNotThrow(function () {
                eval("function f() { return class C { #x = 1; m() { return this.#x; } }; }");
            });
        }
    },
    {
        name: "early errors: duplicate names, #constructor, undeclared, and out-of-class use",
        body() {
            assert.throws(function () { eval("class C { #x; #x; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #constructor; }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #constructor; m() { return this.#constructor; } }"); }, SyntaxError);
            assert.throws(function () { eval("class C { m() { return this.#x; } }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; m() { return this.#y; } }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; } new C().#x;"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; } #x in {};"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; m() { delete this.#x; } }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; m() { for (#x in {}); } }"); }, SyntaxError);
            assert.throws(function () { eval("class C { #x; m() { return #x in #x in this; } }"); }, SyntaxError);
        }
    },
    {
        name: "public fields and methods are unaffected",
        body() {
            class C {
                x = 1;
                #x = 2;
                m() { return this.x + this.#x; }
            }
            const c = new C();
            assert.areEqual(3, c.m());
            assert.areEqual(1, c.x);
            assert.areEqual("x", Object.keys(c).join(","));
        }
    },
    {
        // S1 tail: private references inside computed property names resolve
        // against the class private environment; the brand const is
        // initialized before key evaluation, so a missing brand is a
        // TypeError (never a use-before-declaration error), whatever the
        // declaration order.
        name: "private names resolve in computed field keys with runtime brand checks",
        body() {
            const outer = {};
            assert.throws(function () {
                class C {
                    [outer.#f] = "Test262";
                    #f = "foo";
                }
            }, TypeError, "brand missing on the key receiver is a TypeError");
            assert.throws(function () {
                class C {
                    #f = "foo";
                    [outer.#f] = "Test262";
                }
            }, TypeError, "declaration order does not matter");
            assert.throws(function () {
                class C {
                    [outer.#m()] = "Test262";
                    #m() { throw new Error("must not run"); }
                }
            }, TypeError, "private method brand missing on the key receiver");
            assert.throws(function () {
                class C {
                    #f = "foo";
                    [outer.#f];
                }
            }, TypeError, "bare computed keys with private refs brand-check too");
        }
    },
    {
        // S1 tail: PrivateFieldGet/Put on a primitive receiver throws
        // TypeError (spec GetValue order: the primitive is checked before the
        // brand lookup runs).
        name: "private field access on primitive receivers throws TypeError",
        body() {
            class C {
                #p = 1;
                get() { return this.#p; }
                set(v) { this.#p = v; }
            }
            const c = new C();
            [15, "Test262", Symbol("Test262"), null, undefined].forEach(function (receiver) {
                assert.throws(function () { c.get.call(receiver); }, TypeError, "get on primitive " + String(receiver));
                assert.throws(function () { c.set.call(receiver, 2); }, TypeError, "set on primitive " + String(receiver));
            });
            assert.areEqual(1, c.get.call(c), "instance access keeps working");
        }
    },
    {
        // S1 tail: in object destructuring the assignment target is evaluated
        // as a reference before the property value is read, so a receiver
        // that throws (uninitialized `this` of a derived constructor) throws
        // before any getter on the right-hand side runs.
        name: "object destructuring evaluates the target reference before reading the value",
        body() {
            const log = [];
            class Base { }
            class C extends Base {
                #field;
                field;
                constructor(which) {
                    const init = () => super();
                    const object = {
                        get a() { log.push("get:" + which); init(); return 1; }
                    };
                    if (which === "private") {
                        ({ a: this.#field } = object);
                    } else {
                        ({ a: this.field } = object);
                    }
                }
            }
            assert.throws(function () { new C("private"); }, ReferenceError, "private target throws before the getter");
            assert.throws(function () { new C("public"); }, ReferenceError, "public target throws before the getter");
            assert.areEqual("", log.join(","), "no getter ran");

            // Sanity: key, target, value evaluate in order for member targets.
            const order = [];
            const holder = {};
            const key = function () { order.push("key"); return "p"; };
            const target = function () { order.push("target"); return holder; };
            const source = { get p() { order.push("get"); return 1; } };
            ({ [key()]: target().x } = source);
            assert.areEqual("key,target,get", order.join(","));
            assert.areEqual(1, holder.x);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
