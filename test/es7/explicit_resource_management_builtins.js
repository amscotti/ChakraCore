//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Explicit Resource Management (C3.1a) built-ins verification:
// Symbol.dispose, Symbol.asyncDispose, SuppressedError,
// %IteratorPrototype%[Symbol.dispose], %AsyncIteratorPrototype%[Symbol.asyncDispose],
// DisposableStack, AsyncDisposableStack, cross-realm, reentrancy, GC stress.

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function fail(message) {
    print("explicit_resource_management_builtins: FAIL: " + message);
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
    assertSame(actual.length, expected.length, message + " (length)");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], message + " (index " + i + ")");
    }
}

function assertThrows(ErrorCtor, fn, message) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        assertTrue(e instanceof ErrorCtor, message + " (expected " + ErrorCtor.name + ", got " + String(e) + ")");
    }
    assertTrue(threw, message + " (did not throw)");
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

async function expectReject(promise, check, message) {
    assertTrue(promise instanceof Promise, message + " (must return Promise)");
    try {
        var v = await promise;
        fail(message + " (resolved with " + String(v) + ")");
    } catch (err) {
        if (typeof check === "function") {
            if (check.prototype instanceof Error || check === Error || check === TypeError || check === ReferenceError || check === SuppressedError) {
                assertTrue(err instanceof check, message + " (expected " + check.name + ", got " + String(err) + ")");
            } else {
                assertTrue(check(err), message + " (predicate failed for " + String(err) + ")");
            }
        } else {
            assertSame(err, check, message);
        }
        return err;
    }
}

function createOtherRealm() {
    if (typeof WScript !== "undefined" && typeof WScript.LoadScript === "function") {
        return WScript.LoadScript(
            "var rStack = new DisposableStack();" +
            "var rAsyncStack = new AsyncDisposableStack();" +
            "var rDisposed = [];" +
            "function makeResource(tag) { return { [Symbol.dispose]() { rDisposed.push(tag); } }; }" +
            "function makeAsyncResource(tag) { return { async [Symbol.asyncDispose]() { await Promise.resolve(); rDisposed.push(tag); } }; }",
            "samethread"
        );
    }
    return null;
}

// 1. Symbol.dispose & Symbol.asyncDispose
(function testWellKnownSymbols() {
    for (var i = 0; i < 2; i++) {
        var name = i === 0 ? "dispose" : "asyncDispose";
        var sym = Symbol[name];
        assertSame(typeof sym, "symbol", "Symbol." + name + " typeof");
        assertSame(Symbol.keyFor(sym), undefined, "Symbol." + name + " not registered in Symbol.for");
        var desc = Object.getOwnPropertyDescriptor(Symbol, name);
        assertSame(desc.writable, false, "Symbol." + name + " writable");
        assertSame(desc.enumerable, false, "Symbol." + name + " enumerable");
        assertSame(desc.configurable, false, "Symbol." + name + " configurable");
    }

    var other = createOtherRealm();
    if (other) {
        assertSame(Symbol.dispose, other.Symbol.dispose, "cross-realm Symbol.dispose identity");
        assertSame(Symbol.asyncDispose, other.Symbol.asyncDispose, "cross-realm Symbol.asyncDispose identity");
    }
})();

