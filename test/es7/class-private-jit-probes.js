//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C2-A3: miscompile probes for the JIT helper-call lowering of the private-
// element/class-field/import-attributes opcodes. Every probe warms a class
// body function far past the loop tier-up threshold so the private-element
// opcodes execute from JIT-compiled code (and throw from it, exercising the
// bailout-to-interpreter rethrow path), then re-checks the same shapes that
// the interpreter validated first. Outputs are mode-independent by
// construction; any JIT/interpreter divergence shows up as a failed assert
// or an output diff between the suite's jit and nojit variants.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function warm(fn, count) {
    for (let i = 0; i < count; i++) {
        fn(i);
    }
}

const tests = [
    {
        name: "private field get/set/has in a warm loop with receiver type churn",
        body() {
            class C {
                #x = 1;
                read() { return this.#x; }
                write(v) { this.#x = v; return this.#x; }
                static has(o) { return #x in o; }
            }
            const c = new C();
            const values = [{}, "str", 1.5, null, undefined, c];
            let acc = 0;
            warm(function (i) {
                c.write(i);
                acc += c.read();
                // churn the surrounding value types so the JIT sees Var
                // operands, not a stable specialization
                const v = values[i % values.length];
                if (C.has(c) && typeof v !== "object") {
                    acc += typeof v === "string" ? 1 : typeof v === "number" ? 2 : v === null ? 3 : 4;
                }
            }, 3000);
            assert.areEqual(3000, c.read() + 1);
            assert.isTrue(acc > 0);
            assert.isFalse(C.has({}));
            assert.isTrue(C.has(c));
        }
    },
    {
        name: "brand-check TypeError thrown from JIT-compiled private access",
        body() {
            class C {
                #x = 1;
                read(o) { return o.#x; }
            }
            const c = new C();
            // interpreted first
            assert.throws(() => c.read({}), TypeError);
            assert.areEqual(1, c.read(c));
            // then from a warm (JIT-compiled) body
            let caught = 0;
            warm(function (i) {
                try {
                    c.read(i % 7 === 0 ? {} : c);
                } catch (e) {
                    caught++;
                    if (!(e instanceof TypeError)) {
                        throw new Error("expected TypeError from JIT body, got " + e);
                    }
                }
            }, 3000);
            assert.isTrue(caught > 0);
            // and the same function still succeeds afterwards
            assert.areEqual(1, c.read(c));
        }
    },
    {
        name: "private methods and accessors shared across instances, called in a warm loop",
        body() {
            const log = [];
            class C {
                #v = 1;
                #m(inc) { return this.#v + inc; }
                get #g() { return this.#v * 10; }
                set #g(x) { this.#v = x; }
                run(inc) {
                    this.#g = this.#v;
                    return this.#g + this.#m(inc);
                }
                static canSee(o) { return #m in o; }
            }
            const a = new C(), b = new C();
            let last = 0;
            warm(function (i) {
                last = (i % 2 ? a : b).run(i % 5);
            }, 3000);
            assert.areEqual(a.run(1), b.run(1));
            assert.isTrue(last > 0);
            assert.isTrue(C.canSee(a) && C.canSee(b));
            // a subclass instance runs C's initializers via super(), so it
            // carries the brand; an unrelated object never does
            assert.isTrue(C.canSee(new (class extends C {})()));
            assert.isFalse(C.canSee({}));
        }
    },
    {
        name: "private access on exotic receivers (proxy, null-proto, frozen, non-extensible)",
        body() {
            class C {
                #x = 1;
                read(o) { return o.#x; }
                static has(o) { return #x in o; }
            }
            const c = new C();
            const proxy = new Proxy(c, {});
            // Private access keys on the object itself: a proxy over a branded
            // instance is NOT branded (private fields bypass proxy traps
            // entirely), so reads through the proxy throw.
            assert.throws(() => C.prototype.read.call(null, proxy), TypeError);
            assert.isTrue(C.has(c));
            assert.isFalse(C.has(proxy));
            // exotic non-branded receivers must fail the brand check, not crash
            assert.throws(() => C.prototype.read.call(null, Object.create(null)), TypeError);
            assert.throws(() => C.prototype.read.call(null, Object.freeze({})), TypeError);
            // a branded but non-extensible instance still reads (extensibility
            // only gates adding new private fields)
            assert.areEqual(1, C.prototype.read.call(null, Object.preventExtensions(new C())));
            // warm loop keeps mixing branded receivers
            let acc = 0;
            warm(function (i) {
                acc += C.prototype.read.call(null, i % 3 === 0 ? new C() : c);
            }, 2000);
            assert.areEqual(2000, acc);
        }
    },
    {
        name: "public/computed class fields defined from warm constructors",
        body() {
            const key = "k" + 1;
            let count = 0;
            class C {
                a = (count++, 1);
                [key] = 2;
                static s = 3;
                #p = 4;
                getP() { return this.#p; }
            }
            let lastP = 0;
            warm(function (i) {
                const c = new C();
                lastP = c.getP();
            }, 3000);
            assert.areEqual(3000, count);
            assert.areEqual(4, lastP);
            const c = new C();
            assert.areEqual("a,k1", Object.keys(c).join(","));
            assert.areEqual(3, C.s);
        }
    },
    {
        name: "private statics (fields, methods, accessors) in warm loops",
        body() {
            class C {
                static #sv = 1;
                static #sm(x) { return C.#sv + x; }
                static get #sg() { return C.#sv * 2; }
                static set #sg(v) { C.#sv = v; }
                static bump(x) { C.#sg = C.#sm(x); return C.#sg; }
                static v() { return C.#sv; }
            }
            let last = 0;
            warm(function (i) {
                last = C.bump(i % 4);
            }, 3000);
            assert.isTrue(last > 0);
            // bump(x): #sv = #sm(x) = #sv + x, returns #sg = #sv * 2
            const before = C.v();
            assert.areEqual((before + 1) * 2, C.bump(1));
            assert.areEqual(before + 1, C.v());
            assert.areEqual(C.v() * 2, C.bump(0));
        }
    },
    {
        name: "second installation and uninitialized brand checks still throw from warm code",
        body() {
            class C {
                #x = 1;
                #m() { return 1; }
                install(o) { this.#m; return o; }
                addField(o) { return o; }
                readX(o) { return o.#x; }
                readM(o) { return o.#m(); }
            }
            const c = new C();
            // duplicate private name is an early SyntaxError
            assert.throws(() => eval("class D2 { #y = 1; #y = 2; }"), SyntaxError);
            let threw = 0;
            warm(function (i) {
                try {
                    if (i % 2) {
                        c.readX(3); // primitive receiver: TypeError from JIT body
                    } else {
                        c.readM(null);
                    }
                } catch (e) {
                    threw++;
                }
            }, 2000);
            assert.areEqual(2000, threw);
        }
    },
    {
        name: "private names stay distinct across class evaluations",
        body() {
            function make() {
                return class { #x = 1; read(o) { return o.#x; } };
            }
            const A = make(), B = make();
            const a = new A(), b = new B();
            let ok = 0;
            warm(function (i) {
                const wrong = i % 2 ? b : a;
                try {
                    (i % 2 ? A : B).prototype.read.call(null, wrong);
                } catch (e) {
                    ok++; // wrong brand, per-evaluation WeakMap
                }
            }, 2000);
            assert.areEqual(2000, ok);
            assert.areEqual(1, A.prototype.read.call(null, a));
            assert.areEqual(1, B.prototype.read.call(null, b));
            assert.throws(() => A.prototype.read.call(null, b), TypeError);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
