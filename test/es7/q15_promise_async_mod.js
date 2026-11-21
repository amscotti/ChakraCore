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
    print("q15_promise_async_mod: FAIL: " + message);
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
        if (typeof predicate === "function" && (predicate === TypeError || predicate === ReferenceError || predicate === Error || predicate.prototype instanceof Error)) {
            assertTrue(err instanceof predicate, message + " (expected " + predicate.name + ", got " + String(err) + ")");
        } else if (typeof predicate === "function") {
            assertTrue(predicate(err), message + " (predicate failed for: " + String(err) + ")");
        } else {
            assertSame(err, predicate, message);
        }
        return err;
    }
}

// 1. Promise static combinators: GetPromiseResolve before GetIterator
addAsyncTest("Promise combinators GetPromiseResolve before GetIterator", async function () {
    var combinators = ["all", "allSettled", "race", "any"];
    for (var i = 0; i < combinators.length; i++) {
        var method = combinators[i];
        var sentinel = { errorFrom: method };
        var iterAccessed = 0;
        var iterable = {
            get [Symbol.iterator]() {
                iterAccessed += 1;
                return function () {
                    return { next: function () { return { done: true }; } };
                };
            }
        };

        var rejectedReason = null;
        var rejectCalls = 0;
        function CustomCtorThrowingResolve(executor) {
            executor(
                function () { fail("Promise." + method + " should not resolve"); },
                function (r) { rejectCalls += 1; rejectedReason = r; }
            );
        }
        Object.defineProperty(CustomCtorThrowingResolve, "resolve", {
            get: function () {
                throw sentinel;
            }
        });

        Promise[method].call(CustomCtorThrowingResolve, iterable);
        assertSame(rejectCalls, 1, "Promise." + method + " rejects custom capability when .resolve throws");
        assertSame(rejectedReason, sentinel, "Promise." + method + " passes .resolve error to reject");
        assertSame(iterAccessed, 0, "Promise." + method + " must not read Symbol.iterator when .resolve throws");

        var nonCallReason = null;
        var nonCallRejects = 0;
        function CustomCtorNonCallableResolve(executor) {
            executor(
                function () { fail("Promise." + method + " non-callable should not resolve"); },
                function (r) { nonCallRejects += 1; nonCallReason = r; }
            );
        }
        CustomCtorNonCallableResolve.resolve = 123;

        Promise[method].call(CustomCtorNonCallableResolve, iterable);
        assertSame(nonCallRejects, 1, "Promise." + method + " rejects custom capability when .resolve is non-callable");
        assertTrue(nonCallReason instanceof TypeError, "Promise." + method + " non-callable .resolve rejects with TypeError");
        assertSame(iterAccessed, 0, "Promise." + method + " must not read Symbol.iterator when .resolve is non-callable");
    }
});

// 2. Promise.prototype.finally uses PromiseResolve(C, result)
addAsyncTest("Promise.prototype.finally observable PromiseResolve", async function () {
    for (var iter = 0; iter < 5; iter++) {
        var ctorGets = 0;
        var thenCalls = 0;
        var inner = new Promise(function (resolve) { resolve("inner"); });
        var origThen = inner.then;
        inner.then = function () {
            thenCalls += 1;
            return origThen.apply(this, arguments);
        };
        Object.defineProperty(inner, "constructor", {
            get: function () {
                ctorGets += 1;
                return Promise;
            }
        });

        var res = await Promise.resolve("outer").finally(function () { return inner; });
        assertSame(res, "outer", "finally preserves outer fulfillment value");
        assertSame(ctorGets, 2, "finally reads inner.constructor in PromiseResolve and SpeciesConstructor");
        assertSame(thenCalls, 1, "finally calls inner.then once directly without double-wrapping");

        var rejInner = Promise.resolve("ok");
        var rejCtorGets = 0;
        var rejThenCalls = 0;
        var origRejThen = rejInner.then;
        rejInner.then = function () {
            rejThenCalls += 1;
            return origRejThen.apply(this, arguments);
        };
        Object.defineProperty(rejInner, "constructor", {
            get: function () {
                rejCtorGets += 1;
                return Promise;
            }
        });

        var reasonSentinel = { reason: iter };
        await expectReject(
            Promise.reject(reasonSentinel).finally(function () { return rejInner; }),
            reasonSentinel,
            "finally preserves outer rejection reason"
        );
        assertSame(rejCtorGets, 2, "finally reads rejInner.constructor in PromiseResolve and SpeciesConstructor");
        assertSame(rejThenCalls, 1, "finally calls rejInner.then once on rejected branch");
        gcNow();
    }
});