// 2. SuppressedError semantics & construction ordering
(function testSuppressedError() {
    assertSame(Object.getPrototypeOf(SuppressedError), Error, "SuppressedError.[[Prototype]] === Error");
    assertSame(Object.getPrototypeOf(SuppressedError.prototype), Error.prototype, "SuppressedError.prototype.[[Prototype]]");
    assertSame(SuppressedError.length, 3, "SuppressedError.length");
    assertSame(SuppressedError.name, "SuppressedError", "SuppressedError.name");
    assertSame(Error.isError(new SuppressedError()), true, "Error.isError(new SuppressedError())");
    assertSame(Error.isError(SuppressedError()), true, "Error.isError(SuppressedError())");
    assertSame(Error.isError(SuppressedError.prototype), false, "Error.isError(SuppressedError.prototype)");
    assertSame(SuppressedError.prototype.hasOwnProperty("error"), false, "no error on prototype");
    assertSame(SuppressedError.prototype.hasOwnProperty("suppressed"), false, "no suppressed on prototype");

    // message === undefined -> no own message property
    var noMsg1 = new SuppressedError("e1", "s1");
    var noMsg2 = SuppressedError("e2", "s2", undefined);
    assertSame(Object.prototype.hasOwnProperty.call(noMsg1, "message"), false, "implicit undefined message");
    assertSame(Object.prototype.hasOwnProperty.call(noMsg2, "message"), false, "explicit undefined message");
    assertSame(noMsg1.error, "e1", "noMsg1.error");
    assertSame(noMsg1.suppressed, "s1", "noMsg1.suppressed");

    // Property key ordering: message, error, suppressed contiguous (even with .stack)
    var toStringCalls = 0;
    var msgObj = {
        toString: function () {
            toStringCalls++;
            return "suppressed msg";
        }
    };
    var errObj = { a: 1 };
    var supObj = { b: 2 };
    var se = new SuppressedError(errObj, supObj, msgObj);
    assertSame(toStringCalls, 1, "message ToString called once");
    assertSame(se.message, "suppressed msg", "se.message");
    assertSame(se.error, errObj, "se.error");
    assertSame(se.suppressed, supObj, "se.suppressed");

    var keys = Object.getOwnPropertyNames(se);
    var msgIdx = keys.indexOf("message");
    var errIdx = keys.indexOf("error");
    var supIdx = keys.indexOf("suppressed");
    assertTrue(msgIdx >= 0, "message key present");
    assertSame(errIdx, msgIdx + 1, "error immediately after message");
    assertSame(supIdx, errIdx + 1, "suppressed immediately after error");

    for (var prop of ["message", "error", "suppressed"]) {
        var d = Object.getOwnPropertyDescriptor(se, prop);
        assertSame(d.writable, true, prop + " writable");
        assertSame(d.enumerable, false, prop + " enumerable");
        assertSame(d.configurable, true, prop + " configurable");
    }

    // OrdinaryCreateFromConstructor(newTarget) MUST run BEFORE ToString(message)
    function CustomError() {}
    var stepOrder = [];
    var customProto = {
        set message(v) { stepOrder.push("setter-must-not-run"); },
        set error(v) { stepOrder.push("setter-must-not-run"); },
        set suppressed(v) { stepOrder.push("setter-must-not-run"); }
    };
    var NewTargetProxy = new Proxy(CustomError, {
        get: function (t, p) {
            if (p === "prototype") {
                stepOrder.push("newTarget.prototype");
                throw new RangeError("proto-first");
            }
            return t[p];
        }
    });
    assertThrows(RangeError, function () {
        Reflect.construct(SuppressedError, [1, 2, {
            toString: function () {
                stepOrder.push("message.toString");
                throw new SyntaxError("msg-second");
            }
        }], NewTargetProxy);
    }, "OrdinaryCreateFromConstructor before ToString(message)");
    assertArrayEqual(stepOrder, ["newTarget.prototype"], "prototype looked up before ToString(message)");

    // CreateNonEnumerableDataPropertyOrThrow must NOT invoke prototype setters
    var setterCheckProto = Object.create(Error.prototype);
    Object.defineProperty(setterCheckProto, "message", { set: function () { fail("message setter called"); } });
    Object.defineProperty(setterCheckProto, "error", { set: function () { fail("error setter called"); } });
    Object.defineProperty(setterCheckProto, "suppressed", { set: function () { fail("suppressed setter called"); } });
    var NonSetterCtor = function () {};
    NonSetterCtor.prototype = setterCheckProto;
    var seCustom = Reflect.construct(SuppressedError, [11, 22, "m", { cause: "ignored" }], NonSetterCtor);
    assertSame(Object.getPrototypeOf(seCustom), setterCheckProto, "custom prototype preserved");
    assertSame(seCustom.message, "m", "own message set without invoking setter");
    assertSame(seCustom.error, 11, "own error set without invoking setter");
    assertSame(seCustom.suppressed, 22, "own suppressed set without invoking setter");
    assertSame(Object.prototype.hasOwnProperty.call(seCustom, "cause"), false, "SuppressedError ignores 4th options arg");

    // Cross-realm NewTarget fallback
    var other = createOtherRealm();
    if (other) {
        var otherNT = new other.Function();
        otherNT.prototype = null;
        var seCross = Reflect.construct(SuppressedError, ["e", "s", "m"], otherNT);
        assertSame(Object.getPrototypeOf(seCross), other.SuppressedError.prototype, "cross-realm SuppressedError prototype fallback");
    }
})();

