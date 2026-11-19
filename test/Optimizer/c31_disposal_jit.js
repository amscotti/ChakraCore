//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// C3.1 Explicit Resource Management JIT probes: exercises DisposableStack,
// AsyncDisposableStack, SuppressedError, Error.isError, and
// Iterator.prototype[Symbol.dispose] across SimpleJIT, FullJIT,
// LoopBody JIT, DoOptimizeTry try/catch/finally regions, implicit-call field
// bailouts, and GC pressure.

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertEqual(actual, expected, msg) {
    if (actual !== expected) {
        throw new Error("Assertion failed (" + msg + "): expected " + String(expected) + ", got " + String(actual));
    }
}

function assertThrows(fn, ErrorCtor, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (ErrorCtor && !(e instanceof ErrorCtor)) {
            throw new Error("Assertion failed (" + msg + "): expected " + ErrorCtor.name + ", got " + e);
        }
        return e;
    }
    throw new Error("Assertion failed (" + msg + "): expected throw");
}

function gc() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// 1. Warmed DisposableStack LIFO, use/adopt/defer/move/disposed, null/undefined handling
function testSyncStackWarm() {
    function runOne(iter) {
        var order = [];
        var stack = new DisposableStack();
        assertEqual(stack.disposed, false, "initially not disposed");

        assertEqual(stack.use(null), null, "use(null)");
        assertEqual(stack.use(undefined), undefined, "use(undefined)");

        var r1 = {
            id: iter,
            [Symbol.dispose]: function () {
                order.push("r1:" + this.id);
            }
        };
        assertEqual(stack.use(r1), r1, "use returns resource");
        assertEqual(stack.adopt(10 + iter, function (v) { order.push("adopt:" + v); }), 10 + iter, "adopt returns value");
        stack.defer(function () {
            order.push("defer");
            // Re-entrant dispose must be a no-op once disposal begins.
            assertEqual(stack.disposed, true, "disposed immediately upon dispose()");
            stack.dispose();
            assertThrows(function () { stack.defer(function () {}); }, ReferenceError, "defer while disposing");
        });

        var moved = stack.move();
        assertEqual(stack.disposed, true, "original stack disposed after move");
        assertEqual(moved.disposed, false, "moved stack not disposed");
        assertThrows(function () { stack.use(null); }, ReferenceError, "use after move");
        assertThrows(function () { stack.move(); }, ReferenceError, "move after move");

        moved.dispose();
        assertEqual(moved.disposed, true, "moved stack disposed after dispose");
        moved.dispose(); // idempotent

        assertEqual(order.join(","), "defer,adopt:" + (10 + iter) + ",r1:" + iter, "LIFO order");
        return order.length;
    }

    var total = 0;
    for (var i = 0; i < 600; i++) {
        total += runOne(i);
        if (i === 250 || i === 550) {
            gc();
        }
    }
    assertEqual(total, 1800, "sync stack warm total");
}

// 2. SuppressedError construction, Error.isError, and nested suppression in JIT try/catch/finally
function testSuppressedErrorAndTryRegions() {
    function runSuppressed(i) {
        var stack = new DisposableStack();
        var finallyRan = 0;
        var caught = null;
        stack.defer(function () { throw new Error("first-" + i); });
        stack.defer(function () { throw new TypeError("second-" + i); });
        stack.defer(function () { throw undefined; });

        try {
            try {
                stack.dispose();
            } catch (e) {
                caught = e;
                throw e;
            }
        } catch (outer) {
            assertEqual(outer, caught, "rethrown error preserved across nested try/catch");
        } finally {
            finallyRan++;
        }

        assertEqual(finallyRan, 1, "finally ran once");
        assert(caught instanceof SuppressedError, "outermost is SuppressedError");
        assert(Error.isError(caught), "Error.isError on SuppressedError");
        assert(caught.error instanceof Error, "last thrown first-" + i + " preserved");
        assertEqual(caught.error.message, "first-" + i, "first message");
        assert(caught.suppressed instanceof SuppressedError, "middle SuppressedError");
        assert(caught.suppressed.error instanceof TypeError, "second error preserved");
        assertEqual(caught.suppressed.error.message, "second-" + i, "second message");
        assertEqual(caught.suppressed.suppressed, undefined, "first thrown undefined preserved");
        return 1;
    }

    var sum = 0;
    for (var i = 0; i < 500; i++) {
        sum += runSuppressed(i);
    }
    assertEqual(sum, 500, "suppressed error warm total");
}

// 3. Implicit call / field copy prop / array specialization bailout interaction with disposers
function testImplicitCallAndBailout() {
    function disposeMutatesField(obj, arr, mode) {
        var xBefore = obj.x;
        var aBefore = arr[0];
        var stack = new DisposableStack();
        stack.defer(function () {
            if (mode === 1) {
                obj.x = xBefore + 100;
            } else if (mode === 2) {
                arr[0] = 1.5;
            } else if (mode === 3) {
                gc();
                obj.x = xBefore + 5;
            }
        });
        try {
            if (mode === 4) {
                return xBefore + aBefore;
            }
        } finally {
            stack.dispose();
        }
        return obj.x + arr[0] + xBefore + aBefore;
    }

    var obj = { x: 1 };
    var arr = [2, 3, 4];
    for (var i = 0; i < 400; i++) {
        obj.x = 1;
        arr[0] = 2;
        assertEqual(disposeMutatesField(obj, arr, 0), 6, "no mutation iteration " + i);
    }

    obj.x = 7;
    arr[0] = 2;
    assertEqual(disposeMutatesField(obj, arr, 1), 7 + 2 + 107 + 2, "field mutation bailout");

    obj.x = 3;
    arr = [2, 3, 4];
    assertEqual(disposeMutatesField(obj, arr, 2), 3 + 2 + 3 + 1.5, "array mutation bailout");

    obj.x = 9;
    arr = [4, 5, 6];
    assertEqual(disposeMutatesField(obj, arr, 3), 9 + 4 + 14 + 4, "gc + field mutation");

    obj.x = 11;
    arr = [5, 6];
    assertEqual(disposeMutatesField(obj, arr, 4), 16, "early return through finally");
}

