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
    print("q3_promise_keyed: FAIL: " + message);
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

function assertArraySame(actual, expected, message) {
    assertSame(actual.length, expected.length, message + " (length)");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], message + " (index " + i + ")");
    }
}

function assertThrowsSync(errType, fn, message) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        assertTrue(e instanceof errType, message + " (expected " + errType.name + ", got " + String(e) + ")");
    }
    assertTrue(threw, message + " (did not throw synchronously)");
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
        if (typeof predicate === "function" && (predicate === TypeError || predicate === Error || predicate.prototype instanceof Error)) {
            assertTrue(err instanceof predicate, message + " (expected " + predicate.name + ", got " + String(err) + ")");
        } else if (typeof predicate === "function") {
            assertTrue(predicate(err), message + " (unexpected error: " + String(err) + ")");
        } else {
            assertSame(err, predicate, message);
        }
        return err;
    }
}

// 1. Top-level JIT warmup loop (>=1000 iterations) before async tests.
// Exercises synchronous custom-constructor fast/warm paths, getters, proxies, and throwing traps.
var WARM_ITERATIONS = 1200;
var warmEvidence = {
    allKeyedCount: 0,
    allSettledCount: 0,
    proxyTraps: 0,
    getterThrows: 0
};

function SyncWarmCtor(executor) {
    var self = this;
    executor(
        function (v) { self.status = "fulfilled"; self.value = v; },
        function (r) { self.status = "rejected"; self.reason = r; }
    );
}
SyncWarmCtor.resolve = function (v) {
    return {
        then: function (onFulfilled, onRejected) {
            if (v && v.rejectWith !== undefined) {
                onRejected(v.rejectWith);
            } else {
                onFulfilled(v);
            }
        }
    };
};

var warmErr = { tag: "warm-getter-err" };
var warmSym = Symbol("warm");
for (var w = 0; w < WARM_ITERATIONS; w++) {
    var input = { 1: w, a: w + 1 };
    input[warmSym] = w + 2;
    Object.defineProperty(input, "hidden", { enumerable: false, value: 999 });

    var r1 = Promise.allKeyed.call(SyncWarmCtor, input);
    assertSame(r1.status, "fulfilled", "warm allKeyed status");
    assertSame(Object.getPrototypeOf(r1.value), null, "warm allKeyed null proto");
    assertSame(r1.value[1], w, "warm allKeyed key 1");
    assertSame(r1.value.a, w + 1, "warm allKeyed key a");
    assertSame(r1.value[warmSym], w + 2, "warm allKeyed symbol");
    warmEvidence.allKeyedCount++;

    var r2 = Promise.allSettledKeyed.call(SyncWarmCtor, {
        ok: w,
        bad: { rejectWith: w + 10 }
    });
    assertSame(r2.status, "fulfilled", "warm allSettledKeyed status");
    assertSame(r2.value.ok.status, "fulfilled", "warm settled ok.status");
    assertSame(r2.value.ok.value, w, "warm settled ok.value");
    assertSame(r2.value.bad.status, "rejected", "warm settled bad.status");
    assertSame(r2.value.bad.reason, w + 10, "warm settled bad.reason");
    warmEvidence.allSettledCount++;

    var pInput = new Proxy({ x: w }, {
        getOwnPropertyDescriptor: function (t, k) {
            warmEvidence.proxyTraps++;
            return Object.getOwnPropertyDescriptor(t, k);
        },
        get: function (t, k) {
            warmEvidence.proxyTraps++;
            return t[k];
        }
    });
    var r3 = Promise.allKeyed.call(SyncWarmCtor, pInput);
    assertSame(r3.value.x, w, "warm proxy allKeyed");

    var throwInput = {};
    Object.defineProperty(throwInput, "boom", {
        enumerable: true,
        get: function () { throw warmErr; }
    });
    var r4 = Promise.allKeyed.call(SyncWarmCtor, throwInput);
    assertSame(r4.status, "rejected", "warm throw getter rejected");
    assertSame(r4.reason, warmErr, "warm throw getter reason");
    warmEvidence.getterThrows++;
}
assertSame(warmEvidence.allKeyedCount, WARM_ITERATIONS, "warm allKeyed count");
assertSame(warmEvidence.allSettledCount, WARM_ITERATIONS, "warm allSettled count");
assertSame(warmEvidence.proxyTraps, WARM_ITERATIONS * 2, "warm proxy traps");
assertSame(warmEvidence.getterThrows, WARM_ITERATIONS, "warm getter throws");

