//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// FinalizationRegistry tests use the ch host's real -CollectGarbage and
// WScript.SetTimeout job boundaries. The test intentionally does not fake GC
// or callback timing: targets are collected by the recycler, and cleanup jobs
// are drained by the host message queue as separate host calls.

var A_COUNT = 64;
var B_COUNT = 64;
var TOKEN_COUNT = 32;
var MULTI_COUNT = 32;
var PRIMITIVE_COUNT = 32;
var CLEAR_ALLOWANCE = 8;

var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("finalization-registry: FAIL: " + message);
        WScript.Quit(1);
    }
}

function throwTypeError(fn, message) {
    try {
        fn();
    } catch (e) {
        check(e instanceof TypeError, message + " (got " + e + ")");
        return;
    }
    check(false, message + " (did not throw)");
}

// Constructor and call state.
check(typeof FinalizationRegistry === "function", "global constructor");
check(FinalizationRegistry.length === 1, "constructor length");
check(FinalizationRegistry.name === "FinalizationRegistry", "constructor name");
throwTypeError(function () { FinalizationRegistry(function () {}); }, "constructor call without new");
throwTypeError(function () { new FinalizationRegistry(); }, "missing callback");
throwTypeError(function () { new FinalizationRegistry({}); }, "non-callable callback");

var constructorProtoDesc = Object.getOwnPropertyDescriptor(FinalizationRegistry, "prototype");
check(constructorProtoDesc.writable === false, "prototype writable");
check(constructorProtoDesc.enumerable === false, "prototype enumerable");
check(constructorProtoDesc.configurable === false, "prototype configurable");

var simple = new FinalizationRegistry(function () {});
check(Object.getPrototypeOf(simple) === FinalizationRegistry.prototype, "instance prototype");
check(simple instanceof FinalizationRegistry, "instanceof");
check(Object.prototype.toString.call(simple) === "[object FinalizationRegistry]", "toStringTag");

var register = FinalizationRegistry.prototype.register;
var unregister = FinalizationRegistry.prototype.unregister;
check(typeof register === "function" && typeof unregister === "function", "prototype methods");
check(register.length === 2 && register.name === "register", "register shape");
check(unregister.length === 1 && unregister.name === "unregister", "unregister shape");
check(Symbol.toStringTag in FinalizationRegistry.prototype, "toStringTag property");
check(FinalizationRegistry.prototype[Symbol.toStringTag] === "FinalizationRegistry", "toStringTag value");

function checkMethodDescriptor(name, method) {
    var desc = Object.getOwnPropertyDescriptor(FinalizationRegistry.prototype, name);
    check(desc.writable === true, name + " writable");
    check(desc.enumerable === false, name + " enumerable");
    check(desc.configurable === true, name + " configurable");
}
checkMethodDescriptor("register", register);
checkMethodDescriptor("unregister", unregister);
throwTypeError(function () { new register({}); }, "register is not a constructor");
throwTypeError(function () { new unregister({}); }, "unregister is not a constructor");

// Brand checks and argument validation.
throwTypeError(function () { register.call({}, {}); }, "register brand");
throwTypeError(function () { unregister.call({}, {}); }, "unregister brand");
throwTypeError(function () { simple.register(1); }, "primitive target");
throwTypeError(function () { simple.register(null); }, "null target");
throwTypeError(function () { simple.register(Symbol.for("registered")); }, "registered symbol target");
throwTypeError(function () { simple.register({}, 1, 1); }, "primitive token");
throwTypeError(function () { simple.register({}, 1, Symbol.for("registered")); }, "registered symbol token");
var sameTarget = {};
throwTypeError(function () { simple.register(sameTarget, sameTarget); }, "target equals heldValue");
var sameSymbol = Symbol("same");
throwTypeError(function () { simple.register(sameSymbol, sameSymbol); }, "symbol target equals heldValue");

// Eligibility, duplicate registration, held value types, and unregister.
var symbolTarget = Symbol("weak");
check(simple.register(symbolTarget) === undefined, "unregistered symbol target");
check(simple.register(Symbol.hasInstance) === undefined, "well-known symbol target");
check(simple.register({}, undefined) === undefined, "undefined heldValue");
check(simple.register({}, null) === undefined, "null heldValue");
check(simple.register({}, "text") === undefined, "string heldValue");
check(simple.register({}, Symbol("held")) === undefined, "symbol heldValue");

