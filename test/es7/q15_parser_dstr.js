//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("q15_parser_dstr: FAIL: " + message);
    WScript.Quit(1);
}

function assertTrue(cond, message) {
    if (!cond) {
        fail(message);
    }
}

function assertSame(actual, expected, message) {
    if (!Object.is(actual, expected)) {
        fail(message + " (expected " + String(expected) + ", got " + String(actual) + ")");
    }
}

function assertThrows(ErrorCtor, fn, message) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof ErrorCtor)) {
            fail(message + " (threw " + e + " instead of " + ErrorCtor.name + ")");
        }
    }
    if (!threw) {
        fail(message + " (did not throw " + ErrorCtor.name + ")");
    }
}

function assertModuleSyntaxError(source, message) {
    var threw = false;
    try {
        WScript.LoadModule(source, "samethread");
    } catch (e) {
        threw = true;
        assertTrue(e && e.constructor && e.constructor.name === "SyntaxError", message + " (got " + e + ")");
    }
    if (!threw) {
        fail(message + " (did not throw SyntaxError)");
    }
}

var asyncTests = [];
function addAsyncTest(name, fn) {
    asyncTests.push(
        Promise.resolve()
            .then(fn)
            .catch(function (err) {
                fail(name + ": " + (err && err.stack ? err.stack : err));
            })
    );
}

// 1. Destructuring SetFunctionName & assignment target controls
(function testDestructuringSetFunctionName() {
    var fn, arrow, gen, cls, cover, coverCls, coverArrow, xFn, xCls, xCls2, xCover, xCoverArrow;
    ({
        fn = function () {},
        arrow = () => {},
        gen = function* () {},
        cls = class {},
        cover = (function () {}),
        coverCls = (class {}),
        coverArrow = (() => {}),
        xFn = function namedFn() {},
        xCls = class namedCls {},
        xCls2 = class { static name() {} },
        xCover = (0, function () {}),
        xCoverArrow = (0, () => {})
    } = {});

    assertSame(fn.name, "fn", "assign dstr fn.name");
    assertSame(arrow.name, "arrow", "assign dstr arrow.name");
    assertSame(gen.name, "gen", "assign dstr gen.name");
    assertSame(cls.name, "cls", "assign dstr cls.name");
    assertSame(cover.name, "cover", "assign dstr cover.name");
    assertSame(coverCls.name, "coverCls", "assign dstr coverCls.name");
    assertSame(coverArrow.name, "coverArrow", "assign dstr coverArrow.name");
    assertSame(xFn.name, "namedFn", "assign dstr named function preserves own name");
    assertSame(xCls.name, "namedCls", "assign dstr named class preserves own name");
    assertSame(typeof xCls2.name, "function", "assign dstr static name() preserved");
    assertSame(xCover.name, "", "assign dstr (0, fn) not named");
    assertSame(xCoverArrow.name, "", "assign dstr (0, arrow) not named");

    for (var i = 0; i < 1; i++) {
        ({
            fn = function () {},
            arrow = () => {},
            gen = function* () {},
            cls = class {},
            cover = (function () {})
        } = {});
    }
    assertSame(fn.name, "fn", "loop assign dstr fn.name");

    var loopCount = 0;
    for ({
        fn = function () {},
        arrow = () => {},
        gen = function* () {},
        cls = class {},
        cover = (function () {}),
        xCover = (0, function () {})
    } of [{}]) {
        loopCount++;
        assertSame(fn.name, "fn", "for-of dstr fn.name");
        assertSame(arrow.name, "arrow", "for-of dstr arrow.name");
        assertSame(gen.name, "gen", "for-of dstr gen.name");
        assertSame(cls.name, "cls", "for-of dstr cls.name");
        assertSame(cover.name, "cover", "for-of dstr cover.name");
        assertSame(xCover.name, "", "for-of dstr (0, fn) not named");
    }
    assertSame(loopCount, 1, "for-of loopCount");

    // CoverParenthesizedExpression LHS does NOT perform NamedEvaluation
    var pFn, pArrow, pCls, pCoverCls;
    (pFn) = function () {};
    (pArrow) = () => {};
    (pCls) = class {};
    (pCoverCls) = (class {});
    assertSame(pFn.name, "", "parenthesized LHS function");
    assertSame(pArrow.name, "", "parenthesized LHS arrow");
    assertSame(pCls.name, "", "parenthesized LHS class");
    assertSame(pCoverCls.name, "", "parenthesized LHS cover class");
})();