// 2. Built-in shapes and synchronous receiver validation
(function testShapesAndReceiver() {
    [
        ["allKeyed", Promise.allKeyed],
        ["allSettledKeyed", Promise.allSettledKeyed]
    ].forEach(function (pair) {
        var name = pair[0];
        var fn = pair[1];
        assertSame(Object.getPrototypeOf(fn), Function.prototype, name + " prototype");
        assertTrue(Object.isExtensible(fn), name + " extensible");
        var desc = Object.getOwnPropertyDescriptor(Promise, name);
        assertTrue(desc.writable && !desc.enumerable && desc.configurable, name + " property desc");
        var lenDesc = Object.getOwnPropertyDescriptor(fn, "length");
        assertSame(lenDesc.value, 1, name + ".length");
        assertTrue(!lenDesc.writable && !lenDesc.enumerable && lenDesc.configurable, name + ".length desc");
        var nameDesc = Object.getOwnPropertyDescriptor(fn, "name");
        assertSame(nameDesc.value, name, name + ".name");
        assertTrue(!nameDesc.writable && !nameDesc.enumerable && nameDesc.configurable, name + ".name desc");
        assertThrowsSync(TypeError, function () { new fn({}); }, name + " not a constructor");

        [undefined, null, 42, "str", true, Symbol(), 0n, {}, eval, function () {}].forEach(function (badCtor) {
            if (typeof badCtor === "function" && badCtor !== eval) {
                return;
            }
            assertThrowsSync(TypeError, function () { fn.call(badCtor, {}); }, name + " bad receiver " + String(badCtor));
        });

        var getterCalled = false;
        function BadExecCtor(executor) {
            executor(123, "not-callable");
        }
        Object.defineProperty(BadExecCtor, "resolve", {
            get: function () { getterCalled = true; return Promise.resolve; }
        });
        assertThrowsSync(TypeError, function () { fn.call(BadExecCtor, {}); }, name + " non-callable capability throws before GetPromiseResolve");
        assertTrue(!getterCalled, name + " GetPromiseResolve not called on bad capability");
    });
})();

// 3. String + index + Symbol + __proto__ keys, non-enumerable skipping, prototype ignored
addAsyncTest("keys, __proto__, symbols, non-enumerable, descriptors", async function () {
    var sym1 = Symbol("s1");
    var sym2 = Symbol("s2");
    var hiddenSym = Symbol("hiddenSym");
    var resolveB, resolveA, resolveProto;

    var proto = { inherited: Promise.resolve("should-not-appear") };
    var input = Object.create(proto);
    input.b = new Promise(function (r) { resolveB = r; });
    input[2] = Promise.resolve("two");
    input.a = new Promise(function (r) { resolveA = r; });
    Object.defineProperty(input, "__proto__", {
        value: new Promise(function (r) { resolveProto = r; }),
        enumerable: true,
        writable: true,
        configurable: true
    });
    input[0] = Promise.resolve("zero");
    input[sym1] = Promise.resolve("sym1");
    Object.defineProperty(input, "hidden", {
        enumerable: false,
        get: function () { fail("non-enumerable getter must not be invoked"); }
    });
    Object.defineProperty(input, hiddenSym, {
        enumerable: false,
        value: Promise.resolve("hiddenSymVal")
    });
    input[sym2] = Promise.resolve("sym2");

    var pAll = Promise.allKeyed(input);
    var pSettled = Promise.allSettledKeyed(input);

    // Settle out of key order
    resolveProto("proto-val");
    resolveA("a-val");
    resolveB("b-val");

    var rAll = await pAll;
    assertSame(Object.getPrototypeOf(rAll), null, "allKeyed result null prototype");
    assertSame(rAll.hasOwnProperty, undefined, "allKeyed result has no Object.prototype");
    assertArraySame(Reflect.ownKeys(rAll), ["0", "2", "b", "a", "__proto__", sym1, sym2], "allKeyed key order");
    assertSame(rAll[0], "zero", "rAll[0]");
    assertSame(rAll[2], "two", "rAll[2]");
    assertSame(rAll.b, "b-val", "rAll.b");
    assertSame(rAll.a, "a-val", "rAll.a");
    assertSame(rAll["__proto__"], "proto-val", "rAll['__proto__']");
    assertSame(rAll[sym1], "sym1", "rAll[sym1]");
    assertSame(rAll[sym2], "sym2", "rAll[sym2]");
    assertTrue(!Object.prototype.hasOwnProperty.call(rAll, "hidden"), "hidden excluded");
    assertTrue(!Object.prototype.hasOwnProperty.call(rAll, hiddenSym), "hiddenSym excluded");
    assertTrue(!Object.prototype.hasOwnProperty.call(rAll, "inherited"), "inherited excluded");

    var protoKeyDesc = Object.getOwnPropertyDescriptor(rAll, "__proto__");
    assertSame(protoKeyDesc.value, "proto-val", "__proto__ desc value");
    assertTrue(protoKeyDesc.writable && protoKeyDesc.enumerable && protoKeyDesc.configurable, "__proto__ desc attrs");

    var rSettled = await pSettled;
    assertSame(Object.getPrototypeOf(rSettled), null, "allSettledKeyed result null prototype");
    assertArraySame(Reflect.ownKeys(rSettled), ["0", "2", "b", "a", "__proto__", sym1, sym2], "allSettledKeyed key order");
    assertSame(rSettled["__proto__"].status, "fulfilled", "settled __proto__ status");
    assertSame(rSettled["__proto__"].value, "proto-val", "settled __proto__ value");
});