var token1 = {};
var token2 = Symbol("token");
var target1 = {};
var target2 = {};
var target3 = {};
simple.register(target1, 1, token1);
simple.register(target1, 2, token1);
simple.register(target2, 3, token2);
simple.register(target3, 4, token2);
check(simple.unregister(token1) === true, "unregister token removes all duplicates");
check(simple.unregister(token1) === false, "unregister token is idempotent");
check(simple.unregister(token2) === true, "symbol token removes all matches");
check(simple.unregister(token2) === false, "symbol token is idempotent");
check(simple.unregister(target1) === false, "target is not a token");
check(simple.register(target1, 5) === undefined, "registration without token");
check(simple.unregister(token1) === false, "no cross-talk from previous token");

// Registration factories used by the cross-job checks. Each factory keeps
// only the registry and its cleanup results; target/heldValue/token references
// are dropped before the first collection.
var dead = new FinalizationRegistry(function (heldValue) {
    "use strict";
    deadSeen.push(heldValue);
    check(this === undefined, "cleanup this");
    check(arguments.length === 1, "cleanup argument count");
});
var deadSeen = [];

(function registerDeadTargets() {
    for (var i = 0; i < A_COUNT; i++) {
        dead.register({ index: i }, { marker: i });
    }
})();

var primitiveSeen = [];
var primitiveRegistry = new FinalizationRegistry(function (heldValue) {
    primitiveSeen.push(heldValue);
});
(function registerPrimitiveHeldValues() {
    for (var i = 0; i < PRIMITIVE_COUNT; i++) {
        primitiveRegistry.register({}, "held-" + i);
    }
})();

// Held values are objects that are not otherwise referenced. The callback can
// only observe them if the cell kept them strongly until the cleanup job.
function checkDeadHeldValues() {
    var objects = 0;
    for (var i = 0; i < deadSeen.length; i++) {
        var value = deadSeen[i];
        if (value !== null && typeof value === "object" && typeof value.marker === "number") {
            objects++;
        }
    }
    return objects;
}

// B targets have a WeakRef but no strong reference after this function. The
// WeakRef constructor keeps them alive through job 1; job 2 collects them and
// proves the registry itself did not retain the target.
var weakTargets = [];
var weakRefs = [];
var weakRegistry = new FinalizationRegistry(function (heldValue) {
    weakSeen.push(heldValue);
});
var weakSeen = [];
(function registerWeakTargets() {
    for (var i = 0; i < B_COUNT; i++) {
        var target = { index: i };
        weakTargets.push(target);
        weakRefs.push(new WeakRef(target));
        weakRegistry.register(target, { marker: i });
    }
})();
weakTargets = null;

// Strongly reachable target: no callback until it is dropped in job 2.
var liveTarget = {};
var liveHeld = { marker: "live" };
var liveSeen = 0;
var liveRegistry = new FinalizationRegistry(function (heldValue) {
    check(heldValue === liveHeld, "live held value identity");
    liveSeen++;
});
liveRegistry.register(liveTarget, liveHeld);

// Unregistered before any collection: no callback ever.
var removedTarget = {};
var removedHeld = { marker: "removed" };
var removedToken = {};
var removedSeen = 0;
var removedRegistry = new FinalizationRegistry(function () { removedSeen++; });
removedRegistry.register(removedTarget, removedHeld, removedToken);
removedTarget = null;
removedRegistry.unregister(removedToken);

// Unregistered by a 0 ms host task before the queued cleanup job runs.
var timerToken = {};
var timerTarget = {};
var timerHeld = { marker: "timer-unregistered" };
var timerSeen = 0;
var timerRegistry = new FinalizationRegistry(function () { timerSeen++; });
timerRegistry.register(timerTarget, timerHeld, timerToken);
timerTarget = null;
WScript.SetTimeout(function unregisterBeforeCleanup() {
    check(timerRegistry.unregister(timerToken) === true, "timer unregister");
}, 0);

// Duplicate registrations of the same target produce two independent cells,
// so a collected target invokes the cleanup callback twice.
var duplicateSeen = [];
var duplicateRegistry = new FinalizationRegistry(function (v) { duplicateSeen.push(v); });
(function registerDuplicates() {
    var target = {};
    duplicateRegistry.register(target, "dup-1");
    duplicateRegistry.register(target, "dup-2");
})();

// A live target keeps its cell alive; the heldValue is only referenced by the
// cell and a WeakRef, so job 2's collection proves the cell holds it strongly.
var persistTarget = {};
var persistToken = {};
var persistHeldRef = null;
var persistRegistry = new FinalizationRegistry(function () {});
(function registerPersistHeldValue() {
    var held = { marker: "persist" };
    persistHeldRef = new WeakRef(held);
    persistRegistry.register(persistTarget, held, persistToken);
})();

