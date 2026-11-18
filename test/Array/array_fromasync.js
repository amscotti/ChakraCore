//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function fail(message) {
    print("array_fromasync: FAIL: " + message);
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

function assertArrayEqual(actual, expected, message) {
    assertTrue(Array.isArray(actual) || typeof actual === "object", message + " (not object)");
    assertSame(actual.length, expected.length, message + " (length)");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], message + " (index " + i + ")");
    }
}

function assertThrowsSync(ErrorCtor, fn, message) {
    try {
        fn();
    } catch (e) {
        assertTrue(e instanceof ErrorCtor, message + " (wrong error: " + e + ")");
        return;
    }
    fail(message + " (did not throw)");
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

function expectReject(promise, ErrorCtor, message) {
    assertTrue(promise instanceof Promise, message + " (must return a Promise)");
    return promise.then(
        function (val) {
            fail(message + " (resolved with " + val + " instead of rejecting)");
        },
        function (err) {
            if (ErrorCtor !== undefined) {
                assertTrue(err instanceof ErrorCtor, message + " (expected " + ErrorCtor.name + ", got " + err + ")");
            }
            return err;
        }
    );
}

// 1. Metadata and non-constructor invariants
(function testMetadata() {
    assertSame(typeof Array.fromAsync, "function", "typeof Array.fromAsync");
    assertSame(Object.getPrototypeOf(Array.fromAsync), Function.prototype, "prototype");
    assertSame(Object.getOwnPropertyDescriptor(Array.fromAsync, "prototype"), undefined, "no prototype prop");
    assertTrue(Object.isExtensible(Array.fromAsync), "extensible");

    var propDesc = Object.getOwnPropertyDescriptor(Array, "fromAsync");
    assertTrue(propDesc.writable && !propDesc.enumerable && propDesc.configurable, "Array.fromAsync descriptor");

    var lenDesc = Object.getOwnPropertyDescriptor(Array.fromAsync, "length");
    assertSame(lenDesc.value, 1, "length value");
    assertTrue(!lenDesc.writable && !lenDesc.enumerable && lenDesc.configurable, "length descriptor");

    var nameDesc = Object.getOwnPropertyDescriptor(Array.fromAsync, "name");
    assertSame(nameDesc.value, "fromAsync", "name value");
    assertTrue(!nameDesc.writable && !nameDesc.enumerable && nameDesc.configurable, "name descriptor");

    assertThrowsSync(TypeError, function () { new Array.fromAsync([]); }, "not a constructor");
    assertThrowsSync(TypeError, function () { Reflect.construct(Array.fromAsync, [[]]); }, "Reflect.construct fails");
})();

// 2. Always returns a %Promise% and checks mapfn before accessing @@asyncIterator
addAsyncTest("abrupt validation rejects returned promise", async function () {
    var getterCalled = false;
    var badItems = {
        get [Symbol.asyncIterator]() {
            getterCalled = true;
            throw new Error("should not reach @@asyncIterator");
        }
    };
    await expectReject(Array.fromAsync(badItems, 123), TypeError, "non-callable mapfn");
    assertTrue(!getterCalled, "mapfn validated before @@asyncIterator");

    await expectReject(Array.fromAsync(null), TypeError, "null asyncItems");
    await expectReject(Array.fromAsync(undefined), TypeError, "undefined asyncItems");
    await expectReject(Array.fromAsync({ [Symbol.asyncIterator]: 42 }), TypeError, "non-callable @@asyncIterator");
    await expectReject(Array.fromAsync({ [Symbol.iterator]: "nope" }), TypeError, "non-callable @@iterator");
    await expectReject(Array.fromAsync({ [Symbol.asyncIterator]() { return 42; } }), TypeError, "non-object async iterator");
    await expectReject(Array.fromAsync({ [Symbol.iterator]() { return 42; } }), TypeError, "non-object sync iterator");
    await expectReject(Array.fromAsync.call({}, { length: 4294967296 }), RangeError, "array-like > 2^32 - 1");
    await expectReject(Array.fromAsync({ length: 1n }), TypeError, "BigInt length");
});

// 3. Async iterables: basic, does not await yielded promises without mapfn, awaits mapfn once, GC inside next/mapfn
addAsyncTest("async iterables and GC", async function () {
    var p1 = Promise.resolve("a");
    var p2 = Promise.resolve("b");
    async function* asyncGen() {
        gcNow();
        yield p1;
        gcNow();
        yield p2;
    }
    var unmappedGen = await Array.fromAsync(asyncGen());
    assertTrue(Array.isArray(unmappedGen), "unmapped asyncGen is Array");
    assertArrayEqual(unmappedGen, ["a", "b"], "async generator yields unwrapped values");

    var customAsyncIter = {
        [Symbol.asyncIterator]() {
            var i = 0;
            return {
                async next() {
                    gcNow();
                    if (i === 0) {
                        i++;
                        return { value: p1, done: false };
                    }
                    if (i === 1) {
                        i++;
                        return { value: p2, done: false };
                    }
                    return { done: true };
                }
            };
        }
    };
    var unmapped = await Array.fromAsync(customAsyncIter);
    assertTrue(Array.isArray(unmapped), "unmapped is Array");
    assertSame(unmapped.length, 2, "unmapped length");
    assertSame(unmapped[0], p1, "async iterable does not await yielded promise without mapfn");
    assertSame(unmapped[1], p2, "async iterable second yielded promise");

    var mapCalls = 0;
    var ctx = { tag: 7 };
    var mapped = await Array.fromAsync(customAsyncIter, async function (val, idx) {
        gcNow();
        mapCalls++;
        assertSame(this, ctx, "mapfn thisArg");
        var resolved = await val;
        return resolved + ":" + idx + ":" + this.tag;
    }, ctx);
    assertSame(mapCalls, 2, "mapfn called twice");
    assertArrayEqual(mapped, ["a:0:7", "b:1:7"], "async iterable mapped");

    // Sync iterator returned from @@asyncIterator does not await yielded values
    var syncFromAsync = {
        [Symbol.asyncIterator]() {
            var i = 0;
            return {
                next() {
                    gcNow();
                    if (i < 2) {
                        return { value: i++ === 0 ? p1 : p2, done: false };
                    }
                    return { value: undefined, done: true };
                }
            };
        }
    };
    var syncFromAsyncRes = await Array.fromAsync(syncFromAsync);
    assertSame(syncFromAsyncRes[0], p1, "@@asyncIterator sync iterator preserves promise value");
    assertSame(syncFromAsyncRes[1], p2, "@@asyncIterator sync iterator preserves second promise");
});

// 4. AsyncIteratorClose on mapfn throw/rejection and CreateDataPropertyOrThrow failure
addAsyncTest("async iterator closing", async function () {
    var closedOnSyncThrow = 0;
    var iter1 = {
        next() { return Promise.resolve({ value: 10, done: false }); },
        return() {
            closedOnSyncThrow++;
            gcNow();
            return Promise.resolve({ done: true });
        },
        [Symbol.asyncIterator]() { return this; }
    };
    await expectReject(
        Array.fromAsync(iter1, function () {
            gcNow();
            throw new RangeError("sync mapfn fail");
        }),
        RangeError,
        "async iterator sync mapfn throw"
    );
    assertSame(closedOnSyncThrow, 1, "closed on sync mapfn throw");

    var closedOnAsyncReject = 0;
    var iter2 = {
        next() { return Promise.resolve({ value: 20, done: false }); },
        return() {
            closedOnAsyncReject++;
            return Promise.reject(new Error("return error should be masked"));
        },
        [Symbol.asyncIterator]() { return this; }
    };
    var err2 = await expectReject(
        Array.fromAsync(iter2, async function () {
            throw new SyntaxError("async mapfn reject");
        }),
        SyntaxError,
        "async iterator async mapfn reject keeps original error"
    );
    assertSame(err2.message, "async mapfn reject", "original error wins over return rejection");
    assertSame(closedOnAsyncReject, 1, "closed on async mapfn rejection");

    // CreateDataPropertyOrThrow failure closes async iterator
    function UnsettableCtor() {
        Object.defineProperty(this, 0, { value: 99, writable: true, configurable: false, enumerable: true });
    }
    var closedOnDefine = 0;
    var iter3 = {
        next() { return Promise.resolve({ value: 1, done: false }); },
        return() {
            closedOnDefine++;
            return Promise.resolve({ done: true });
        },
        [Symbol.asyncIterator]() { return this; }
    };
    await expectReject(
        Array.fromAsync.call(UnsettableCtor, iter3),
        TypeError,
        "CreateDataPropertyOrThrow failure closes async iterator"
    );
    assertSame(closedOnDefine, 1, "closed on define failure");

    // next() rejection does NOT close async iterator
    var closedOnNextReject = 0;
    var iter4 = {
        next() { return Promise.reject(new URIError("next fail")); },
        return() {
            closedOnNextReject++;
            return Promise.resolve({ done: true });
        },
        [Symbol.asyncIterator]() { return this; }
    };
    await expectReject(Array.fromAsync(iter4), URIError, "next() rejection");
    assertSame(closedOnNextReject, 0, "next() rejection does not call return()");

    // Async iterator resolving to non-object rejects with TypeError without closing
    var closedOnNonObjectResult = 0;
    var iter5 = {
        next() { return Promise.resolve(42); },
        return() {
            closedOnNonObjectResult++;
            return Promise.resolve({ done: true });
        },
        [Symbol.asyncIterator]() { return this; }
    };
    await expectReject(Array.fromAsync(iter5), TypeError, "async iterator non-object result");
    assertSame(closedOnNonObjectResult, 0, "non-object next result does not call return()");
});

// 5. Sync iterables (AsyncFromSyncIterator) including rejecting thenable closing + GC
addAsyncTest("sync iterables and AsyncFromSyncIterator close", async function () {
    var finallyCount = 0;
    var rejectingThenable = {
        then: function (resolve, reject) {
            gcNow();
            reject(new RangeError("thenable rejected"));
        }
    };
    function* genRejecting() {
        try {
            yield rejectingThenable;
        } finally {
            finallyCount++;
        }
    }
    await expectReject(Array.fromAsync(genRejecting()), RangeError, "sync iterable rejecting thenable");
    assertSame(finallyCount, 1, "sync generator finally ran when yielded thenable rejected");

    // Yielded Promise with throwing .constructor getter closes sync iterator
    var finallyCountCtorThrow = 0;
    var badCtorPromise = Promise.resolve(1);
    Object.defineProperty(badCtorPromise, "constructor", {
        get: function () { throw new URIError("bad constructor getter"); }
    });
    function* genBadCtorPromise() {
        try {
            yield badCtorPromise;
        } finally {
            finallyCountCtorThrow++;
        }
    }
    await expectReject(Array.fromAsync(genBadCtorPromise()), URIError, "sync iterable Promise.constructor getter throws");
    assertSame(finallyCountCtorThrow, 1, "sync generator finally ran when Promise.constructor getter threw");

    // Throwing .then on yielded thenable also closes sync iterator
    var finallyCountThrowThen = 0;
    function* genThrowingThen() {
        try {
            yield {
                then: function () {
                    throw new TypeError("then threw");
                }
            };
        } finally {
            finallyCountThrowThen++;
        }
    }
    await expectReject(Array.fromAsync(genThrowingThen()), TypeError, "sync iterable throwing then");
    assertSame(finallyCountThrowThen, 1, "sync generator finally ran when .then threw");

    // Rejecting thenable with done: true must NOT call return()
    var closedOnDoneTrue = 0;
    var iterDoneTrue = {
        next() { return { value: rejectingThenable, done: true }; },
        return() { closedOnDoneTrue++; return { done: true }; },
        [Symbol.iterator]() { return this; }
    };
    await expectReject(Array.fromAsync(iterDoneTrue), RangeError, "done:true rejecting thenable");
    assertSame(closedOnDoneTrue, 0, "done:true does not call return()");

    // mapfn throw / CreateDataPropertyOrThrow failure closes sync iterator and awaits return().value
    var syncReturnAwaited = false;
    var syncClosed = 0;
    var syncIterMapFail = {
        next() { return { value: 5, done: false }; },
        return() {
            syncClosed++;
            return {
                value: {
                    then: function (resolve) {
                        syncReturnAwaited = true;
                        resolve(undefined);
                    }
                },
                done: true
            };
        },
        [Symbol.iterator]() { return this; }
    };
    await expectReject(
        Array.fromAsync(syncIterMapFail, function () { throw new Error("sync map fail"); }),
        Error,
        "sync iterable mapfn throw closes iterator"
    );
    assertSame(syncClosed, 1, "sync iterator return() called");
    assertTrue(syncReturnAwaited, "sync iterator return().value awaited before rejection");

    // Unsettable property closes sync iterator
    function UnsettableCtor() {
        Object.defineProperty(this, 0, { value: 0, writable: true, configurable: false, enumerable: true });
    }
    var unsettableClosed = 0;
    var iterUnsettable = {
        next() { return { value: 1, done: false }; },
        return() { unsettableClosed++; return { done: true }; },
        [Symbol.iterator]() { return this; }
    };
    await expectReject(Array.fromAsync.call(UnsettableCtor, iterUnsettable), TypeError, "unsettable closes sync iterator");
    assertSame(unsettableClosed, 1, "sync iterator closed on CreateDataPropertyOrThrow failure");

    // Yielding thenables awaits once with and without mapfn
    var thenCalls = 0;
    var thenableOnce = {
        then: function (resolve) {
            thenCalls++;
            gcNow();
            resolve(42);
        }
    };
    var resOnce = await Array.fromAsync([thenableOnce], function (v) { return v + 1; });
    assertSame(thenCalls, 1, "sync iterable thenable awaited once");
    assertArrayEqual(resOnce, [43], "sync iterable thenable mapped");
});

// 6. Array-like inputs (holes, promises, thenables, mapfn, primitives)
addAsyncTest("array-likes with promises, thenables, and holes", async function () {
    var arrayLike = {
        length: 4,
        0: 10,
        1: Promise.resolve(20),
        3: {
            then: function (resolve) {
                gcNow();
                resolve(30);
            }
        }
    };
    var res = await Array.fromAsync(arrayLike, function (v, k) {
        gcNow();
        return (v === undefined ? -1 : v) + k;
    });
    assertArrayEqual(res, [10, 21, 1, 33], "array-like holes & promises mapped");

    await expectReject(
        Array.fromAsync({ length: 1, 0: Promise.reject(new RangeError("elem reject")) }),
        RangeError,
        "array-like rejecting element"
    );
});

// 7. Custom `this` constructor and operation ordering
addAsyncTest("custom this constructor operations", async function () {
    var ops = [];
    function CustomArray() {
        ops.push("ctor:" + arguments.length + (arguments.length ? ":" + arguments[0] : ""));
        return new Proxy(Object.create(null), {
            defineProperty: function (t, k, desc) {
                ops.push("def:" + String(k) + "=" + desc.value);
                return Reflect.defineProperty(t, k, desc);
            },
            set: function (t, k, val) {
                ops.push("set:" + String(k) + "=" + val);
                return Reflect.set(t, k, val);
            }
        });
    }

    var r1 = await Array.fromAsync.call(CustomArray, [7, 8]);
    assertSame(r1.length, 2, "custom iterable length");
    assertArrayEqual(ops, ["ctor:0", "def:0=7", "def:1=8", "set:length=2"], "iterable ctor ops");

    ops = [];
    var r2 = await Array.fromAsync.call(CustomArray, { length: 2, 0: 9, 1: Promise.resolve(10) });
    assertSame(r2.length, 2, "custom array-like length");
    assertArrayEqual(ops, ["ctor:1:2", "def:0=9", "def:1=10", "set:length=2"], "array-like ctor ops");

    // Non-writable configurable element is overwritten by CreateDataPropertyOrThrow
    function ReadonlyElemCtor() {
        Object.defineProperty(this, 0, { value: 99, writable: false, configurable: true, enumerable: true });
    }
    var r3 = await Array.fromAsync.call(ReadonlyElemCtor, [123]);
    assertSame(r3[0], 123, "readonly configurable element overwritten");

    // Non-writable length rejects
    function ReadonlyLenCtor() {
        Object.defineProperty(this, "length", { value: 99, writable: false, configurable: true, enumerable: true });
    }
    await expectReject(Array.fromAsync.call(ReadonlyLenCtor, [1]), TypeError, "readonly length rejects");
    await expectReject(Array.fromAsync.call(ReadonlyLenCtor, { length: 1, 0: 1 }), TypeError, "readonly length array-like rejects");

    // Proxy returning false from defineProperty closes iterator; Proxy returning false from set rejects on length
    function FalseDefineCtor() {
        return new Proxy({}, {
            defineProperty: function () { return false; }
        });
    }
    var closedOnProxyDefineFalse = 0;
    await expectReject(
        Array.fromAsync.call(FalseDefineCtor, {
            next: function () { return { value: 1, done: false }; },
            return: function () { closedOnProxyDefineFalse++; return { done: true }; },
            [Symbol.iterator]: function () { return this; }
        }),
        TypeError,
        "Proxy defineProperty returning false rejects"
    );
    assertSame(closedOnProxyDefineFalse, 1, "Proxy defineProperty false closes iterator");

    function FalseSetLengthCtor() {
        return new Proxy({}, {
            set: function () { return false; }
        });
    }
    await expectReject(Array.fromAsync.call(FalseSetLengthCtor, [1, 2]), TypeError, "Proxy set length returning false rejects");
    await expectReject(Array.fromAsync.call(FalseSetLengthCtor, { length: 0 }), TypeError, "Proxy set length 0 returning false rejects");

    // Array.prototype[0] setter is bypassed by CreateDataPropertyOrThrow
    Object.defineProperty(Array.prototype, 0, {
        configurable: true,
        set: function () { throw new Error("Array.prototype[0] setter should not run"); }
    });
    try {
        var rDefault = await Array.fromAsync([42]);
        assertArrayEqual(rDefault, [42], "default ArrayCreate bypasses prototype setter");
        var rArrayCtor = await Array.fromAsync.call(Array, [99]);
        assertArrayEqual(rArrayCtor, [99], "Array constructor CreateDataProperty bypasses prototype setter");
    } finally {
        delete Array.prototype[0];
    }
});

// 8. Cross-realm %Promise%, cross-realm iterables, and unreferenced promise survival across GC
addAsyncTest("cross-realm %Promise% and unreferenced GC", async function () {
    var other = WScript.LoadScript(
        "function makeForeignAsyncIter() {" +
        "  return {" +
        "    [Symbol.asyncIterator]() {" +
        "      var i = 0;" +
        "      return {" +
        "        next() {" +
        "          if (i < 2) return Promise.resolve({ value: { n: ++i }, done: false });" +
        "          return Promise.resolve({ done: true });" +
        "        }" +
        "      };" +
        "    }" +
        "  };" +
        "}" +
        "function ForeignCtor() {}" +
        "function foreignMap(v, k) { return v.n * 10 + k; }",
        "samethread"
    );

    var foreignSuccess = other.Array.fromAsync([1, 2]);
    assertTrue(foreignSuccess instanceof other.Promise, "other.Array.fromAsync uses other realm %Promise%");
    assertTrue(!(foreignSuccess instanceof Promise), "not current realm Promise");
    var foreignArr = await foreignSuccess;
    assertTrue(foreignArr instanceof other.Array, "result is other realm Array");
    assertArrayEqual(foreignArr, [1, 2], "foreign realm result elements");

    var foreignFail = other.Array.fromAsync(null);
    assertTrue(foreignFail instanceof other.Promise, "other.Array.fromAsync rejection uses other realm %Promise%");
    assertTrue(!(foreignFail instanceof Promise), "rejection not current realm Promise");
    await foreignFail.then(
        function () { fail("foreignFail should reject"); },
        function (e) { assertTrue(e instanceof other.TypeError, "rejection TypeError from other realm"); }
    );

    // Current realm Array.fromAsync over foreign iterable, foreign mapfn, foreign constructor
    var crossRes = await Array.fromAsync.call(other.ForeignCtor, other.makeForeignAsyncIter(), other.foreignMap);
    assertTrue(crossRes instanceof other.ForeignCtor, "cross-realm constructor instance");
    assertSame(crossRes.length, 2, "cross-realm length");
    assertSame(crossRes[0], 10, "cross-realm elem 0");
    assertSame(crossRes[1], 21, "cross-realm elem 1");

    // Unreferenced pending Array.fromAsync survives aggressive GC
    var sideEffect = [];
    var release;
    var gate = new Promise(function (resolve) { release = resolve; });
    Array.fromAsync([gate], function (v) {
        sideEffect.push(v);
        return v;
    });
    gcNow();
    gcNow();
    release("survived");
    // Wait a few microtask turns for the unreferenced Array.fromAsync to complete
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
    assertArrayEqual(sideEffect, ["survived"], "unreferenced Array.fromAsync survived GC");
});

Promise.all(asyncTests).then(
    function () {
        print("pass");
    },
    function (err) {
        fail("unhandled runner error: " + (err && err.stack ? err.stack : err));
    }
);
