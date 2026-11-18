//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertEqual(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error(msg + " (expected: " + JSON.stringify(expected) + ", actual: " + JSON.stringify(actual) + ")");
    }
}

function assertThrowsTypeError(fn, msg) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        assert(e instanceof TypeError, msg + " (threw non-TypeError: " + e + ")");
    }
    assert(threw, msg + " (did not throw TypeError)");
}

// 1. Unparenthesized single-argument arrow functions (Parser SetSymRef null PidRefStack fix)
(function testSingleArgArrows() {
    const inc = x => x + 1;
    assertEqual(inc(41), 42, "single-arg arrow execution");
    assertEqual(inc.toString(), "x => x + 1", "single-arg arrow toString");

    const doubled = /* before */y /* a */ => /* b */ y * 2/* after */;
    assertEqual(doubled(21), 42, "commented single-arg arrow execution");
    assertEqual(doubled.toString(), "y /* a */ => /* b */ y * 2", "commented single-arg arrow toString");

    const curried = a => b => a + b;
    assertEqual(curried(10)(32), 42, "curried single-arg arrows");
    assertEqual(curried.toString(), "a => b => a + b", "curried arrow toString");

    const asyncArrow = async z => z + 1;
    assertEqual(asyncArrow.toString(), "async z => z + 1", "async single-arg arrow toString");

    const mapped = [1, 2, 3].map(p => p * 10);
    assertEqual(mapped.join(","), "10,20,30", "array.map single-arg arrow");

    const evalArrow = eval("q => q * 3");
    assertEqual(evalArrow(14), 42, "eval single-arg arrow execution");
    assertEqual(evalArrow.toString(), "q => q * 3", "eval single-arg arrow toString");
})();

// 2. Callable Proxy Function.prototype.toString (ES2025 20.2.3.5)
(function testCallableProxyToString() {
    const expectedNative = "function() {\n    [native code]\n}";

    const targets = [
        function namedFn(a, b) { return a + b; },
        () => 1,
        x => x,
        function* genFn() { yield 1; },
        async function asyncFn() { return 1; },
        async function* asyncGenFn() { yield 1; },
        class ProxyClassTarget {},
        (function boundTarget() {}).bind({}),
        ({ method() {} }).method
    ];

    for (let i = 0; i < targets.length; i++) {
        const p = new Proxy(targets[i], {
            apply() {
                throw new Error("apply trap must not be invoked by toString");
            }
        });
        assertEqual(Function.prototype.toString.call(p), expectedNative, "callable Proxy #" + i);
        const nested = new Proxy(p, {});
        assertEqual(Function.prototype.toString.call(nested), expectedNative, "nested callable Proxy #" + i);
    }

    const revocableCallable = Proxy.revocable(function secret() {}, {});
    assertEqual(Function.prototype.toString.call(revocableCallable.proxy), expectedNative, "revocable callable Proxy before revoke");
    revocableCallable.revoke();
    assertEqual(Function.prototype.toString.call(revocableCallable.proxy), expectedNative, "revoked callable Proxy still returns NativeFunction");

    assertThrowsTypeError(function () {
        Function.prototype.toString.call(new Proxy({}, {}));
    }, "non-callable Proxy throws TypeError");

    const revocableObj = Proxy.revocable({}, {});
    assertThrowsTypeError(function () {
        Function.prototype.toString.call(revocableObj.proxy);
    }, "non-callable revocable Proxy before revoke throws TypeError");
    revocableObj.revoke();
    assertThrowsTypeError(function () {
        Function.prototype.toString.call(revocableObj.proxy);
    }, "revoked non-callable Proxy throws TypeError");

    assertThrowsTypeError(function () { Function.prototype.toString.call({}); }, "plain object throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call(null); }, "null throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call(undefined); }, "undefined throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call(42); }, "number throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call("fn"); }, "string throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call(Symbol("s")); }, "symbol throws TypeError");
    assertThrowsTypeError(function () { Function.prototype.toString.call(1n); }, "bigint throws TypeError");

    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        const other = WScript.LoadScript("var fn = function cross() { return 7; }; var p = new Proxy(fn, {});", "samethread");
        assertEqual(Function.prototype.toString.call(other.fn), "function cross() { return 7; }", "cross-realm function toString");
        assertEqual(Function.prototype.toString.call(other.p), expectedNative, "cross-realm Proxy toString");
    }
})();

