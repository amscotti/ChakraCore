//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function fail(message) {
    print("async_from_sync_iterator_es2025: FAIL: " + message);
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
    assertTrue(promise instanceof Promise, message + " (must return a Promise)");
    try {
        var val = await promise;
        fail(message + " (resolved with " + String(val) + " instead of rejecting)");
    } catch (err) {
        if (typeof predicate === "function" && predicate.prototype instanceof Error || predicate === TypeError || predicate === Error) {
            assertTrue(err instanceof predicate, message + " (expected " + predicate.name + ", got " + String(err) + ")");
        } else if (typeof predicate === "function") {
            assertTrue(predicate(err), message + " (unexpected error: " + String(err) + ")");
        } else {
            assertSame(err, predicate, message);
        }
        return err;
    }
}

// 1. return(null) and return(undefined) and closeOnRejection=false on return()
addAsyncTest("return null/undefined and non-callable and closeOnRejection=false", async function () {
    var returnGets = 0;
    var iterations = 0;
    var syncIterNullReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        get return() {
            returnGets += 1;
            return null;
        }
    };

    for await (var x of syncIterNullReturn) {
        iterations += 1;
        assertSame(x, 1, "for-await yielded 1");
        break;
    }
    assertSame(iterations, 1, "for-await ran 1 iteration");
    assertSame(returnGets, 1, "return getter invoked once on break");

    var returnGets2 = 0;
    var syncIterNullReturn2 = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 2, done: false }; },
        get return() {
            returnGets2 += 1;
            return null;
        }
    };
    async function* agWrapNull() {
        return yield* syncIterNullReturn2;
    }
    var gNull = agWrapNull();
    var r1 = await gNull.next();
    assertSame(r1.value, 2, "yield* next value");
    assertSame(r1.done, false, "yield* next not done");
    var r2 = await gNull.return("early");
    assertSame(r2.value, "early", "return(null) resolves with passed value");
    assertSame(r2.done, true, "return(null) sets done: true");
    assertSame(returnGets2, 1, "return getter invoked once on yield* return");
    var r3 = await gNull.next();
    assertSame(r3.done, true, "generator done after return");
    assertSame(r3.value, undefined, "generator value undefined after return");

    // Non-callable return rejects with TypeError
    var syncIterBadReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: 42
    };
    await expectReject((async function () {
        for await (var _ of syncIterBadReturn) {
            break;
        }
    })(), TypeError, "non-callable return rejects with TypeError");

    // closeOnRejection is false for .return(): rejected valueWrapper must NOT re-call return()
    var returnCallsOnReturn = 0;
    var returnSentinel = { tag: "return-reject" };
    var syncIterReturnRejects = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: function () {
            returnCallsOnReturn += 1;
            return { value: Promise.reject(returnSentinel), done: false };
        }
    };
    async function* agReturnRejects() {
        yield* syncIterReturnRejects;
    }
    var gRetRej = agReturnRejects();
    await gRetRej.next();
    await expectReject(gRetRej.return("arg"), returnSentinel, "return() rejected value propagates");
    assertSame(returnCallsOnReturn, 1, "return() does not call return() again on rejection");

    // closeOnRejection is false for .return() even when PromiseResolve throws
    var returnCallsOnPoison = 0;
    var ctorErr = { tag: "return-ctor-throw" };
    var syncIterReturnPoison = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: function () {
            returnCallsOnPoison += 1;
            var p = Promise.resolve(1);
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            return { value: p, done: false };
        }
    };
    async function* agReturnPoison() {
        yield* syncIterReturnPoison;
    }
    var gRetPoison = agReturnPoison();
    await gRetPoison.next();
    await expectReject(gRetPoison.return("arg"), ctorErr, "return() poisoned wrapper propagates");
    assertSame(returnCallsOnPoison, 1, "return() does not re-close on poisoned wrapper");
});

