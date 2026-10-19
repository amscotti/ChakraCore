//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function parses(code) {
    try {
        eval("function test() {" + code + "}");
        return true;
    } catch (e) {
        return false;
    }
}

var tests = [
    {
        name: "Basic member and index chains",
        body() {
            const a = { b: { c: 42 } };
            assert.areEqual(42, a?.b?.c);
            assert.areEqual(42, a?.b.c);
            assert.areEqual(42, a?.["b"]?.["c"]);
            assert.areEqual(undefined, null?.b);
            assert.areEqual(undefined, undefined?.b.c);
            assert.areEqual(undefined, null?.b?.c);
            assert.areEqual(undefined, null?.["b"]);
        }
    },
    {
        name: "Non-optional access after a nullish intermediate still throws",
        body() {
            assert.throws(() => ({ b: null })?.b.c, TypeError);
            assert.throws(() => ({ b: undefined })?.b.c, TypeError);
        }
    },
    {
        name: "'a?.5:3' remains a conditional expression",
        body() {
            assert.areEqual(0.5, true?.5:3);
            assert.areEqual(3, false?.5:3);
        }
    },
    {
        name: "Short-circuit skips keys, arguments, and the rest of the chain",
        body() {
            let count = 0;
            const key = () => { count++; return "x"; };
            const arg = () => { count++; return 1; };
            const n = null;
            assert.areEqual(undefined, n?.[key()]);
            assert.areEqual(undefined, n?.b[key()]);
            assert.areEqual(undefined, n?.b(arg()));
            assert.areEqual(0, count);
        }
    },
    {
        name: "Base is evaluated exactly once",
        body() {
            let count = 0;
            const get = () => { count++; return { b: 7 }; };
            assert.areEqual(7, get()?.b);
            assert.areEqual(1, count);
        }
    },
    {
        name: "'this' binding of optional and parenthesized calls",
        body() {
            const a = { b() { return this._b; }, _b: { c: 42 } };
            assert.areEqual(42, a?.b().c);
            assert.areEqual(42, (a?.b)().c);
            assert.areEqual(42, a.b?.().c);
            assert.areEqual(42, (a.b)?.().c);
            assert.areEqual(42, a?.b?.().c);
            assert.areEqual(42, (a?.b)?.().c);

            const deep = { inner: { v: 3, m() { return this.v; } } };
            assert.areEqual(3, deep?.inner.m());
            assert.areEqual(3, deep?.inner.m?.());
            assert.areEqual(undefined, deep?.missing?.());
        }
    },
    {
        name: "Member getter runs once for '?.()'",
        body() {
            let count = 0;
            const holder = {};
            Object.defineProperty(holder, "f", {
                get() { count++; return function () { return this === holder; }; }
            });
            assert.isTrue(holder.f?.());
            assert.areEqual(1, count);
        }
    },
    {
        name: "delete with an optional chain",
        body() {
            const d = { x: 1, y: 2 };
            assert.isTrue(delete d?.x);
            assert.isFalse("x" in d);
            assert.isTrue(delete d?.["y"]);
            assert.isFalse("y" in d);
            assert.isTrue(delete null?.x);
            assert.isTrue(delete undefined?.x.y);
        }
    },
    {
        name: "Early errors for optional chains",
        body() {
            assert.isFalse(parses("var a; a?.b = 1"));
            assert.isFalse(parses("var a; a?.b += 1"));
            assert.isFalse(parses("var a; a?.b++"));
            assert.isFalse(parses("var a; ++a?.b"));
            assert.isFalse(parses("var a; (a?.b) = 1"));
            assert.isFalse(parses("var a; a?.b.c = 1"));
            assert.isTrue(parses("var a; (a?.b).c = 1"));
            assert.isFalse(parses("var a; for (a?.b in {}) {}"));
            assert.isFalse(parses("var a; for (a?.b of []) {}"));
            assert.isFalse(parses("var a; ({x: a?.b} = {});"));
            assert.isFalse(parses("var a; [a?.b] = [];"));
            assert.isFalse(parses("var a; new a?.b()"));
            assert.isFalse(parses("var a; new a?.()"));
            assert.isTrue(parses("var a; new (a?.b)()"));
            assert.isFalse(parses("var a; a?.b`x`"));
            assert.isFalse(parses("class B { m() { return super?.x; } }"));
            assert.isTrue(parses("class B { m() { return super.x?.y; } }"));
        }
    },
    {
        name: "Nested member optional calls keep 'this' at every level",
        body() {
            // Regression: the first '?.()' temp for 'this' lands in the
            // base of the next chain segment; it must still get a register
            // and be filled with the member base (debug builds asserted
            // with a bad register, release aborted).
            const o = { x: 5, m() { return { y: this.x, g() { return this.y; } }; } };
            assert.areEqual(5, o.m?.().g?.());
            const k = "g";
            assert.areEqual(5, o.m?.()[k]?.());
            assert.areEqual(5, o.m?.().g());
            assert.areEqual(5, (o.m?.()).g?.());

            const deep = { m() { return { n() { return { p() { return 42; } }; } }; } };
            assert.areEqual(42, deep.m?.().n?.().p?.());

            const mid = { m() { return { c() { return { d() { return 11; } }; } }; } };
            assert.areEqual(11, mid.m?.().c().d?.());

            // Member optional calls nested in arguments of both levels.
            const h = { f() { return 10; }, g(x) { return x + 1; } };
            const o2 = { m(a, b) { return { g(c) { return a + b + c; } }; } };
            assert.areEqual(22, o2.m?.(h.f?.(), h.g?.(1)).g?.(h.f?.()));
        }
    },
    {
        name: "Nested member optional calls short-circuit per segment",
        body() {
            assert.areEqual(undefined, ({ m: null }).m?.().g?.());
            assert.areEqual(undefined, ({ m() { return { g: null }; } }).m?.().g?.());
            assert.areEqual(undefined, ({ m() { return null; } }).m?.()?.g?.());
            // The non-optional '.g' on a null call result still throws.
            assert.throws(() => ({ m() { return null; } }).m?.().g?.(), TypeError);
        }
    },
    {
        name: "Parenthesized chain calls repeat without stale call state",
        body() {
            // Regression: reference-mode emission registered an LdFld
            // call-cache entry for a register no call consumes; a second
            // parenthesized-chain call asserted on the stale entry.
            const a = { x: 4, b() { return this.x; } };
            assert.areEqual(4, (a?.b)?.());
            assert.areEqual(4, (a?.b)?.());
            assert.areEqual(4, (a?.b)());
            assert.areEqual(4, (a?.b)());

            const o = { b() { return { c() { return 6; } }; } };
            assert.areEqual(6, (o?.b)?.().c?.());
            assert.areEqual(6, (o?.b)?.().c?.());
        }
    },
    {
        name: "Super member optional calls",
        body() {
            class B {
                m() { return 7; }
                x() { return { y() { return 5; } }; }
            }
            class C extends B {
                callM() { return super.m?.(); }
                callNested() { return super.x?.().y?.(); }
            }
            assert.areEqual(7, new C().callM());
            assert.areEqual(5, new C().callNested());
        }
    },
    {
        name: "Computed member optional call binds 'this'",
        body() {
            const o = { v: 3, f() { return this.v; } };
            const k = "f";
            assert.areEqual(3, o[k]?.());
            assert.areEqual(undefined, o["missing"]?.());
            let count = 0;
            const key = () => { count++; return "f"; };
            assert.areEqual(3, o[key()]?.());
            assert.areEqual(1, count);
        }
    },
    {
        name: "Chains in operand positions",
        body() {
            assert.areEqual("undefined", typeof null?.b);
            assert.areEqual(3, { b: 2 }?.b + 1);
            assert.isTrue(!null?.b);
            assert.areEqual(9, { b: 0 }?.b || 9);
            assert.areEqual("dflt", null?.b ?? "dflt");
            assert.areEqual(20, { b: [10, 20] }?.b?.[1]);
        }
    },
    {
        name: "Private names in optional chains",
        body() {
            class C {
                #m = 'test262';
                c = this;
                static access(obj) { return obj?.#m; }
                via(obj) { return obj?.c.#m; }
                self() { return this?.#m; }
            }
            const c = new C();
            // '?. PrivateIdentifier' direct form
            assert.areEqual('test262', C.access(c));
            assert.areEqual(undefined, C.access(null));
            assert.areEqual(undefined, C.access(undefined));
            assert.throws(() => C.access({}), TypeError,
                "brand check still applies to non-nullish non-instances");
            // 'OptionalChain . PrivateIdentifier' continuation form
            assert.areEqual('test262', c.via(c));
            assert.areEqual(undefined, c.via(null));
            assert.throws(() => c.via({ c: {} }), TypeError,
                "continuation private access brand-checks the tail value");
            assert.areEqual('test262', c.self());
            // A private name in an optional chain is still not assignable.
            assert.isFalse(parses("class D { #x = 1; m(o) { o?.#x = 2; } }"),
                "assignment through ?. private reference is an early SyntaxError");
            assert.isFalse(parses("class D { #x = 1; m(o) { o?.c.#x = 2; } }"),
                "assignment through continuation private after ?. is an early SyntaxError");
            // Delete of a private reference is an early SyntaxError in all
            // forms. Assert via eval-or-call so deferred method bodies,
            // which reparse on first call, are covered the same way.
            const deletePrivate = (expr) => () => {
                const D = eval("(class { #x = 1; c = this; m(o) { return delete " + expr + "; } })");
                new D().m(new D());
            };
            assert.throws(deletePrivate("o?.#x"), SyntaxError,
                "delete of ?. private reference is an early SyntaxError");
            assert.throws(deletePrivate("o?.c.#x"), SyntaxError,
                "delete of continuation private after ?. is an early SyntaxError");
            assert.throws(deletePrivate("(o?.#x)"), SyntaxError,
                "delete of parenthesized ?. private reference is an early SyntaxError");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
