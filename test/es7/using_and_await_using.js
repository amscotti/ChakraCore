//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(message) {
    print("using_and_await_using: FAIL: " + message);
    if (typeof WScript !== "undefined" && typeof WScript.Quit === "function") {
        WScript.Quit(1);
    }
    throw new Error(message);
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

function assertArrayEquals(actual, expected, message) {
    assertSame(actual.length, expected.length, message + " (length)");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], message + " [" + i + "]");
    }
}

function assertThrows(ctor, fn, message) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (ctor && !(e instanceof ctor)) {
            fail(message + " (expected " + ctor.name + ", got " + e + ")");
        }
    }
    if (!threw) {
        fail(message + " (did not throw)");
    }
}

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
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

async function expectReject(promise, predicate, message) {
    try {
        var v = await promise;
        fail(message + " (resolved with " + String(v) + ")");
    } catch (err) {
        if (typeof predicate === "function" && (predicate === Error || predicate.prototype instanceof Error)) {
            assertTrue(err instanceof predicate, message + " (expected " + predicate.name + ", got " + err + ")");
        } else if (typeof predicate === "function") {
            assertTrue(predicate(err), message + " (unexpected error: " + err + ")");
        } else {
            assertSame(err, predicate, message);
        }
        return err;
    }
}

// 1. Sync using: LIFO disposal, null/undefined, nested scopes, completion value, early returns/breaks/continues.
(function testSyncUsingBasics() {
    var log = [];
    function res(name) {
        return {
            [Symbol.dispose]: function () {
                log.push(name);
            }
        };
    }

    {
        using a = res("a");
        using b = null;
        using c = undefined;
        using d = res("d");
        log.push("body");
    }
    assertArrayEquals(log, ["body", "d", "a"], "block LIFO order");

    log = [];
    {
        using outer = res("outer");
        {
            using inner = res("inner");
            log.push("inner");
        }
        log.push("after-inner");
    }
    assertArrayEquals(log, ["inner", "inner", "after-inner", "outer"], "nested block LIFO");

    log = [];
    var retVal = (function () {
        if (false) {
            return 1;
        }
        using r = res("ret");
        return 42;
    })();
    assertSame(retVal, 42, "return preserves value across using");
    assertArrayEquals(log, ["ret"], "return in function body disposes");

    // Return before lexical position of using inside block still works cleanly.
    log = [];
    var earlyRet = (function (cond) {
        {
            if (cond) {
                return 7;
            }
            using r = res("never");
        }
        return 9;
    })(true);
    assertSame(earlyRet, 7, "early return before using declaration");
    assertArrayEquals(log, [], "unreached using not registered");

    // Labeled break out of nested block with using
    log = [];
    outerLabel: {
        using a = res("a");
        {
            using b = res("b");
            break outerLabel;
        }
    }
    assertArrayEquals(log, ["b", "a"], "labeled break disposes inner then outer");

    // Completion value preservation in eval
    assertSame(eval("4; { using x = null; }"), 4, "eval completion keeps 4 across empty-completion using block");
    assertSame(eval("{ using x = null; }"), undefined, "eval block with only using returns undefined");
    assertSame(eval("4; { 5; using x = null; }"), 5, "eval block updates completion to 5");
})();