// 2. throw() when throw is missing / undefined / null
addAsyncTest("throw() missing/undefined/null performs IteratorClose(NormalCompletion) then TypeError", async function () {
    // 2a. Missing throw and missing return -> TypeError (not undefined)
    var missingAll = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; }
    };
    async function* agMissingAll() { yield* missingAll; }
    var gMissing = agMissingAll();
    await gMissing.next();
    await expectReject(gMissing.throw("boom"), TypeError, "missing throw rejects with TypeError");
    var afterMissing = await gMissing.next();
    assertSame(afterMissing.done, true, "completed after throw missing");
    assertSame(afterMissing.value, undefined, "value undefined after throw missing");

    // 2b. throw: null with return getter returning null
    var throwGets = 0;
    var returnGets = 0;
    var nullThrowNullReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        get throw() { throwGets += 1; return null; },
        get return() { returnGets += 1; return null; }
    };
    async function* agNullBoth() { yield* nullThrowNullReturn; }
    var gNullBoth = agNullBoth();
    await gNullBoth.next();
    await expectReject(gNullBoth.throw("ignored"), TypeError, "null throw + null return rejects with TypeError");
    assertSame(throwGets, 1, "throw getter accessed once");
    assertSame(returnGets, 1, "return getter accessed once");

    // 2c. throw: undefined with return() returning an object (including rejected promise value ignored)
    var returnArgCounts = [];
    var undefinedThrowObjReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        throw: undefined,
        return: function () {
            returnArgCounts.push(arguments.length);
            var rejected = Promise.reject("must-be-ignored");
            rejected.catch(function () {});
            return { value: rejected, done: true };
        }
    };
    async function* agObjReturn() { yield* undefinedThrowObjReturn; }
    var gObjReturn = agObjReturn();
    await gObjReturn.next();
    await expectReject(gObjReturn.throw("err"), TypeError, "undefined throw + object return rejects with TypeError");
    assertSame(returnArgCounts.length, 1, "return() called once");
    assertSame(returnArgCounts[0], 0, "IteratorClose calls return() with 0 arguments");

    // 2d. throw: undefined with poisoned return getter -> poisoned error wins
    var poisonedErr = { tag: "poisoned-return-getter" };
    var poisonedReturnCalls = 0;
    var undefinedThrowPoisonedReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        get return() {
            poisonedReturnCalls += 1;
            throw poisonedErr;
        }
    };
    async function* agPoisonedReturn() { yield* undefinedThrowPoisonedReturn; }
    var gPoisoned = agPoisonedReturn();
    await gPoisoned.next();
    await expectReject(gPoisoned.throw("ignored"), poisonedErr, "poisoned return getter error wins over TypeError");
    assertSame(poisonedReturnCalls, 1, "poisoned return getter accessed once");

    // 2e. throw: undefined with return() throwing -> return()'s exception wins
    var returnThrowErr = { tag: "return-threw" };
    var undefinedThrowThrowingReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: function () {
            throw returnThrowErr;
        }
    };
    async function* agThrowingReturn() { yield* undefinedThrowThrowingReturn; }
    var gThrowRet = agThrowingReturn();
    await gThrowRet.next();
    await expectReject(gThrowRet.throw("ignored"), returnThrowErr, "throwing return() error wins");

    // 2f. throw: undefined with return() returning non-object -> TypeError
    var nonObjReturnCalls = 0;
    var undefinedThrowNonObjReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: function () {
            nonObjReturnCalls += 1;
            return 2;
        }
    };
    async function* agNonObjReturn() { yield* undefinedThrowNonObjReturn; }
    var gNonObj = agNonObjReturn();
    await gNonObj.next();
    await expectReject(gNonObj.throw("ignored"), TypeError, "non-object return() rejects with TypeError");
    assertSame(nonObjReturnCalls, 1, "return() called once on non-object return");

    // 2g. throw: undefined with non-callable return -> TypeError
    var undefinedThrowNonCallableReturn = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        return: 123
    };
    async function* agNonCallableReturn() { yield* undefinedThrowNonCallableReturn; }
    var gNonCallableRet = agNonCallableReturn();
    await gNonCallableRet.next();
    await expectReject(gNonCallableRet.throw("ignored"), TypeError, "non-callable return rejects with TypeError");
});