addAsyncTest("for-await-of destructuring SetFunctionName & async LHS", async function () {
    var fn, arrow, gen, cls, cover, xCover;
    var async;
    var hits = 0;
    for await ({
        fn = function () {},
        arrow = () => {},
        gen = function* () {},
        cls = class {},
        cover = (function () {}),
        xCover = (0, function () {})
    } of [{}]) {
        hits++;
        assertSame(fn.name, "fn", "async fn for-await-of fn.name");
        assertSame(arrow.name, "arrow", "async fn for-await-of arrow.name");
        assertSame(gen.name, "gen", "async fn for-await-of gen.name");
        assertSame(cls.name, "cls", "async fn for-await-of cls.name");
        assertSame(cover.name, "cover", "async fn for-await-of cover.name");
        assertSame(xCover.name, "", "async fn for-await-of xCover.name");
    }
    assertSame(hits, 1, "async fn for-await-of hits");

    async function* ag() {
        for await ({
            fn = function () {},
            arrow = () => {},
            gen = function* () {},
            cls = class {},
            cover = (function () {})
        } of [{}]) {
            yield [fn.name, arrow.name, gen.name, cls.name, cover.name];
        }
        var seen = [];
        for await (async of [7, 8]) {
            seen.push(async);
        }
        yield seen;
    }
    var g = ag();
    var r1 = await g.next();
    assertSame(r1.value.join(","), "fn,arrow,gen,cls,cover", "async gen for-await-of dstr names");
    var r2 = await g.next();
    assertSame(r2.value.join(","), "7,8", "for await (async of ...)");
    assertThrows(SyntaxError, function () {
        eval("for (async of [1]) {}");
    }, "non-await for (async of ...) remains SyntaxError");
});

// 2. __proto__ function naming and duplicate __proto__ in patterns vs object literals
(function testProtoNamingAndDuplicates() {
    var o1 = { __proto__: function () {} };
    var o2 = { __proto__: () => {} };
    var o3 = { __proto__: class {} };
    var o4 = { ["__proto__"]: function () {} };
    var o5 = { __proto__() {} };
    assertSame(Object.getPrototypeOf(o1).name, "", "__proto__: function has empty name");
    assertSame(Object.getPrototypeOf(o2).name, "", "__proto__: arrow has empty name");
    assertSame(Object.getPrototypeOf(o3).name, "", "__proto__: class has empty name");
    assertSame(o4.__proto__.name, "__proto__", "computed ['__proto__'] gets __proto__ name");
    assertSame(o5.__proto__.name, "__proto__", "concise __proto__ method gets __proto__ name");

    var a = 0, b = 0;
    ({ __proto__: a, __proto__: b } = { __proto__: { x: 42 } });
    assertSame(a.x, 42, "assign pattern dup __proto__ #1");
    assertSame(b.x, 42, "assign pattern dup __proto__ #2");

    var count = 0;
    for ({ __proto__: a, __proto__: b } of [{ __proto__: { x: 99 } }]) {
        count++;
    }
    assertSame(count, 1, "for-of dup __proto__ count");
    assertSame(a.x + b.x, 198, "for-of pattern dup __proto__");

    assertThrows(SyntaxError, function () { eval("({ __proto__: {}, __proto__: {} })"); }, "object literal dup __proto__");
    assertThrows(SyntaxError, function () { eval("({ __proto__: a, __proto__: b } = { __proto__: 1, __proto__: 2 })"); }, "RHS object literal dup __proto__");
    assertThrows(SyntaxError, function () { eval("([a = { __proto__: 1, __proto__: 2 }] = [])"); }, "dstr array default object literal dup __proto__");
    assertThrows(SyntaxError, function () { eval("({ a = { __proto__: 1, __proto__: 2 } } = {})"); }, "dstr obj default object literal dup __proto__");
    assertThrows(SyntaxError, function () { eval("(function(x = { __proto__: 1, __proto__: 2 }) {})"); }, "default param object literal dup __proto__");
    assertThrows(SyntaxError, function () { eval("for ({ __proto__: 1, __proto__: 2 }; false; ) {}"); }, "for header object literal dup __proto__");
})();

// 3. Object literal MemberExpression targets in array destructuring
(function testObjectLiteralPropertyRefTarget() {
    var getterRan = 0;
    var setterVal = 0;
    [{
        get y() {
            getterRan++;
            return undefined;
        },
        set y(v) {
            setterVal = v;
        }
    }.y = 42] = [];
    assertSame(getterRan, 0, "assign array elem obj literal prop ref default getter not called");
    assertSame(setterVal, 42, "assign array elem obj literal prop ref default setter called");

    setterVal = 0;
    [{ set y(v) { setterVal = v; } }.y] = [77];
    assertSame(setterVal, 77, "assign array elem obj literal prop ref setter called");

    setterVal = 0;
    for ([{ set y(v) { setterVal = v; } }.y = 55] of [[]]) {}
    assertSame(setterVal, 55, "for-of array elem obj literal prop ref default setter called");

    assertThrows(SyntaxError, function () {
        eval("[{}?.y] = [1]");
    }, "optional chain on object literal is invalid assignment target");
})();