// 2. Sync using: error recording, subsequent initializer throwing, and SuppressedError chaining.
(function testSyncUsingErrors() {
    var log = [];
    assertThrows(Error, function () {
        using a = { [Symbol.dispose]: function () { log.push("a"); } };
        using b = (function () { throw new Error("init-fail"); })();
    }, "subsequent initializer throws");
    assertArrayEquals(log, ["a"], "earlier resource still disposed when later initializer throws");

    log = [];
    var e1 = new Error("body");
    var e2 = new Error("dispose-b");
    var e3 = new Error("dispose-a");
    var caught;
    try {
        using a = { [Symbol.dispose]: function () { log.push("a"); throw e3; } };
        using b = { [Symbol.dispose]: function () { log.push("b"); throw e2; } };
        throw e1;
    } catch (err) {
        caught = err;
    }
    assertArrayEquals(log, ["b", "a"], "both disposers ran despite throws");
    assertTrue(caught instanceof SuppressedError, "multiple errors produce SuppressedError");
    assertSame(caught.error, e3, "outermost SuppressedError.error is latest disposal error");
    assertTrue(caught.suppressed instanceof SuppressedError, "suppressed is SuppressedError");
    assertSame(caught.suppressed.error, e2, "middle error is dispose-b");
    assertSame(caught.suppressed.suppressed, e1, "innermost suppressed error is body");

    // Non-object / missing Symbol.dispose throws TypeError before initializing subsequent bindings.
    assertThrows(TypeError, function () {
        using bad = 123;
    }, "primitive resource throws TypeError");
    assertThrows(TypeError, function () {
        using missing = {};
    }, "object without Symbol.dispose throws TypeError");
})();

// 3. Sync using in for(;;), for-of, and generators.
(function testSyncForAndGenerators() {
    var log = [];
    var count = 0;
    for (using head = { [Symbol.dispose]: function () { log.push("head"); } }; count < 3;) {
        count++;
        if (count === 2) {
            log.push("continue");
            continue;
        }
        log.push("iter:" + count);
    }
    assertArrayEquals(log, ["iter:1", "continue", "iter:3", "head"], "for(using;;) disposes once after loop, not on continue");

    log = [];
    for (using head = { [Symbol.dispose]: function () { log.push("head-break"); } };;) {
        log.push("body");
        break;
    }
    assertArrayEquals(log, ["body", "head-break"], "for(using;;) disposes on break");

    // for-of fresh binding per iteration, disposed per iteration, disposed before iterator.return() on break
    log = [];
    var closures = [];
    var iterClosed = false;
    var items = [
        { id: 1, [Symbol.dispose]: function () { log.push("d1"); } },
        { id: 2, [Symbol.dispose]: function () { log.push("d2"); } },
        { id: 3, [Symbol.dispose]: function () { log.push("d3"); } }
    ];
    var iterable = {
        idx: 0,
        [Symbol.iterator]: function () { return this; },
        next: function () {
            if (this.idx < items.length) {
                return { value: items[this.idx++], done: false };
            }
            return { done: true };
        },
        return: function () {
            iterClosed = true;
            log.push("iter.return");
            return { done: true };
        }
    };
    for (using item of iterable) {
        closures.push(function () { return item.id; });
        log.push("b" + item.id);
        if (item.id === 1) {
            continue;
        }
        if (item.id === 2) {
            break;
        }
    }
    assertArrayEquals(log, ["b1", "d1", "b2", "d2", "iter.return"], "for-of per-iteration disposal before iterator.return");
    assertTrue(iterClosed, "iterator.return was invoked");
    assertSame(closures[0](), 1, "fresh binding iteration 0");
    assertSame(closures[1](), 2, "fresh binding iteration 1");

    // Generator with yield inside sync using
    log = [];
    function* genFn() {
        using g = { [Symbol.dispose]: function () { log.push("gen-disposed"); } };
        yield 10;
        yield 20;
    }
    var g = genFn();
    assertSame(g.next().value, 10, "gen yield 1");
    assertArrayEquals(log, [], "not disposed while suspended");
    assertSame(g.return(99).value, 99, "gen.return value preserved");
    assertArrayEquals(log, ["gen-disposed"], "gen.return triggers sync disposal");
})();