// 3. Poisoned Promise constructor / .then getter closes sync iterator on !done && closeOnRejection
addAsyncTest("poisoned Promise constructor and .then getter close sync iterator", async function () {
    // 3a. yield* next() with poisoned .constructor on yielded Promise
    var ctorErr = { tag: "ctor-err" };
    var finallyCount = 0;
    function* genBadCtor() {
        try {
            var p = Promise.resolve("x");
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            yield p;
        } finally {
            finallyCount += 1;
        }
    }
    async function* agBadCtor() { yield* genBadCtor(); }
    await expectReject(agBadCtor().next(), ctorErr, "yield* next() rejects with poisoned constructor error");
    assertSame(finallyCount, 1, "sync generator finally ran on poisoned constructor in yield*");

    // 3b. for-await-of with poisoned .constructor on yielded Promise
    var forAwaitReturnCount = 0;
    var syncIterForAwait = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            var p = Promise.resolve("x");
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            return { value: p, done: false };
        },
        return: function () {
            forAwaitReturnCount += 1;
            return { done: true };
        }
    };
    await expectReject((async function () {
        for await (var v of syncIterForAwait) {
            fail("body must not run: " + v);
        }
    })(), ctorErr, "for-await-of rejects with poisoned constructor error");
    assertSame(forAwaitReturnCount, 1, "for-await-of calls sync return() once on poisoned constructor");

    // 3c. Original error wins over throwing / non-object return() during IteratorClose(ThrowCompletion)
    var returnCalledOnClose = 0;
    var syncIterCloseThrows = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            var p = Promise.resolve("x");
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            return { value: p, done: false };
        },
        return: function () {
            returnCalledOnClose += 1;
            throw new Error("secondary-return-error");
        }
    };
    async function* agCloseThrows() { yield* syncIterCloseThrows; }
    await expectReject(agCloseThrows().next(), ctorErr, "original wrapper error wins over return() throw");
    assertSame(returnCalledOnClose, 1, "return() was invoked during IteratorClose");

    // 3d. done: true with poisoned .constructor must NOT call return()
    var doneTrueReturnCalls = 0;
    var syncIterDoneTruePoison = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            var p = Promise.resolve("x");
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            return { value: p, done: true };
        },
        return: function () {
            doneTrueReturnCalls += 1;
            return { done: true };
        }
    };
    async function* agDoneTruePoison() { return yield* syncIterDoneTruePoison; }
    await expectReject(agDoneTruePoison().next(), ctorErr, "done:true poisoned constructor still rejects");
    assertSame(doneTrueReturnCalls, 0, "done:true must not call return() on poisoned constructor");

    // 3e. throw() result with poisoned .constructor closes sync iterator
    var throwPoisonReturns = 0;
    var syncIterThrowPoison = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        throw: function () {
            var p = Promise.resolve("x");
            Object.defineProperty(p, "constructor", {
                get: function () { throw ctorErr; }
            });
            return { value: p, done: false };
        },
        return: function () {
            throwPoisonReturns += 1;
            return { done: true };
        }
    };
    async function* agThrowPoison() { yield* syncIterThrowPoison; }
    var gThrowPoison = agThrowPoison();
    await gThrowPoison.next();
    await expectReject(gThrowPoison.throw("arg"), ctorErr, "throw() poisoned constructor rejects");
    assertSame(throwPoisonReturns, 1, "throw() poisoned constructor closes sync iterator");

    // 3f. Yielded non-Promise object with throwing .then getter closes sync iterator
    var thenGetErr = { tag: "then-getter-err" };
    var thenGetFinally = 0;
    function* genBadThen() {
        try {
            yield {
                get then() {
                    throw thenGetErr;
                }
            };
        } finally {
            thenGetFinally += 1;
        }
    }
    async function* agBadThen() { yield* genBadThen(); }
    await expectReject(agBadThen().next(), thenGetErr, "throwing .then getter rejects");
    assertSame(thenGetFinally, 1, "throwing .then getter closes sync generator");

    // 3g. Single constructor read in PromiseResolve; SpeciesConstructor must NOT be invoked
    var ctorReads = 0;
    var returnOnCtorOnce = 0;
    var syncIterCtorOnce = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            var p = Promise.resolve(99);
            Object.defineProperty(p, "constructor", {
                get: function () {
                    ctorReads += 1;
                    if (ctorReads > 1) {
                        throw new Error("constructor read more than once");
                    }
                    return Promise;
                }
            });
            return { value: p, done: false };
        },
        return: function () {
            returnOnCtorOnce += 1;
            return { done: true };
        }
    };
    async function* agCtorOnce() { yield* syncIterCtorOnce; }
    var gOnce = agCtorOnce();
    var onceRes = await gOnce.next();
    assertSame(onceRes.value, 99, "resolved with 99 when constructor read once");
    assertSame(ctorReads, 1, "Promise.constructor read exactly once by PromiseResolve");
    assertSame(returnOnCtorOnce, 0, "return not called on fulfilled value");
});