// 3. CreateDynamicFunction toString (ES2025 20.2.1.1.1)
(function testDynamicFunctionsToString() {
    const GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor;
    const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
    const AsyncGeneratorFunction = Object.getPrototypeOf(async function* () {}).constructor;

    assertEqual(Function().toString(), "function anonymous(\n) {\n\n}", "empty Function()");
    assertEqual(Function("return 1;").toString(), "function anonymous(\n) {\nreturn 1;\n}", "body-only Function");
    assertEqual(
        Function("a", " /* a */ b, c /* b */ //", "/* c */ ; /* d */ //").toString(),
        "function anonymous(a, /* a */ b, c /* b */ //\n) {\n/* c */ ; /* d */ //\n}",
        "Function with comments and line comments"
    );

    assertEqual(GeneratorFunction().toString(), "function* anonymous(\n) {\n\n}", "empty GeneratorFunction()");
    assertEqual(
        GeneratorFunction("a", " /* a */ b, c /* b */ //", "/* c */ yield yield; /* d */ //").toString(),
        "function* anonymous(a, /* a */ b, c /* b */ //\n) {\n/* c */ yield yield; /* d */ //\n}",
        "GeneratorFunction with comments"
    );

    assertEqual(AsyncFunction().toString(), "async function anonymous(\n) {\n\n}", "empty AsyncFunction()");
    assertEqual(
        AsyncFunction("a", " /* a */ b, c /* b */ //", "/* c */ ; /* d */ //").toString(),
        "async function anonymous(a, /* a */ b, c /* b */ //\n) {\n/* c */ ; /* d */ //\n}",
        "AsyncFunction with comments"
    );

    assertEqual(AsyncGeneratorFunction().toString(), "async function* anonymous(\n) {\n\n}", "empty AsyncGeneratorFunction()");
    assertEqual(
        AsyncGeneratorFunction("a", " /* a */ b, c /* b */ //", "/* c */ ; /* d */ //").toString(),
        "async function* anonymous(a, /* a */ b, c /* b */ //\n) {\n/* c */ ; /* d */ //\n}",
        "AsyncGeneratorFunction with comments"
    );

    // Line-comment in body still compiles and executes because of leading \n after '{'
    const fLineComment = Function("x", "// leading comment\nreturn x + 1; // trailing");
    assertEqual(fLineComment(41), 42, "Function with line comments executes");

    // Stack frame line number inside Function body remains 1-based line 1 (numberLinesPrependedToAnonymousFunction == 2)
    try {
        Function("throw new Error('dyn-err');")();
        assert(false, "expected throw");
    } catch (e) {
        assert(e.stack.indexOf("Function code:1:1") !== -1, "dynamic Function body line/col 1:1, got: " + e.stack);
    }
})();

// 4. Class get/set accessors with comments, computed names, static, and private
(function testClassAccessorToString() {
    const k = "dyn";

    class CDecl {
        /* before */get /* a */ f /* b */ ( /* c */ ) /* d */ { /* e */ return 1; }/* after */
        /* before */set /* a */ f /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }/* after */
        /* before */get /* a */ [ /* b */ "g" /* c */ ] /* d */ ( /* e */ ) /* f */ { /* g */ return 2; }/* after */
        /* before */set /* a */ [ /* b */ k /* c */ ] /* d */ ( /* e */ v /* f */ ) /* g */ { /* h */ }/* after */
        static /* before */get /* a */ sf /* b */ ( /* c */ ) /* d */ { /* e */ return 3; }/* after */
        static /* before */set /* a */ sf /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }/* after */
        static /* before */get /* a */ [ /* b */ k /* c */ ] /* d */ ( /* e */ ) /* f */ { /* g */ return 4; }/* after */
        static /* before */set /* a */ [ /* b */ "sg" /* c */ ] /* d */ ( /* e */ v /* f */ ) /* g */ { /* h */ }/* after */
        /* before */get /* a */ #pf /* b */ ( /* c */ ) /* d */ { /* e */ return 5; }/* after */
        /* before */set /* a */ #pf /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }/* after */
        /* before */async /* a */ * /* b */ #pm /* c */ ( /* d */ ) /* e */ { /* f */ }/* after */
        static inspectPrivate() {
            class Priv {
                /* before */get /* a */ #x /* b */ ( /* c */ ) /* d */ { /* e */ return 9; }/* after */
                /* before */set /* a */ #x /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }/* after */
                /* before */async /* a */ * /* b */ #m /* c */ ( /* d */ ) /* e */ { /* f */ }/* after */
                static check() {
                    // Verify private accessors and async generator private methods
                    const p = new Priv();
                    p.#x = 1;
                    return { val: p.#x, mStr: p.#m.toString() };
                }
            }
            return Priv.check();
        }
    }

    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl.prototype, "f").get.toString(),
        "get /* a */ f /* b */ ( /* c */ ) /* d */ { /* e */ return 1; }",
        "class decl instance getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl.prototype, "f").set.toString(),
        "set /* a */ f /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }",
        "class decl instance setter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl.prototype, "g").get.toString(),
        "get /* a */ [ /* b */ \"g\" /* c */ ] /* d */ ( /* e */ ) /* f */ { /* g */ return 2; }",
        "class decl computed getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl.prototype, k).set.toString(),
        "set /* a */ [ /* b */ k /* c */ ] /* d */ ( /* e */ v /* f */ ) /* g */ { /* h */ }",
        "class decl dynamic computed setter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl, "sf").get.toString(),
        "get /* a */ sf /* b */ ( /* c */ ) /* d */ { /* e */ return 3; }",
        "class decl static getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl, "sf").set.toString(),
        "set /* a */ sf /* b */ ( /* c */ v /* d */ ) /* e */ { /* f */ }",
        "class decl static setter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl, k).get.toString(),
        "get /* a */ [ /* b */ k /* c */ ] /* d */ ( /* e */ ) /* f */ { /* g */ return 4; }",
        "class decl static computed getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl, "sg").set.toString(),
        "set /* a */ [ /* b */ \"sg\" /* c */ ] /* d */ ( /* e */ v /* f */ ) /* g */ { /* h */ }",
        "class decl static computed setter"
    );
    const privRes = CDecl.inspectPrivate();
    assertEqual(privRes.val, 9, "private accessor execution");
    assertEqual(
        privRes.mStr,
        "async /* a */ * /* b */ #m /* c */ ( /* d */ ) /* e */ { /* f */ }",
        "private async * method toString"
    );

    const CExpr = class {
        /* before */get /* a */ f /* b */ ( /* c */ ) /* d */ { /* e */ }/* after */
        /* before */set /* a */ f /* b */ ( /* c */ a /* d */ ) /* e */ { /* f */ }/* after */
        static /* before */get /* a */ s /* b */ ( /* c */ ) /* d */ { /* e */ }/* after */
        static /* before */set /* a */ s /* b */ ( /* c */ a /* d */ ) /* e */ { /* f */ }/* after */
    };
    assertEqual(
        Object.getOwnPropertyDescriptor(CExpr.prototype, "f").get.toString(),
        "get /* a */ f /* b */ ( /* c */ ) /* d */ { /* e */ }",
        "class expr getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CExpr.prototype, "f").set.toString(),
        "set /* a */ f /* b */ ( /* c */ a /* d */ ) /* e */ { /* f */ }",
        "class expr setter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CExpr, "s").get.toString(),
        "get /* a */ s /* b */ ( /* c */ ) /* d */ { /* e */ }",
        "class expr static getter"
    );
    assertEqual(
        Object.getOwnPropertyDescriptor(CExpr, "s").set.toString(),
        "set /* a */ s /* b */ ( /* c */ a /* d */ ) /* e */ { /* f */ }",
        "class expr static setter"
    );

    // Invoke after toString (forces undefer if deferred) and check toString again
    const inst = new CDecl();
    assertEqual(inst.f, 1, "call instance getter");
    assertEqual(CDecl.sf, 3, "call static getter");
    assertEqual(
        Object.getOwnPropertyDescriptor(CDecl.prototype, "f").get.toString(),
        "get /* a */ f /* b */ ( /* c */ ) /* d */ { /* e */ return 1; }",
        "class getter toString after invocation"
    );
})();