// 3. %IteratorPrototype%[Symbol.dispose]
(function testIteratorSymbolDispose() {
    var IteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
    var fn = IteratorPrototype[Symbol.dispose];
    assertSame(typeof fn, "function", "IteratorPrototype[Symbol.dispose] is function");
    assertSame(fn.length, 0, "IteratorPrototype[Symbol.dispose].length");
    assertSame(fn.name, "[Symbol.dispose]", "IteratorPrototype[Symbol.dispose].name");
    assertThrows(TypeError, function () { new fn(); }, "not a constructor");
    assertThrows(TypeError, function () { fn.call(null); }, "null this");
    assertThrows(TypeError, function () { fn.call(undefined); }, "undefined this");

    var calls = 0;
    var argLen = -1;
    var seenThis = null;
    var iter = Object.create(IteratorPrototype);
    iter.return = function () {
        calls++;
        argLen = arguments.length;
        seenThis = this;
        return { value: 99, done: true };
    };
    assertSame(iter[Symbol.dispose](), undefined, "returns undefined");
    assertSame(calls, 1, "return called once");
    assertSame(argLen, 0, "return called with 0 arguments");
    assertSame(seenThis, iter, "return called with iter as this");

    // Primitive receiver preserves primitive `this` in GetV / GetMethod
    var observedGetterThisType = null;
    Object.defineProperty(Number.prototype, "return", {
        configurable: true,
        get: function () {
            "use strict";
            observedGetterThisType = typeof this;
            return undefined;
        }
    });
    try {
        assertSame(fn.call(42), undefined, "primitive receiver with undefined return");
        assertSame(observedGetterThisType, "number", "primitive receiver preserved in getter");
    } finally {
        delete Number.prototype.return;
    }

    // Non-callable return throws TypeError
    assertThrows(TypeError, function () {
        fn.call({ return: 123 });
    }, "non-callable return throws TypeError");
})();

// 4. %AsyncIteratorPrototype%[Symbol.asyncDispose]
addAsyncTest("AsyncIteratorPrototype[Symbol.asyncDispose]", async function () {
    async function* ag() {}
    var AsyncIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(ag.prototype));
    var fn = AsyncIteratorPrototype[Symbol.asyncDispose];
    assertSame(typeof fn, "function", "AsyncIteratorPrototype[Symbol.asyncDispose] is function");
    assertSame(fn.length, 0, "AsyncIteratorPrototype[Symbol.asyncDispose].length");
    assertSame(fn.name, "[Symbol.asyncDispose]", "AsyncIteratorPrototype[Symbol.asyncDispose].name");
    assertThrows(TypeError, function () { new fn(); }, "not a constructor");

    await expectReject(fn.call(null), TypeError, "null this rejects");
    await expectReject(fn.call(undefined), TypeError, "undefined this rejects");
    await expectReject(fn.call({ get return() { throw new RangeError("getter"); } }), RangeError, "throwing getter rejects");
    await expectReject(fn.call({ return: 42 }), TypeError, "non-callable return rejects");
    await expectReject(fn.call({ return: function () { throw new ReferenceError("sync"); } }), ReferenceError, "throwing return rejects");

    class CustomErr extends Error {}
    await expectReject(fn.call({ return: function () { return Promise.reject(new CustomErr("rej")); } }), CustomErr, "rejected promise from return rejects");

    var calls = 0;
    var argLen = -1;
    var res = await fn.call({
        return: async function () {
            calls++;
            argLen = arguments.length;
            return { value: "ignored", done: true };
        }
    });
    assertSame(res, undefined, "resolved value unwrapped to undefined");
    assertSame(calls, 1, "async return called once");
    assertSame(argLen, 0, "async return called with 0 args");
});