// 4. Non-object promises rejection, GetPromiseResolve errors, Proxy/getter traps, mid-loop mutation
addAsyncTest("abrupt rejections, proxy traps, and mid-loop mutation", async function () {
    var primitives = [undefined, null, 0, "abc", false, Symbol("p"), 123n];
    for (var i = 0; i < primitives.length; i++) {
        await expectReject(Promise.allKeyed(primitives[i]), TypeError, "allKeyed primitive " + i);
        await expectReject(Promise.allSettledKeyed(primitives[i]), TypeError, "allSettledKeyed primitive " + i);
    }

    // Empty & non-enumerable-only & function input
    var emptyAll = await Promise.allKeyed({});
    assertSame(Object.getPrototypeOf(emptyAll), null, "empty allKeyed null proto");
    assertArraySame(Reflect.ownKeys(emptyAll), [], "empty allKeyed keys");

    function fnInput() {}
    fnInput.x = Promise.resolve(7);
    var fnRes = await Promise.allSettledKeyed(fnInput);
    assertArraySame(Reflect.ownKeys(fnRes), ["x"], "function own enumerable key only");
    assertSame(fnRes.x.value, 7, "function key value");

    // Proxy ownKeys throws, getOwnPropertyDescriptor throws, getOwnPropertyDescriptor returns undefined, get throws
    var sentinel = { err: "trap" };
    await expectReject(
        Promise.allKeyed(new Proxy({}, { ownKeys: function () { throw sentinel; } })),
        sentinel,
        "allKeyed ownKeys throws"
    );
    await expectReject(
        Promise.allSettledKeyed(new Proxy({ a: 1 }, { getOwnPropertyDescriptor: function () { throw sentinel; } })),
        sentinel,
        "allSettledKeyed gOPD throws"
    );

    var getCalled = 0;
    var skipped = await Promise.allKeyed(new Proxy({ a: 1 }, {
        getOwnPropertyDescriptor: function () { return undefined; },
        get: function () { getCalled++; return 1; }
    }));
    assertArraySame(Reflect.ownKeys(skipped), [], "gOPD undefined skipped");
    assertSame(getCalled, 0, "get not called when gOPD undefined");

    var getterThrowObj = {};
    Object.defineProperty(getterThrowObj, "k", {
        enumerable: true,
        get: function () { throw sentinel; }
    });
    await expectReject(Promise.allSettledKeyed(getterThrowObj), sentinel, "enumerable getter throws");

    // Mid-loop mutation: deleting later key and changing enumerability
    var mutating = {
        first: undefined,
        deletedKey: Promise.resolve("no"),
        demotedKey: Promise.resolve("no2")
    };
    Object.defineProperty(mutating, "first", {
        enumerable: true,
        configurable: true,
        get: function () {
            delete mutating.deletedKey;
            Object.defineProperty(mutating, "demotedKey", { enumerable: false });
            mutating.addedLater = Promise.resolve("not-in-ownkeys");
            return Promise.resolve("yes");
        }
    });
    var mutRes = await Promise.allKeyed(mutating);
    assertArraySame(Reflect.ownKeys(mutRes), ["first"], "mid-loop mutation respected");
    assertSame(mutRes.first, "yes", "first resolved");

    // Missing/non-callable/throwing resolve on subclass
    class SubPromise extends Promise {}
    SubPromise.resolve = null;
    await expectReject(Promise.allKeyed.call(SubPromise, { a: 1 }), TypeError, "non-callable resolve rejects");
    delete SubPromise.resolve;
    Object.defineProperty(SubPromise, "resolve", {
        get: function () { throw sentinel; }
    });
    await expectReject(Promise.allSettledKeyed.call(SubPromise, {}), sentinel, "throwing resolve getter rejects even on empty {}");
});