// 5. Object literal async * method (and generator / class async * comparison)
(function testObjectAsyncGeneratorMethodToString() {
    const x = "h";
    const obj = {
        /* before */async /* a */ * /* b */ f /* c */ ( /* d */ ) /* e */ { /* f */ }/* after */,
        /* before */async /* a */ * /* b */ [ /* c */ "g" /* d */ ] /* e */ ( /* f */ ) /* g */ { /* h */ }/* after */,
        /* before */async /* a */ * /* b */ [ /* c */ x /* d */ ] /* e */ ( /* f */ ) /* g */ { /* h */ }/* after */,
        /* before */* /* a */ gen /* b */ ( /* c */ ) /* d */ { /* e */ }/* after */
    };

    assertEqual(
        obj.f.toString(),
        "async /* a */ * /* b */ f /* c */ ( /* d */ ) /* e */ { /* f */ }",
        "object async * method"
    );
    assertEqual(
        obj.g.toString(),
        "async /* a */ * /* b */ [ /* c */ \"g\" /* d */ ] /* e */ ( /* f */ ) /* g */ { /* h */ }",
        "object computed string async * method"
    );
    assertEqual(
        obj.h.toString(),
        "async /* a */ * /* b */ [ /* c */ x /* d */ ] /* e */ ( /* f */ ) /* g */ { /* h */ }",
        "object computed expr async * method"
    );
    assertEqual(
        obj.gen.toString(),
        "* /* a */ gen /* b */ ( /* c */ ) /* d */ { /* e */ }",
        "object sync * method"
    );

    class CAsyncGen {
        /* before */async /* a */ * /* b */ m /* c */ ( /* d */ ) /* e */ { /* f */ }/* after */
        static /* before */async /* a */ * /* b */ sm /* c */ ( /* d */ ) /* e */ { /* f */ }/* after */
    }
    assertEqual(
        CAsyncGen.prototype.m.toString(),
        "async /* a */ * /* b */ m /* c */ ( /* d */ ) /* e */ { /* f */ }",
        "class instance async * method"
    );
    assertEqual(
        CAsyncGen.sm.toString(),
        "async /* a */ * /* b */ sm /* c */ ( /* d */ ) /* e */ { /* f */ }",
        "class static async * method"
    );
})();

if (typeof CollectGarbage === "function") {
    CollectGarbage();
}

print("PASS");