// A throwing cleanup callback must not prevent later host tasks. The throw is
// reported by the host job; the flag proves the callback ran.
var throwSeen = false;
var throwRegistry = new FinalizationRegistry(function () {
    throwSeen = true;
    throw new Error("expected cleanup callback failure");
});
throwRegistry.register({}, "throw");

// Multiple registries and registrations.
var multiA = [];
var multiB = [];
var registryA = new FinalizationRegistry(function (v) { multiA.push(v); });
var registryB = new FinalizationRegistry(function (v) { multiB.push(v); });
(function registerMultiple() {
    for (var i = 0; i < MULTI_COUNT; i++) {
        registryA.register({ index: i }, "a" + i);
        registryB.register({ index: i }, "b" + i);
    }
})();
registryB.register(Symbol("target"), "symbol");

// Token and heldValue lifetime in a live cell. The target stays strongly
// reachable, so the cell is not cleaned; tokens are weak and must clear after
// the job-1 kept set is released.
var tokenRefs = [];
var liveTokenTargets = [];
var tokenRegistry = new FinalizationRegistry(function () {});
(function registerLiveTokens() {
    for (var i = 0; i < TOKEN_COUNT; i++) {
        var token = { index: i };
        var target = { index: i };
        tokenRefs.push(new WeakRef(token));
        liveTokenTargets.push(target);
        tokenRegistry.register(target, "live-token", token);
    }
})();

// Real collections in job 1. Targets in `dead`, `timer`, `throw`, `multi`,
// and `removed` are collectible now; `weak` and `live` targets stay alive
// until job 2's collection.
CollectGarbage();
CollectGarbage();

WScript.SetTimeout(function job2() {
    // Job-1 cleanup jobs ran before this timer (their host tasks were queued at
    // the job-1 boundary with no delay). The throwing callback is one of them.
    check(deadSeen.length >= A_COUNT - CLEAR_ALLOWANCE, "dead target cleanup jobs");
    check(primitiveSeen.length >= PRIMITIVE_COUNT - CLEAR_ALLOWANCE, "primitive heldValues");
    check(checkDeadHeldValues() >= A_COUNT - CLEAR_ALLOWANCE, "object heldValues strongly held");
    check(throwSeen, "throwing cleanup callback ran");
    check(duplicateSeen.length === 2 && duplicateSeen[0] === "dup-1" && duplicateSeen[1] === "dup-2",
        "duplicate registrations produce two cleanup callbacks");
    check(typeof persistHeldRef.deref() === "object", "heldValue strongly held while target is alive");
    check(multiA.length >= MULTI_COUNT - CLEAR_ALLOWANCE && multiB.length >= MULTI_COUNT - CLEAR_ALLOWANCE, "multiple registries/registrations");
    check(timerSeen === 0, "unregister before cleanup prevents callback");
    check(removedSeen === 0, "unregister prevents callback");
    check(liveSeen === 0, "no cleanup while target strongly reachable");

    // Job 2 collection: the WeakRef constructor's keep-during-job is over, so
    // the targets have no strong owner, while the registry cell holds only a
    // weak handle. This is the target-not-strongly-retained check.
    CollectGarbage();
    CollectGarbage();

    var cleared = 0;
    for (var i = 0; i < weakRefs.length; i++) {
        if (weakRefs[i].deref() === undefined) {
            cleared++;
        }
    }
    check(cleared >= B_COUNT - CLEAR_ALLOWANCE, "registry does not retain targets: " + cleared);

    var weakTokensCleared = 0;
    for (var j = 0; j < tokenRefs.length; j++) {
        if (tokenRefs[j].deref() === undefined) {
            weakTokensCleared++;
        }
    }
    check(weakTokensCleared >= TOKEN_COUNT - CLEAR_ALLOWANCE, "registry does not retain unregister tokens: " + weakTokensCleared);

    // Drop the live target; job 2's boundary queues its cleanup callback.
    liveTarget = null;
    CollectGarbage();
    CollectGarbage();

    WScript.SetTimeout(function job3() {
        check(liveSeen === 1, "cleanup after target becomes unreachable");
        check(weakSeen.length >= B_COUNT - CLEAR_ALLOWANCE, "weak-target cleanup jobs");
        var weakHeldObjects = 0;
        for (var k = 0; k < weakSeen.length; k++) {
            var value = weakSeen[k];
            if (value !== null && typeof value === "object" && typeof value.marker === "number") {
                weakHeldObjects++;
            }
        }
        check(weakHeldObjects >= B_COUNT - CLEAR_ALLOWANCE, "weak-target heldValues strongly held");
        check(timerSeen === 0 && removedSeen === 0, "no callback after unregister");
        check(failures.length === 0, "no recorded failures");
        print("pass");
    }, 10);
}, 10);