// 4. Param scopes: arguments lexical shadow in split scope + legacy simple-param compat
(function testParamScopes() {
    var capturedArgs;
    function lexArgsFn(x = (capturedArgs = arguments)) {
        var beforeTdz = false;
        try {
            void arguments;
        } catch (e) {
            beforeTdz = e instanceof ReferenceError;
        }
        let arguments = 123;
        return [beforeTdz, arguments];
    }
    var res = lexArgsFn(undefined, "beta");
    assertSame(res[0], true, "body arguments is in TDZ before let");
    assertSame(res[1], 123, "body arguments bound by let");
    assertSame(capturedArgs.length, 2, "param arguments still bound to Arguments");
    assertSame(capturedArgs[1], "beta", "param arguments[1]");

    function* lexArgsGen(x = (capturedArgs = arguments)) {
        let arguments = "shadow";
        yield arguments;
    }
    assertSame(lexArgsGen(undefined, "g1").next().value, "shadow", "generator body let arguments");
    assertSame(capturedArgs[1], "g1", "generator param arguments");

    assertThrows(SyntaxError, function () {
        function simpleLexEval() {
            let arguments = 1;
            eval("var arguments");
        }
        simpleLexEval();
    }, "simple param body let arguments with eval var arguments throws SyntaxError");

    assertThrows(SyntaxError, function () {
        function splitLexEval(p = 1) {
            let arguments = 1;
            eval("var arguments");
        }
        splitLexEval();
    }, "split param body let arguments with eval var arguments throws SyntaxError");

    assertThrows(SyntaxError, function () {
        eval("function bad(arguments) { let arguments; }");
    }, "formal arguments redeclared by body let is SyntaxError");
})();

// 5. delete super[expr]
(function testDeleteSuper() {
    var key = {
        toString: function () {
            fail("ToPropertyKey must not run for delete super[key]");
        }
    };
    var obj = {
        m() {
            delete super[key];
        }
    };
    assertThrows(ReferenceError, function () {
        obj.m();
    }, "delete super[key] throws ReferenceError without ToPropertyKey");

    class Base {
        constructor() {
            fail("Base constructor must not be called when this is uninitialized");
        }
    }
    class Derived extends Base {
        constructor() {
            delete super[(super(), 0)];
        }
    }
    assertThrows(ReferenceError, function () {
        new Derived();
    }, "delete super[(super(), 0)] checks this binding before evaluating key");
})();

// 6. Tagged template invalid escape sequences
(function testTaggedTemplateInvalidEscapes() {
    function tag(strings) {
        return { cooked: strings[0], raw: strings.raw[0], len: strings.length };
    }
    var a = tag`\u`;
    var b = tag`\u0`;
    var c = tag`\u{0`;
    var d = tag`\u{110000}`;
    var e = tag`\x0`;
    var f = tag`\u${42}\u{`;
    assertSame(a.cooked, undefined, "tag \\u cooked undefined");
    assertSame(a.raw, "\\u", "tag \\u raw");
    assertSame(b.cooked, undefined, "tag \\u0 cooked undefined");
    assertSame(b.raw, "\\u0", "tag \\u0 raw");
    assertSame(c.cooked, undefined, "tag \\u{0 cooked undefined");
    assertSame(c.raw, "\\u{0", "tag \\u{0 raw");
    assertSame(d.cooked, undefined, "tag \\u{110000} cooked undefined");
    assertSame(d.raw, "\\u{110000}", "tag \\u{110000} raw");
    assertSame(e.cooked, undefined, "tag \\x0 cooked undefined");
    assertSame(e.raw, "\\x0", "tag \\x0 raw");
    assertSame(f.cooked, undefined, "tag \\u${42} cooked undefined");
    assertSame(f.raw, "\\u", "tag \\u${42} raw");

    assertThrows(SyntaxError, function () {
        eval("`\\u0`");
    }, "untagged invalid template escape throws SyntaxError");
})();

// 7. Module early new.target
(function testModuleSyntax() {
    assertModuleSyntaxError("new.target;", "top-level new.target in module is SyntaxError");
    WScript.LoadModule("export default function() {} var ok = 1;", "samethread");
})();

// 8. Annex B dynamic Function body first-line --> & catch redeclared for-in/of var
(function testAnnexB() {
    assertSame((new Function("--> body comment\nreturn 44;"))(), 44, "Function body line 1 --> allowed");
    assertThrows(SyntaxError, function () {
        new Function("-->", "");
    }, "Function params line 1 --> throws SyntaxError");
    assertThrows(SyntaxError, function () {
        eval("1 --> 0");
    }, "--> after token on line 1 is not comment");
    assertThrows(SyntaxError, function () {
        eval("/* */ --> comment");
    }, "eval /* */ --> preserves legacy HTMLComments.js contract");

    assertThrows(SyntaxError, function () {
        eval("'use strict'; f() = 1;");
    }, "strict mode f() = 1 is SyntaxError");

    function testCatchForInOf() {
        try {
            throw "initial";
        } catch (err) {
            for (var err in { k: 1 }) {
                assertSame(err, "k", "catch for-in mutates catch err");
            }
            for (var err of ["v"]) {
                assertSame(err, "v", "catch for-of mutates catch err");
            }
        }
        return err;
    }
    for (var j = 0; j < 200; j++) {
        assertSame(testCatchForInOf(), undefined, "function var err stays undefined outside catch");
    }
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
})();

Promise.all(asyncTests).then(
    function () {
        print("pass");
    },
    function (err) {
        fail("unhandled async test error: " + (err && err.stack ? err.stack : err));
    }
);