// 4. Rejected yielded promise closes sync iterator in next(), throw(), for-await-of, yield*
addAsyncTest("rejected yielded promises close sync iterator and preserve original error", async function () {
    // 4a. for-await-of over sync generator yielding Promise.reject
    var faGenFinally = 0;
    function* genRejectForAwait() {
        try {
            yield Promise.reject("reject-fa-gen");
        } finally {
            faGenFinally += 1;
        }
    }
    await expectReject((async function () {
        for await (var v of genRejectForAwait()) {
            fail("for-await body should not execute: " + v);
        }
    })(), "reject-fa-gen", "for-await sync generator rejected promise");
    assertSame(faGenFinally, 1, "sync generator finally ran once in for-await");

    // 4b. for-await-of over custom sync iterator yielding Promise.reject
    var faIterReturn = 0;
    var syncIterReject = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            return { value: Promise.reject("reject-fa-iter"), done: false };
        },
        return: function () {
            faIterReturn += 1;
        }
    };
    await expectReject((async function () {
        for await (var v of syncIterReject) {
            fail("for-await body should not execute: " + v);
        }
    })(), "reject-fa-iter", "for-await custom sync iterator rejected promise");
    assertSame(faIterReturn, 1, "custom sync iterator return() called once in for-await");

    // 4c. async function* yield* over sync generator & custom sync iterator yielding Promise.reject
    var ysGenFinally = 0;
    function* genRejectYieldStar() {
        try {
            yield Promise.reject("reject-ys-gen");
        } finally {
            ysGenFinally += 1;
        }
    }
    async function* agYsGen() { yield* genRejectYieldStar(); }
    await expectReject(agYsGen().next(), "reject-ys-gen", "yield* sync generator rejected promise");
    assertSame(ysGenFinally, 1, "yield* sync generator finally ran once");

    // 4d. throw() returning { value: Promise.reject(...), done: false } closes sync iterator
    var throwRejectReturns = 0;
    var throwErr = { tag: "throw-yielded-reject" };
    var syncIterThrowReject = {
        [Symbol.iterator]: function () { return this; },
        next: function () { return { value: 1, done: false }; },
        throw: function () {
            return { value: Promise.reject(throwErr), done: false };
        },
        return: function () {
            throwRejectReturns += 1;
            return 123; // non-object must still preserve original error
        }
    };
    async function* agThrowReject() { return yield* syncIterThrowReject; }
    var gThrowRej = agThrowReject();
    await gThrowRej.next();
    await expectReject(gThrowRej.throw("ignored"), throwErr, "throw() yielded rejected promise propagates original error");
    assertSame(throwRejectReturns, 1, "throw() yielded rejected promise calls return() once");
    var afterThrowRej = await gThrowRej.next();
    assertSame(afterThrowRej.done, true, "completed after throw() yielded rejection");

    // 4e. Primitive / undefined rejection + throwing return() preserves original error
    var closeThrowCalls = 0;
    var syncIterRejectCloseThrows = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            return { value: Promise.reject(undefined), done: false };
        },
        return: function () {
            closeThrowCalls += 1;
            throw new RangeError("must-be-swallowed");
        }
    };
    async function* agUndefinedRej() { yield* syncIterRejectCloseThrows; }
    await expectReject(agUndefinedRej().next(), undefined, "undefined rejection preserved when return() throws");
    assertSame(closeThrowCalls, 1, "return() invoked once even when throwing");

    // 4f. Rejected promise with done: true must NOT call return()
    var doneTrueRejectCalls = 0;
    var syncIterDoneTrueReject = {
        [Symbol.iterator]: function () { return this; },
        next: function () {
            return { value: Promise.reject("done-true-err"), done: true };
        },
        return: function () {
            doneTrueRejectCalls += 1;
        }
    };
    async function* agDoneTrueRej() { return yield* syncIterDoneTrueReject; }
    await expectReject(agDoneTrueRej().next(), "done-true-err", "done:true rejected promise propagates");
    assertSame(doneTrueRejectCalls, 0, "done:true must not call return()");
});