// 5. DisposableStack: ordering, ReferenceError vs TypeError, reentrancy, throw undefined, move, cross-realm, JIT loops
(function testDisposableStack() {
    assertThrows(TypeError, function () { DisposableStack(); }, "DisposableStack requires new");
    assertSame(DisposableStack.prototype[Symbol.dispose], DisposableStack.prototype.dispose, "[Symbol.dispose] === dispose");
    assertSame(DisposableStack.prototype[Symbol.toStringTag], "DisposableStack", "toStringTag");

    // Brand checks
    var stackProtoDesc = Object.getOwnPropertyDescriptor(DisposableStack.prototype, "disposed");
    assertThrows(TypeError, function () { stackProtoDesc.get.call({}); }, "disposed brand check");
    assertThrows(TypeError, function () { DisposableStack.prototype.use.call({}, null); }, "use brand check");
    assertThrows(TypeError, function () { DisposableStack.prototype.adopt.call({}, 1, function () {}); }, "adopt brand check");
    assertThrows(TypeError, function () { DisposableStack.prototype.defer.call({}, function () {}); }, "defer brand check");
    assertThrows(TypeError, function () { DisposableStack.prototype.dispose.call({}); }, "dispose brand check");
    assertThrows(TypeError, function () { DisposableStack.prototype.move.call({}); }, "move brand check");

    // ReferenceError on disposed stack MUST take precedence over TypeError for invalid args
    var disposedStack = new DisposableStack();
    disposedStack.dispose();
    assertSame(disposedStack.disposed, true, "disposed is true");
    assertThrows(ReferenceError, function () { disposedStack.use(42); }, "use on disposed stack throws ReferenceError before non-object check");
    assertThrows(ReferenceError, function () { disposedStack.use({}); }, "use on disposed stack throws ReferenceError before GetMethod");
    assertThrows(ReferenceError, function () { disposedStack.adopt(1, "not-callable"); }, "adopt on disposed stack throws ReferenceError before IsCallable");
    assertThrows(ReferenceError, function () { disposedStack.defer("not-callable"); }, "defer on disposed stack throws ReferenceError before IsCallable");
    assertThrows(ReferenceError, function () { disposedStack.move(); }, "move on disposed stack throws ReferenceError");
    assertSame(disposedStack.dispose(), undefined, "dispose on disposed stack is no-op");

    // Reentrancy & disposed === true immediately on entering dispose()
    var reentLog = [];
    var stack = new DisposableStack();
    assertSame(stack.disposed, false, "initially not disposed");
    stack.defer(function () {
        reentLog.push("d1:" + stack.disposed);
        assertSame(stack.dispose(), undefined, "reentrant dispose() returns undefined");
        assertThrows(ReferenceError, function () { stack.use(null); }, "reentrant use() throws ReferenceError");
        assertThrows(ReferenceError, function () { stack.adopt(1, function () {}); }, "reentrant adopt() throws ReferenceError");
        assertThrows(ReferenceError, function () { stack.defer(function () {}); }, "reentrant defer() throws ReferenceError");
        assertThrows(ReferenceError, function () { stack.move(); }, "reentrant move() throws ReferenceError");
    });
    stack.adopt("val", function (v) {
        "use strict";
        reentLog.push("adopt:" + v + ":" + arguments.length + ":" + (this === undefined) + ":" + stack.disposed);
    });
    stack.use({
        [Symbol.dispose]: function () {
            reentLog.push("use:" + stack.disposed);
        }
    });
    stack.dispose();
    assertArrayEqual(
        reentLog,
        ["use:true", "adopt:val:1:true:true", "d1:true"],
        "LIFO order and immediate disposed=true"
    );

    // throw undefined single & multiple -> SuppressedError(undefined, undefined)
    var sUndef1 = new DisposableStack();
    sUndef1.defer(function () { throw undefined; });
    var caughtUndef = "sentinel";
    try {
        sUndef1.dispose();
    } catch (e) {
        caughtUndef = e;
    }
    assertSame(caughtUndef, undefined, "single throw undefined preserved");

    var sUndef2 = new DisposableStack();
    sUndef2.defer(function () { throw undefined; });
    sUndef2.defer(function () { throw undefined; });
    var caughtSupUndef = null;
    try {
        sUndef2.dispose();
    } catch (e) {
        caughtSupUndef = e;
    }
    assertTrue(caughtSupUndef instanceof SuppressedError, "two throw undefined -> SuppressedError");
    assertSame(caughtSupUndef.error, undefined, "SuppressedError.error === undefined");
    assertSame(caughtSupUndef.suppressed, undefined, "SuppressedError.suppressed === undefined");

    // move() & subclassing
    class SubStack extends DisposableStack {}
    var sub = new SubStack();
    var movedLog = [];
    sub.defer(function () { movedLog.push("a"); });
    sub.defer(function () { movedLog.push("b"); });
    var moved = sub.move();
    assertSame(sub.disposed, true, "source disposed after move");
    assertSame(moved.disposed, false, "moved stack pending");
    assertTrue(moved instanceof DisposableStack && !(moved instanceof SubStack), "move returns base DisposableStack");
    assertArrayEqual(movedLog, [], "move does not dispose");
    moved.dispose();
    assertArrayEqual(movedLog, ["b", "a"], "moved stack disposes in LIFO");
    assertThrows(ReferenceError, function () { sub.move(); }, "second move throws ReferenceError");

    // Cross-realm & GC + JIT loop stress
    var other = createOtherRealm();
    if (other) {
        other.rStack.use(other.makeResource("r1"));
        DisposableStack.prototype.use.call(other.rStack, {
            [Symbol.dispose]: function () { other.rDisposed.push("local-on-other"); }
        });
        var movedOther = DisposableStack.prototype.move.call(other.rStack);
        assertSame(other.rStack.disposed, true, "cross-realm source disposed after move");
        assertTrue(movedOther instanceof DisposableStack, "movedOther belongs to local DisposableStack realm");
        movedOther.dispose();
        assertArrayEqual(other.rDisposed, ["local-on-other", "r1"], "cross-realm disposal order");
    }

    // Loop to trigger JIT loop-body compilation & GC
    for (var k = 0; k < 200; k++) {
        var loopStack = new DisposableStack();
        var count = 0;
        loopStack.adopt(k, function (v) { count += v; });
        loopStack.defer(function () { count += 1; });
        loopStack.dispose();
        assertSame(count, k + 1, "loop DisposableStack iteration " + k);
        if ((k % 50) === 0) {
            gcNow();
        }
    }
})();