// 5. Custom Promise subclass, alreadyCalled guards, resolve-before-loop-exit, Object.prototype pollution immunity
addAsyncTest("subclass, alreadyCalled guards, Object.prototype pollution immunity", async function () {
    // Pollute Object.prototype with throwing setters for status/value/reason
    Object.defineProperty(Object.prototype, "status", Object.assign(Object.create(null), {
        configurable: true,
        set: function () { fail("Object.prototype.status setter must not be called"); }
    }));
    Object.defineProperty(Object.prototype, "value", Object.assign(Object.create(null), {
        configurable: true,
        set: function () { fail("Object.prototype.value setter must not be called"); }
    }));
    Object.defineProperty(Object.prototype, "reason", Object.assign(Object.create(null), {
        configurable: true,
        set: function () { fail("Object.prototype.reason setter must not be called"); }
    }));

    try {
        var settledMixed = await Promise.allSettledKeyed({
            ok: Promise.resolve(11),
            bad: Promise.reject(22)
        });
        assertSame(settledMixed.ok.status, "fulfilled", "settled ok.status");
        assertSame(settledMixed.ok.value, 11, "settled ok.value");
        assertSame(settledMixed.bad.status, "rejected", "settled bad.status");
        assertSame(settledMixed.bad.reason, 22, "settled bad.reason");
    } finally {
        delete Object.prototype.status;
        delete Object.prototype.value;
        delete Object.prototype.reason;
    }

    // Custom subclass + resolve-from-same-thenable + reject-from-same-thenable
    class CustomPromise extends Promise {}
    var resolveHits = 0;
    CustomPromise.resolve = function (v) {
        assertSame(this, CustomPromise, "CustomPromise.resolve receiver");
        resolveHits++;
        return Promise.resolve(v);
    };
    var subResult = await Promise.allKeyed.call(CustomPromise, { x: 1, y: 2 });
    assertSame(resolveHits, 2, "CustomPromise.resolve called per enumerable key");
    assertSame(subResult.x, 1, "subResult.x");
    assertSame(subResult.y, 2, "subResult.y");

    // alreadyCalled guard in allSettledKeyed (onFulfilled and onRejected share alreadyCalled)
    var settledResolveCount = 0;
    function GuardCtor(executor) {
        executor(
            function (res) {
                settledResolveCount++;
                assertSame(res.k.status, "rejected", "first reject wins over later resolve");
                assertSame(res.k.reason, "first-err", "first reason wins");
            },
            function (e) { fail("unexpected reject: " + e); }
        );
    }
    GuardCtor.resolve = function (v) { return v; };

    var rejectCalls = [];
    Promise.allSettledKeyed.call(GuardCtor, {
        k: {
            then: function (onFulfilled, onRejected) {
                assertSame(onFulfilled.length, 1, "element fn length");
                assertSame(onFulfilled.name, "", "element fn name");
                assertSame(Object.getPrototypeOf(onFulfilled), Function.prototype, "element fn proto");
                assertTrue(!Object.prototype.hasOwnProperty.call(onFulfilled, "prototype"), "no .prototype");
                assertThrowsSync(TypeError, function () { new onFulfilled(); }, "element fn not constructor");
                assertTrue(onFulfilled !== onRejected, "distinct resolve/reject functions");
                rejectCalls.push(onRejected("first-err"));
                rejectCalls.push(onFulfilled("second-ok"));
                rejectCalls.push(onRejected("third-err"));
            }
        }
    });
    assertSame(settledResolveCount, 1, "GuardCtor resolved once");
    assertArraySame(rejectCalls, [undefined, undefined, undefined], "guard calls return undefined");

    // capability.resolve throwing rejects capability
    var resolveErr = { tag: "cap-resolve-throw" };
    function ThrowingResolveCtor(executor) {
        return new Promise(function (_, reject) {
            executor(function () { throw resolveErr; }, reject);
        });
    }
    ThrowingResolveCtor.resolve = Promise.resolve;
    await expectReject(Promise.allKeyed.call(ThrowingResolveCtor, {}), resolveErr, "capability.resolve throw rejects");
});