// 4. Contextual grammar, TDZ, immutability, function name inference, early syntax errors.
(function testGrammarTDZAndEarlyErrors() {
    var using = [10, 20], x = 1;
    assertSame((using[x] = 99), 99, "using[x] element access works");
    assertSame(using[1], 99, "using[1] updated");

    {
        var y = 0;
        {
            using
            y = 5;
        }
        assertSame(y, 5, "using followed by newline is identifier + ASI");
    }

    var foo = 2, g = 5;
    assertSame((using = 20, using / foo / g), 2, "using/foo/g divides");

    {
        using using = null;
        assertSame(using, null, "using binding identifier named 'using'");
    }

    eval("var using, of = [[9], [8], [7]], result = []; for (using of of [0, 1, 2]) { result.push(using); } assertSame(result.length, 1); assertSame(result[0], 7);");
    for (using of = null;;) {
        assertSame(of, null, "for (using of = null;;) declares of");
        break;
    }

    {
        using \u0078 = null;
        assertSame(x, null, "escaped binding identifier allowed");
    }

    assertThrows(ReferenceError, function () {
        using self = self;
    }, "TDZ in initializer");

    assertThrows(ReferenceError, function () {
        var read = function () { return later; };
        read();
        using later = null;
    }, "TDZ in closure");

    assertThrows(ReferenceError, function () {
        let loopVar = null;
        for (using loopVar of [loopVar]) {}
    }, "TDZ in for-of RHS");

    assertThrows(TypeError, function () {
        using immutable = null;
        immutable = {};
    }, "reassignment to using binding throws TypeError");

    assertThrows(TypeError, function () {
        for (using k = null; k === null; k = {}) {}
    }, "reassignment in for-next throws TypeError");

    // Function and class name inference
    {
        using cls = class { static [Symbol.dispose]() {} };
        assertSame(cls.name, "cls", "using class name inference");
    }
    var origDisp = Function.prototype[Symbol.dispose];
    Function.prototype[Symbol.dispose] = function () {};
    try {
        using arrowFn = () => {};
        assertSame(arrowFn.name, "arrowFn", "using arrow name inference");
        using coverFn = (function () {});
        assertSame(coverFn.name, "coverFn", "using cover fn name inference");
    } finally {
        if (origDisp === undefined) {
            delete Function.prototype[Symbol.dispose];
        } else {
            Function.prototype[Symbol.dispose] = origDisp;
        }
    }

    // Syntax errors via eval / Function
    var syntaxErrorCases = [
        "using x = null;",
        "{ using x; }",
        "{ using x = null, y; }",
        "{ using [] = null; }",
        "{ using {} = null; }",
        "{ using x = null, [] = null; }",
        "{ using let = null; }",
        "{ 'use strict'; using let = null; }",
        "{ \\u0075sing x = null; }",
        "for (using x in {}) {}",
        "for (using x = null of []) {}",
        "switch (0) { case 0: using x = null; }",
        "switch (0) { default: using x = null; }",
        "if (true) using x = null;",
        "if (true) {} else using x = null;",
        "while (false) using x = null;",
        "do using x = null; while (false)",
        "{ label: using x = null; }",
        "function f() { await using x = null; }",
        "async function f() { await using x; }",
        "async function f() { await using [] = null; }",
        "async function f() { await using {} = null; }",
        "async function f() { for (await using x in {}) {} }",
        "async function f() { for (await using x = null of []) {} }",
        "async function f() { switch (0) { case 0: await using x = null; } }",
        "async function f() { label: await using x = null; }",
        "async function f() { if (true) await using x = null; }",
        "class C { static { await using x = null; } }",
        "class C { static { using await = null; } }",
        "{ using x = null; var x; }",
        "for (using x of []) { var x; }"
    ];
    for (var i = 0; i < syntaxErrorCases.length; i++) {
        assertThrows(SyntaxError, function () {
            eval(syntaxErrorCases[i]);
        }, "syntaxErrorCases[" + i + "]: " + syntaxErrorCases[i]);
    }

    // Allowed placements inside eval / static blocks
    eval("{ using ok = null; }");
    eval("switch (0) { case 0: { using ok = null; } }");
    eval("class C { static { using ok = null; } }");
    eval("class D { static { (() => { using await = null; }); } }");
})();