// 3. AsyncGenerator.prototype.return with broken promise (.constructor getter throws)
addAsyncTest("AsyncGenerator.prototype.return broken promise states and reentrancy", async function () {
    for (var iter = 0; iter < 4; iter++) {
        var err = { broken: iter };
        var broken = Promise.resolve(1);
        Object.defineProperty(broken, "constructor", {
            get: function () { throw err; }
        });

        async function* completedGen() {}
        var g1 = completedGen();
        var r1 = await g1.next();
        assertSame(r1.done, true, "completedGen finished");
        await expectReject(g1.return(broken), err, "completed state return(broken)");
        var after1 = await g1.next();
        assertSame(after1.done, true, "next() after completed return(broken) resolves done");

        var ranBody = false;
        async function* suspendedStartGen() {
            ranBody = true;
            yield 1;
        }
        var g2 = suspendedStartGen();
        await expectReject(g2.return(broken), err, "suspendedStart state return(broken)");
        assertSame(ranBody, false, "suspendedStart body did not run");
        var after2 = await g2.next();
        assertSame(after2.done, true, "next() after suspendedStart return(broken) resolves done");

        var caughtInYield = null;
        async function* tryCatchGen() {
            try {
                yield "first";
                fail("should not continue after throw into yield");
            } catch (e) {
                caughtInYield = e;
                return "recovered";
            }
        }
        var g3 = tryCatchGen();
        var first = await g3.next();
        assertSame(first.value, "first", "tryCatchGen yielded first");
        assertSame(first.done, false, "tryCatchGen not done");
        var ret3 = await g3.return(broken);
        assertSame(caughtInYield, err, "suspendedYield return(broken) throws into try/catch");
        assertSame(ret3.value, "recovered", "try/catch return value preserved");
        assertSame(ret3.done, true, "tryCatchGen done after recovery");

        // Reentrancy: .constructor getter enqueues g4.next() before throwing
        var reentrantErr = { reentrant: iter };
        async function* reentrantGen() {
            yield "ok";
        }
        var g4 = reentrantGen();
        await g4.next();
        var queuedNext = null;
        var reentrantBroken = Promise.resolve(1);
        Object.defineProperty(reentrantBroken, "constructor", {
            get: function () {
                queuedNext = g4.next();
                throw reentrantErr;
            }
        });
        await expectReject(g4.return(reentrantBroken), reentrantErr, "reentrant return(broken) rejects");
        var qRes = await queuedNext;
        assertSame(qRes.done, true, "queued reentrant next() drains cleanly after rejection");
        assertSame(qRes.value, undefined, "queued reentrant next() returns undefined");

        gcNow();
    }
});

// 4. Function.prototype.bind length ToIntegerOrInfinity & non-Number handling
(function testBoundFunctionLength() {
    function makeFnWithLength(lenDesc) {
        function f() {}
        if (lenDesc !== undefined) {
            Object.defineProperty(f, "length", lenDesc);
        } else {
            delete f.length;
        }
        return f;
    }

    assertSame(makeFnWithLength({ value: 4294967296, configurable: true }).bind().length, 4294967296, "2^32 length");
    assertSame(makeFnWithLength({ value: 4294967296, configurable: true }).bind(null, 1, 2).length, 4294967294, "2^32 - 2 length");
    assertSame(makeFnWithLength({ value: Number.MAX_SAFE_INTEGER, configurable: true }).bind(null, 1).length, Number.MAX_SAFE_INTEGER - 1, "MAX_SAFE_INTEGER - 1");
    assertSame(makeFnWithLength({ value: Infinity, configurable: true }).bind().length, Infinity, "Infinity length");
    assertSame(makeFnWithLength({ value: Infinity, configurable: true }).bind(null, 1, 2, 3).length, Infinity, "Infinity - 3 remains Infinity");
    assertSame(makeFnWithLength({ value: -Infinity, configurable: true }).bind().length, 0, "-Infinity clamps to +0");
    assertSame(makeFnWithLength({ value: NaN, configurable: true }).bind().length, 0, "NaN becomes +0");
    assertSame(makeFnWithLength({ value: -0, configurable: true }).bind().length, 0, "-0 normalizes to +0");
    assertSame(makeFnWithLength({ value: -5, configurable: true }).bind().length, 0, "-5 clamps to +0");
    assertSame(makeFnWithLength({ value: 2.9, configurable: true }).bind(null, 1).length, 1, "trunc(2.9) - 1 === 1");

    var badInputs = [
        "100",
        true,
        null,
        undefined,
        Symbol("sym"),
        { valueOf: function () { fail("must not call valueOf on non-Number length"); return 5; } }
    ];
    for (var i = 0; i < badInputs.length; i++) {
        assertSame(makeFnWithLength({ value: badInputs[i], configurable: true }).bind().length, 0, "non-Number length produces +0");
    }
    assertSame(makeFnWithLength(undefined).bind().length, 0, "missing length produces +0");
})();