// 6. GC stress with dynamic PropertyRecords (-CollectGarbage) + cross-realm
addAsyncTest("GC stress with dynamic keys and cross-realm", async function () {
    var pending = [];
    for (var i = 0; i < 30; i++) {
        (function (idx) {
            var resolvers = {};
            var dynamicPrefix = "dyn_" + String(idx) + "_" + Math.floor(idx * 97 + 13);
            var sym = Symbol("gc_" + idx);
            var dict = {};
            dict[dynamicPrefix + "_a"] = new Promise(function (r) { resolvers.a = r; });
            dict[dynamicPrefix + "_b"] = new Promise(function (r) { resolvers.b = r; });
            dict[sym] = new Promise(function (r) { resolvers.s = r; });

            var pAll = Promise.allKeyed(dict);
            var pSet = Promise.allSettledKeyed(dict);
            pending.push({
                idx: idx,
                prefix: dynamicPrefix,
                sym: sym,
                resolvers: resolvers,
                pAll: pAll,
                pSet: pSet
            });
        })(i);
    }

    // Churn transient objects and PropertyRecords + force GC before settling
    for (var c = 0; c < 500; c++) {
        var tmp = {};
        tmp["churn_" + c] = { payload: [c, c + 1] };
    }
    gcNow();
    gcNow();

    for (var j = pending.length - 1; j >= 0; j--) {
        if ((j % 3) === 0) {
            gcNow();
        }
        var item = pending[j];
        item.resolvers.s({ symIdx: item.idx });
        item.resolvers.b("val_b_" + item.idx);
        item.resolvers.a("val_a_" + item.idx);
    }

    for (var k = 0; k < pending.length; k++) {
        var entry = pending[k];
        var resAll = await entry.pAll;
        assertArraySame(
            Reflect.ownKeys(resAll),
            [entry.prefix + "_a", entry.prefix + "_b", entry.sym],
            "GC stress key order " + k
        );
        assertSame(resAll[entry.prefix + "_a"], "val_a_" + entry.idx, "GC all a " + k);
        assertSame(resAll[entry.prefix + "_b"], "val_b_" + entry.idx, "GC all b " + k);
        assertSame(resAll[entry.sym].symIdx, entry.idx, "GC all sym " + k);

        var resSet = await entry.pSet;
        assertSame(resSet[entry.sym].status, "fulfilled", "GC settled sym status " + k);
        assertSame(resSet[entry.sym].value.symIdx, entry.idx, "GC settled sym value " + k);
    }

    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        var other = WScript.LoadScript(
            "var foreignSym = Symbol('foreign');" +
            "var foreignDict = { a: Promise.resolve(10), [foreignSym]: Promise.reject('f-err') };",
            "samethread"
        );
        var crossSettled = await Promise.allSettledKeyed(other.foreignDict);
        assertSame(Object.getPrototypeOf(crossSettled), null, "cross-realm null proto");
        assertArraySame(Reflect.ownKeys(crossSettled), ["a", other.foreignSym], "cross-realm keys");
        assertSame(crossSettled.a.status, "fulfilled", "cross-realm a status");
        assertSame(crossSettled.a.value, 10, "cross-realm a value");
        assertSame(crossSettled[other.foreignSym].status, "rejected", "cross-realm sym status");
        assertSame(crossSettled[other.foreignSym].reason, "f-err", "cross-realm sym reason");
    }
});

Promise.all(asyncTests).then(
    function () {
        print("PASS");
    },
    function (err) {
        fail("Unhandled error: " + (err && err.stack ? err.stack : err));
    }
);