// 4. Loop-body try/catch/finally early break/continue/return with disposal
function testLoopTryFinallyDisposal() {
    function runLoop(limit) {
        var disposedCount = 0;
        var acc = 0;
        for (var i = 0; i < limit; i++) {
            var s = new DisposableStack();
            s.defer(function () { disposedCount++; });
            try {
                if ((i & 3) === 1) {
                    acc += 10;
                    continue;
                }
                if (i === limit - 1) {
                    acc += 100;
                    break;
                }
                acc += 1;
            } finally {
                s.dispose();
            }
        }
        return disposedCount * 10000 + acc;
    }

    var r = 0;
    for (var k = 0; k < 200; k++) {
        r = runLoop(20);
    }
    assertEqual(r, 20 * 10000 + 5 * 10 + 14 + 100, "loop try/finally disposal");
}

// 5. Iterator.prototype[Symbol.dispose] and AsyncIteratorPrototype[Symbol.asyncDispose]
function testIteratorDisposeWarm() {
    function makeGen(closedLog) {
        function* g() {
            try {
                yield 1;
                yield 2;
            } finally {
                closedLog.push("closed");
            }
        }
        return g();
    }

    var closed = [];
    for (var i = 0; i < 400; i++) {
        var it = makeGen(closed);
        assertEqual(it.next().value, 1, "gen yield 1");
        it[Symbol.dispose]();
        assertEqual(it.next().done, true, "gen done after Symbol.dispose");
    }
    assertEqual(closed.length, 400, "all generators closed via Symbol.dispose");

    // No-op when return is missing or undefined
    var customIter = [1, 2, 3][Symbol.iterator]();
    assertEqual(customIter[Symbol.dispose](), undefined, "array iterator dispose");
}

// 6. AsyncDisposableStack warm JIT + async settlement + GC
function testAsyncStackWarm() {
    var tasks = [];
    function runAsyncBatch(id) {
        var events = [];
        var stack = new AsyncDisposableStack();
        stack.use({
            [Symbol.asyncDispose]: function () {
                events.push("async1:" + id);
                return Promise.resolve().then(function () {
                    if ((id % 50) === 0) {
                        gc();
                    }
                    events.push("async1-settled:" + id);
                });
            }
        });
        // Sync fallback in AsyncDisposableStack must NOT await returned promise
        var unresolved = new Promise(function () {});
        stack.use({
            [Symbol.dispose]: function () {
                events.push("sync-fallback:" + id);
                return unresolved;
            }
        });
        stack.adopt("val-" + id, function (v) {
            events.push("adopt:" + v);
        });
        stack.defer(function () {
            events.push("defer:" + id);
        });

        return stack.disposeAsync().then(function (res) {
            assertEqual(res, undefined, "disposeAsync resolves to undefined");
            assertEqual(stack.disposed, true, "async stack disposed");
            assertEqual(
                events.join("|"),
                "defer:" + id + "|adopt:val-" + id + "|sync-fallback:" + id + "|async1:" + id + "|async1-settled:" + id,
                "async stack LIFO order"
            );
        });
    }

    for (var i = 0; i < 5; i++) {
        tasks.push(runAsyncBatch(i));
    }

    // Async SuppressedError chaining
    var errStack = new AsyncDisposableStack();
    errStack.defer(function () { throw new Error("async-e1"); });
    errStack.defer(function () { return Promise.reject(new RangeError("async-e2")); });
    tasks.push(
        errStack.disposeAsync().then(
            function () { throw new Error("expected rejection"); },
            function (e) {
                assert(e instanceof SuppressedError, "async SuppressedError");
                assert(e.error instanceof Error && e.error.message === "async-e1", "async newest error");
                assert(e.suppressed instanceof RangeError && e.suppressed.message === "async-e2", "async suppressed error");
            }
        )
    );

    return Promise.all(tasks);
}

testSyncStackWarm();
testSuppressedErrorAndTryRegions();
testImplicitCallAndBailout();
testLoopTryFinallyDisposal();
testIteratorDisposeWarm();

testAsyncStackWarm().then(
    function () {
        if (typeof WScript !== "undefined" && WScript.Echo) {
            WScript.Echo("PASS");
        } else {
            print("PASS");
        }
    },
    function (err) {
        if (typeof WScript !== "undefined" && WScript.Echo) {
            WScript.Echo("FAIL: " + (err && err.stack ? err.stack : err));
        } else {
            print("FAIL: " + (err && err.stack ? err.stack : err));
        }
    }
);