// 5. Cross-realm derived constructor completion errors (Return non-object, Uninitialized this)
(function testCrossRealmDerivedConstructors() {
    if (typeof WScript === "undefined" || typeof WScript.LoadScript !== "function") {
        return;
    }

    var other = WScript.LoadScript(
        "var RetNull = class RetNull extends Object { constructor() { return null; } };\n" +
        "var RetNum = class RetNum extends Object { constructor() { return 42; } };\n" +
        "var Uninit = class Uninit extends Object { constructor() {} };\n" +
        "var RetUndef = class RetUndef extends Object { constructor() { return undefined; } };\n" +
        "var NonEmptyNoSuper = class NonEmptyNoSuper extends Object { constructor() { var a = 1; var b = a + 2; if (b === 0) return {}; } };\n" +
        "var BodyTdz = class BodyTdz extends Object { constructor() { let x = x; } };\n" +
        "var BodyThisBeforeSuper = class BodyThisBeforeSuper extends Object { constructor() { this.p = 1; } };\n" +
        "class InnerBadRet extends Object { constructor() { return 1; } }\n" +
        "var CallsInner = class CallsInner extends Object { constructor() { new InnerBadRet(); super(); } };\n" +
        "var BoundRetNullOther = RetNull.bind(null);\n" +
        "var BoundUninitOther = Uninit.bind(null);\n" +
        "var OtherTypeError = TypeError;\n" +
        "var OtherReferenceError = ReferenceError;\n",
        "samethread"
    );

    function assertFromCallerRealm(fn, ExpectedCallerErr, ExpectedOtherErr, label) {
        var threw = false;
        try {
            fn();
        } catch (e) {
            threw = true;
            assertTrue(e instanceof ExpectedCallerErr, label + " must be instanceof caller " + ExpectedCallerErr.name + " (got " + String(e) + ")");
            assertTrue(!(e instanceof ExpectedOtherErr), label + " must NOT be instanceof callee " + ExpectedOtherErr.name);
        }
        assertTrue(threw, label + " did not throw");
    }

    function assertFromOtherRealm(fn, ExpectedOtherErr, ExpectedCallerErr, label) {
        var threw = false;
        try {
            fn();
        } catch (e) {
            threw = true;
            assertTrue(e instanceof ExpectedOtherErr, label + " must be instanceof other " + ExpectedOtherErr.name + " (got " + String(e) + ")");
            assertTrue(!(e instanceof ExpectedCallerErr), label + " must NOT be instanceof caller " + ExpectedCallerErr.name);
        }
        assertTrue(threw, label + " did not throw");
    }

    var BoundRetNullSame = other.RetNull.bind(null);
    var BoundRetNullOther = other.BoundRetNullOther;
    var BoundUninitOther = other.BoundUninitOther;
    class DerivedSubCaller extends other.RetNull {
        constructor() {
            super();
        }
    }

    // Run 20 iterations so interpreter and JIT paths are both exercised.
    for (var i = 0; i < 20; i++) {
        assertFromCallerRealm(function () { new other.RetNull(); }, TypeError, other.OtherTypeError, "new other.RetNull #" + i);
        assertFromCallerRealm(function () { new other.RetNum(); }, TypeError, other.OtherTypeError, "new other.RetNum #" + i);
        assertFromCallerRealm(function () { Reflect.construct(other.RetNull, []); }, TypeError, other.OtherTypeError, "Reflect.construct RetNull #" + i);
        assertFromCallerRealm(function () { new BoundRetNullSame(); }, TypeError, other.OtherTypeError, "new BoundRetNullSame #" + i);
        assertFromCallerRealm(function () { new BoundRetNullOther(); }, TypeError, other.OtherTypeError, "new BoundRetNullOther #" + i);
        assertFromCallerRealm(function () { new DerivedSubCaller(); }, TypeError, other.OtherTypeError, "super() into other.RetNull #" + i);

        assertFromCallerRealm(function () { new other.Uninit(); }, ReferenceError, other.OtherReferenceError, "new other.Uninit #" + i);
        assertFromCallerRealm(function () { new other.RetUndef(); }, ReferenceError, other.OtherReferenceError, "new other.RetUndef #" + i);
        assertFromCallerRealm(function () { new other.NonEmptyNoSuper(); }, ReferenceError, other.OtherReferenceError, "new other.NonEmptyNoSuper #" + i);
        assertFromCallerRealm(function () { Reflect.construct(other.Uninit, []); }, ReferenceError, other.OtherReferenceError, "Reflect.construct Uninit #" + i);
        assertFromCallerRealm(function () { new BoundUninitOther(); }, ReferenceError, other.OtherReferenceError, "new BoundUninitOther #" + i);

        assertFromOtherRealm(function () { new other.BodyTdz(); }, other.OtherReferenceError, ReferenceError, "body TDZ stays in other realm #" + i);
        assertFromOtherRealm(function () { new other.BodyThisBeforeSuper(); }, other.OtherReferenceError, ReferenceError, "this before super stays in other realm #" + i);
        assertFromOtherRealm(function () { new other.CallsInner(); }, other.OtherTypeError, TypeError, "inner constructor throw stays in other realm #" + i);
    }
    gcNow();
})();