// 5. Async tests for await using, mixed using + await using, async generators, GC pressure.
addAsyncTest("await using lifecycle, order, null/undefined tick, mixed sync+async, errors", async function () {
    var log = [];
    async function runBlock() {
        await using a = {
            [Symbol.asyncDispose]: function () {
                log.push("a-start");
                return Promise.resolve().then(function () { log.push("a-end"); });
            }
        };
        using s = {
            [Symbol.dispose]: function () {
                log.push("sync-s");
            }
        };
        await using b = {
            [Symbol.dispose]: function () {
                log.push("b-fallback-dispose");
            }
        };
        log.push("body");
    }
    await runBlock();
    assertArrayEquals(log, ["body", "b-fallback-dispose", "sync-s", "a-start", "a-end"], "mixed async+sync LIFO");

    // Null/undefined await using implies one await tick if evaluated, zero extra ticks if unreached
    log = [];
    async function unreached() {
        log.push("before-throw");
        throw new Error("early");
        await using u = null;
    }
    var pUnreached = expectReject(unreached(), Error, "early throw before await using");
    Promise.resolve().then(function () { log.push("tick1"); });
    await pUnreached;
    // Let tick1 run if not already run:
    await Promise.resolve();
    assertArrayEquals(log, ["before-throw", "tick1"], "unreached await using adds no extra await");

    // Error during await disposal + body error + multiple rejections + GC stress
    var errBody = new Error("async-body");
    var errDisp1 = new Error("async-disp1");
    var errDisp2 = new Error("async-disp2");
    var asyncCaught = await expectReject((async function () {
        await using r1 = {
            [Symbol.asyncDispose]: function () {
                gcNow();
                return Promise.reject(errDisp1);
            }
        };
        await using r2 = {
            [Symbol.dispose]: function () {
                gcNow();
                throw errDisp2;
            }
        };
        throw errBody;
    })(), SuppressedError, "multiple errors during await using produce SuppressedError");
    assertSame(asyncCaught.error, errDisp1, "outer error is r1 async rejection");
    assertTrue(asyncCaught.suppressed instanceof SuppressedError, "middle is SuppressedError");
    assertSame(asyncCaught.suppressed.error, errDisp2, "middle error is r2");
    assertSame(asyncCaught.suppressed.suppressed, errBody, "innermost is body error");
});

addAsyncTest("await using in async generator, for-of, for-await-of, and contextual await using", async function () {
    var using = [123];
    assertSame(await using[0], 123, "await using[0] remains AwaitExpression");

    var log = [];
    async function* ag() {
        await using x = {
            async [Symbol.asyncDispose]() {
                gcNow();
                await Promise.resolve();
                log.push("ag-disposed");
            }
        };
        yield 1;
        yield 2;
    }
    var gen = ag();
    assertSame((await gen.next()).value, 1, "ag first yield");
    assertArrayEquals(log, [], "ag not disposed while paused");
    assertSame((await gen.return(88)).value, 88, "ag.return returns 88");
    assertArrayEquals(log, ["ag-disposed"], "ag.return disposes await using");

    log = [];
    for await (await using item of [
        { async [Symbol.asyncDispose]() { await Promise.resolve(); log.push("d1"); } },
        { [Symbol.dispose]() { log.push("d2"); } }
    ]) {
        log.push("iter");
        if (log.length === 1) continue;
        log.push("after-continue");
    }
    assertArrayEquals(log, ["iter", "d1", "iter", "after-continue", "d2"], "for-await-of await using disposes each iteration");

    for (await using of of []) {
        fail("should not iterate");
    }
});

Promise.all(asyncTests).then(
    function () {
        print("pass");
    },
    function (err) {
        fail("unhandled runner error: " + (err && err.stack ? err.stack : err));
    }
);