// 5. -CollectGarbage stress & cross-realm rejection close
addAsyncTest("GC stress during async rejections and cross-realm close", async function () {
    var totalIterators = 40;
    var closedCount = 0;
    var pendingPromises = [];

    for (var i = 0; i < totalIterators; i++) {
        (function (idx) {
            var rejectFn;
            var pending = new Promise(function (_, reject) { rejectFn = reject; });
            var sentinel = { index: idx };
            var syncIter = {
                marker: new Array(64).fill(idx),
                [Symbol.iterator]: function () { return this; },
                next: function () {
                    return { value: pending, done: false };
                },
                return: function () {
                    gcNow();
                    closedCount += 1;
                    assertSame(this.marker[0], idx, "syncIterator state intact after GC");
                    return { done: true };
                }
            };
            async function* ag() {
                yield* syncIter;
            }
            var g = ag();
            var p = g.next().then(
                function () { fail("GC stress iteration " + idx + " should reject"); },
                function (err) { assertSame(err, sentinel, "GC stress preserved rejection reason " + idx); }
            );
            pendingPromises.push({ p: p, rejectFn: rejectFn, sentinel: sentinel });
        })(i);
    }

    // Drop any transient allocations and force multiple GC cycles before rejecting
    for (var alloc = 0; alloc < 1000; alloc++) {
        ({ a: alloc, b: [alloc, alloc + 1] });
    }
    gcNow();
    gcNow();

    for (var j = 0; j < pendingPromises.length; j++) {
        if ((j % 5) === 0) {
            gcNow();
        }
        pendingPromises[j].rejectFn(pendingPromises[j].sentinel);
    }

    await Promise.all(pendingPromises.map(function (entry) { return entry.p; }));
    assertSame(closedCount, totalIterators, "all sync iterators closed after GC stress");

    // Cross-realm sync iterator & rejected promise
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        var other = WScript.LoadScript(
            "var foreignClosed = 0;" +
            "var foreignErr = { from: 'other-realm' };" +
            "var foreignIter = {" +
            "  [Symbol.iterator]() { return this; }," +
            "  next() { return { value: Promise.reject(foreignErr), done: false }; }," +
            "  return() { foreignClosed += 1; return { done: true }; }" +
            "};",
            "samethread"
        );
        await expectReject((async function () {
            for await (var v of other.foreignIter) {
                fail("cross-realm body should not run: " + v);
            }
        })(), other.foreignErr, "cross-realm rejected promise preserves foreign error");
        assertSame(other.foreignClosed, 1, "cross-realm sync iterator closed once");
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