// 6. ModuleNamespace TDZ & Unambiguous export * as + import * as re-export
addAsyncTest("ModuleNamespace TDZ and unambiguous export * as", async function () {
    if (typeof WScript === "undefined" || typeof WScript.RegisterModuleSource !== "function") {
        return;
    }

    WScript.RegisterModuleSource("q15_tdz_entry.js",
        'import "q15_tdz_reader.js";\n' +
        'export let tdzVar = 99;\n'
    );
    WScript.RegisterModuleSource("q15_tdz_reader.js",
        'import * as ns from "q15_tdz_entry.js";\n' +
        'function captureError(fn) { try { fn(); return null; } catch (e) { return e; } }\n' +
        'globalThis.__q15TdzSnap = {\n' +
        '    ns: ns,\n' +
        '    hasIn: ("tdzVar" in ns),\n' +
        '    hasSymbolTagOwn: Object.prototype.hasOwnProperty.call(ns, Symbol.toStringTag),\n' +
        '    ownKeys: Reflect.ownKeys(ns),\n' +
        '    ownNames: Object.getOwnPropertyNames(ns),\n' +
        '    hasOwnErr: captureError(function () { Object.prototype.hasOwnProperty.call(ns, "tdzVar"); }),\n' +
        '    gopdErr: captureError(function () { Object.getOwnPropertyDescriptor(ns, "tdzVar"); }),\n' +
        '    keysErr: captureError(function () { Object.keys(ns); }),\n' +
        '    forInErr: captureError(function () { for (var k in ns) {} })\n' +
        '};\n'
    );

    var entryNs = await import("q15_tdz_entry.js");
    var snap = globalThis.__q15TdzSnap;
    assertTrue(snap && snap.ns === entryNs, "TDZ reader observed live namespace");
    assertSame(snap.hasIn, true, "'tdzVar' in ns does not throw and returns true");
    assertSame(snap.hasSymbolTagOwn, true, "ns.hasOwnProperty(Symbol.toStringTag) is true");
    assertSame(snap.ownNames.length, 1, "getOwnPropertyNames contains export");
    assertSame(snap.ownNames[0], "tdzVar", "getOwnPropertyNames returns tdzVar");
    assertSame(snap.ownKeys.length, 2, "Reflect.ownKeys includes tdzVar and Symbol.toStringTag");
    assertTrue(snap.hasOwnErr instanceof ReferenceError, "hasOwnProperty on TDZ throws ReferenceError");
    assertTrue(snap.gopdErr instanceof ReferenceError, "getOwnPropertyDescriptor on TDZ throws ReferenceError");
    assertTrue(snap.keysErr instanceof ReferenceError, "Object.keys on TDZ throws ReferenceError");
    assertTrue(snap.forInErr instanceof ReferenceError, "for-in on TDZ throws ReferenceError");

    assertSame(Object.prototype.hasOwnProperty.call(entryNs, "tdzVar"), true, "after init hasOwnProperty is true");
    assertSame(Object.getOwnPropertyDescriptor(entryNs, "tdzVar").value, 99, "after init descriptor value is 99");
    assertSame(Object.keys(entryNs)[0], "tdzVar", "after init Object.keys returns tdzVar");

    // Unambiguous export * as foo + import * as foo; export { foo }
    WScript.RegisterModuleSource("q15_ns_target.js", "export const answer = 42;\n");
    WScript.RegisterModuleSource("q15_ns_a.js", 'export * as foo from "q15_ns_target.js";\n');
    WScript.RegisterModuleSource("q15_ns_b.js", 'import * as foo from "q15_ns_target.js"; export { foo };\n');
    WScript.RegisterModuleSource("q15_ns_reexport.js", 'export * from "q15_ns_a.js";\nexport * from "q15_ns_b.js";\n');
    var reexp = await import("q15_ns_reexport.js");
    var target = await import("q15_ns_target.js");
    assertSame(reexp.foo, target, "export * as + import * as re-export resolves unambiguously to target namespace");
    assertSame(reexp.foo.answer, 42, "unambiguous namespace export preserves bindings");
});

Promise.all(asyncTests).then(
    function () {
        gcNow();
        print("pass");
    },
    function (err) {
        fail("unhandled runner error: " + (err && err.stack ? err.stack : err));
    }
);