// 6. AsyncDisposableStack: Promise rejection on invalid `this`, ReferenceError precedence,
// sync fallback ignoring unresolved promise, sequential LIFO awaits, null/undefined tick count,
// throw undefined, move, cross-realm, and GC stress across async ticks.
addAsyncTest("AsyncDisposableStack comprehensive semantics & GC stress", async function () {
    assertThrows(TypeError, function () { AsyncDisposableStack(); }, "AsyncDisposableStack requires new");
    assertSame(AsyncDisposableStack.prototype[Symbol.asyncDispose], AsyncDisposableStack.prototype.disposeAsync, "[Symbol.asyncDispose] === disposeAsync");
    assertSame(AsyncDisposableStack.prototype[Symbol.toStringTag], "AsyncDisposableStack", "toStringTag");

    // disposeAsync MUST reject with TypeError rather than throwing synchronously
    await expectReject(AsyncDisposableStack.prototype.disposeAsync.call(undefined), TypeError, "disposeAsync(undefined) rejects");
    await expectReject(AsyncDisposableStack.prototype.disposeAsync.call(null), TypeError, "disposeAsync(null) rejects");
    await expectReject(AsyncDisposableStack.prototype.disposeAsync.call({}), TypeError, "disposeAsync({}) rejects");
    await expectReject(AsyncDisposableStack.prototype.disposeAsync.call(new DisposableStack()), TypeError, "disposeAsync(syncStack) rejects");

    // ReferenceError precedence on disposed AsyncDisposableStack
    var ds = new AsyncDisposableStack();
    var pInit = ds.disposeAsync();
    assertSame(ds.disposed, true, "disposed immediately true before await");
    assertThrows(ReferenceError, function () { ds.use(42); }, "async use on disposed throws ReferenceError");
    assertThrows(ReferenceError, function () { ds.adopt(1, 2); }, "async adopt on disposed throws ReferenceError");
    assertThrows(ReferenceError, function () { ds.defer(2); }, "async defer on disposed throws ReferenceError");
    assertThrows(ReferenceError, function () { ds.move(); }, "async move on disposed throws ReferenceError");
    assertSame(await pInit, undefined, "disposeAsync resolves to undefined");
    assertSame(await ds.disposeAsync(), undefined, "second disposeAsync resolves to undefined");

    // Sync [Symbol.dispose] fallback in AsyncDisposableStack MUST NOT await returned unresolved promise
    var unresolved = new Promise(function () {});
    var syncFallbackRan = false;
    var sSyncFallback = new AsyncDisposableStack();
    sSyncFallback.use({
        [Symbol.asyncDispose]: null,
        [Symbol.dispose]: function () {
            syncFallbackRan = true;
            return unresolved;
        }
    });
    assertSame(await sSyncFallback.disposeAsync(), undefined, "sync fallback ignores unresolved promise");
    assertSame(syncFallbackRan, true, "sync fallback executed");

    // Sync [Symbol.dispose] fallback throwing rejects disposeAsync with thrown error
    var sSyncThrow = new AsyncDisposableStack();
    sSyncThrow.defer(function () { throw undefined; });
    sSyncThrow.use({
        [Symbol.dispose]: function () { throw "sync-fallback-err"; }
    });
    var syncSup = await expectReject(sSyncThrow.disposeAsync(), SuppressedError, "sync fallback throw chains SuppressedError");
    assertSame(syncSup.error, undefined, "later throw undefined is error");
    assertSame(syncSup.suppressed, "sync-fallback-err", "earlier sync fallback error is suppressed");

    // Sequential LIFO async ordering + needsAwait/hasAwaited microtask precision
    var seq = [];
    var sNullDefer = new AsyncDisposableStack();
    sNullDefer.defer(function () { seq.push("sync-defer-inside-disposeAsync"); });
    sNullDefer.use(null);
    sNullDefer.use(undefined);
    var pNullDefer = sNullDefer.disposeAsync().then(function () {
        seq.push("dispose-done");
    });
    seq.push("after-call");
    await Promise.all([
        Promise.resolve().then(function () { seq.push("tick1"); }),
        pNullDefer
    ]);
    assertArrayEqual(seq, ["sync-defer-inside-disposeAsync", "after-call", "tick1", "dispose-done"], "null/undefined await once at end after sync defer");

    // Sequential await: second disposer only starts after first promise resolves
    var asyncOrder = [];
    var sSeq = new AsyncDisposableStack();
    sSeq.use({
        [Symbol.asyncDispose]: function () {
            asyncOrder.push("first-start");
            return Promise.resolve().then(function () { asyncOrder.push("first-end"); });
        }
    });
    sSeq.use({
        [Symbol.asyncDispose]: function () {
            asyncOrder.push("second-start");
            return Promise.resolve().then(function () {
                asyncOrder.push("second-tick1");
            }).then(function () {
                asyncOrder.push("second-end");
            });
        }
    });
    await sSeq.disposeAsync();
    assertArrayEqual(asyncOrder, ["second-start", "second-tick1", "second-end", "first-start", "first-end"], "sequential LIFO async disposal");

    // Reentrancy during async disposal
    var reentAsyncCount = 0;
    var sReent = new AsyncDisposableStack();
    sReent.use({
        [Symbol.asyncDispose]: async function () {
            reentAsyncCount++;
            assertSame(sReent.disposed, true, "disposed during async disposer");
            assertThrows(ReferenceError, function () { sReent.use(null); }, "reentrant async use throws ReferenceError");
            assertSame(await sReent.disposeAsync(), undefined, "reentrant disposeAsync returns resolved undefined");
        }
    });
    await sReent.disposeAsync();
    assertSame(reentAsyncCount, 1, "async disposer ran once");

    // GC stress: drop all JS references to stack, run CollectGarbage across pending steps
    var gcSurviveLog = [];
    var releaseFirst = null;
    function launchDetachedDispose() {
        var transient = new AsyncDisposableStack();
        var tag = { marker: "survived-gc" };
        transient.adopt(tag, async function (v) {
            gcNow();
            gcSurviveLog.push(v.marker);
        });
        transient.defer(function () {
            gcNow();
            throw new RangeError("gc-err-2");
        });
        transient.use({
            [Symbol.asyncDispose]: function () {
                return new Promise(function (resolve) {
                    releaseFirst = resolve;
                });
            }
        });
        return transient.disposeAsync();
    }
    var detachedPromise = launchDetachedDispose();
    for (var g = 0; g < 1000; g++) {
        ({ payload: [g, g + 1] });
    }
    gcNow();
    gcNow();
    assertTrue(typeof releaseFirst === "function", "first disposer started");
    releaseFirst();
    gcNow();
    await expectReject(detachedPromise, RangeError, "detached stack error survives GC");
    assertArrayEqual(gcSurviveLog, ["survived-gc"], "detached adopt resource survives GC");

    // Cross-realm AsyncDisposableStack
    var other = createOtherRealm();
    if (other) {
        other.rAsyncStack.use(other.makeAsyncResource("ar1"));
        AsyncDisposableStack.prototype.use.call(other.rAsyncStack, {
            [Symbol.asyncDispose]: async function () {
                await Promise.resolve();
                other.rDisposed.push("local-async");
            }
        });
        var movedAsync = AsyncDisposableStack.prototype.move.call(other.rAsyncStack);
        assertSame(other.rAsyncStack.disposed, true, "cross-realm async source disposed after move");
        assertTrue(movedAsync instanceof AsyncDisposableStack, "movedAsync in local realm");
        await movedAsync.disposeAsync();
        assertArrayEqual(other.rDisposed, ["local-async", "ar1"], "cross-realm async LIFO disposal");
    }

    // JIT loop stress for AsyncDisposableStack
    for (var j = 0; j < 60; j++) {
        var ls = new AsyncDisposableStack();
        var hit = 0;
        ls.adopt(j, async function (val) {
            await Promise.resolve();
            hit = val + 1;
        });
        await ls.disposeAsync();
        assertSame(hit, j + 1, "loop AsyncDisposableStack iteration " + j);
    }
});

Promise.all(asyncTests).then(
    function () {
        print("pass");
    },
    function (err) {
        fail("unhandled error: " + (err && err.stack ? err.stack : err));
    }
);
