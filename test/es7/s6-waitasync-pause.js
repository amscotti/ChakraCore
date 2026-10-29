//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S6b/S6d flag-on surface: Atomics.pause and Atomics.waitAsync. Runs with
// -ESSharedArrayBuffer (doc/c2-s6-waitasync.md, doc/c2-s6-waitasync-timeout.md).
// The ch host installs the timer continuation callback, so finite positive
// timeouts resolve "timed-out" after ceil(timeout) ms (sub-millisecond values
// round up to 1 ms); notify wins over a pending timeout, the losing side
// no-ops, and NaN/+Infinity/omitted timeouts stay pending until notify.

var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("s6-waitasync-pause: FAIL: " + message);
    }
}

function throws(errorType, fn, message) {
    try {
        fn();
    } catch (e) {
        check(e instanceof errorType, message + " (threw " + e + ", expected " + errorType.name + ")");
        return;
    }
    check(false, message + " (did not throw " + errorType.name + ")");
}

function sameArray(view, expected, message) {
    check(view.length === expected.length, message + ": length " + view.length + " != " + expected.length);
    for (var i = 0; i < expected.length; i++) {
        check(view[i] === expected[i], message + "[" + i + "]: " + view[i] + " != " + expected[i]);
    }
}

function descriptor(object, name, writable, enumerable, configurable) {
    var d = Object.getOwnPropertyDescriptor(object, name);
    check(!!d, name + " descriptor exists");
    if (d) {
        check(d.writable === writable, name + ".writable");
        check(d.enumerable === enumerable, name + ".enumerable");
        check(d.configurable === configurable, name + ".configurable");
    }
}

// --- Atomics.pause ---------------------------------------------------------
try {
check(typeof Atomics.pause === "function", "Atomics.pause is a function");
check(Atomics.pause.length === 0, "Atomics.pause.length is 0");
check(Atomics.pause.name === "pause", "Atomics.pause.name is pause");
descriptor(Atomics, "pause", true, false, true);
descriptor(Atomics.pause, "length", false, false, true);
descriptor(Atomics.pause, "name", false, false, true);

var pauseBuffer = new SharedArrayBuffer(16);
var pauseView = new Int32Array(pauseBuffer);
Atomics.store(pauseView, 0, 0x12345678);
check(Atomics.pause() === undefined, "Atomics.pause() returns undefined");
check(Atomics.pause(1, "ignored", {}) === undefined, "Atomics.pause ignores extra arguments");
check(Atomics.pause({ valueOf: function () { check(false, "pause coerced an argument"); throw new Error("coerced"); } }) === undefined,
    "Atomics.pause does not coerce arguments");
check(Atomics.load(pauseView, 0) === 0x12345678, "Atomics.pause does not mutate the SAB");
sameArray(new Int32Array(pauseBuffer), [0x12345678, 0, 0, 0], "Atomics.pause leaves bytes unchanged");
throws(TypeError, function () { new Atomics.pause(); }, "Atomics.pause is not a constructor");

// --- Atomics.waitAsync shape ----------------------------------------------
check(typeof Atomics.waitAsync === "function", "Atomics.waitAsync is a function");
check(Atomics.waitAsync.length === 4, "Atomics.waitAsync.length is 4");
check(Atomics.waitAsync.name === "waitAsync", "Atomics.waitAsync.name is waitAsync");
descriptor(Atomics, "waitAsync", true, false, true);
descriptor(Atomics.waitAsync, "length", false, false, true);
descriptor(Atomics.waitAsync, "name", false, false, true);
throws(TypeError, function () { new Atomics.waitAsync(pauseView, 0, 0); }, "Atomics.waitAsync is not a constructor");

var sab = new SharedArrayBuffer(32);
var view = new Int32Array(sab, 8, 4);

// not-equal fast path: async false, direct string value.
Atomics.store(view, 0, 1);
var notEqual = Atomics.waitAsync(view, 0, 2);
check(notEqual.async === false, "not-equal async is false");
check(notEqual.value === "not-equal", "not-equal value");
check(typeof notEqual.value === "string", "not-equal value is not a promise");

// zero / negative / -Infinity timeouts are synchronous timed-out results.
[0, -0, -1, -1000.5, -Infinity, false, null].forEach(function (timeout) {
    Atomics.store(view, 1, 0);
    var r = Atomics.waitAsync(view, 1, 0, timeout);
    check(r.async === false, String(timeout) + " async is false");
    check(r.value === "timed-out", String(timeout) + " value is timed-out");
});

// Fractional small positive timeout is finite: it must not take a fast path.
Atomics.store(view, 2, 0);
var fractional = Atomics.waitAsync(view, 2, 0, 0.5);
check(fractional.async === true, "0.5 timeout is asynchronous");
check(fractional.value instanceof Promise, "0.5 timeout value is a promise");
Atomics.notify(view, 2, 1);

// Missing timeout is ToNumber(undefined) => NaN => infinite.
Atomics.store(view, 3, 0);
var missing = Atomics.waitAsync(view, 3, 0);
check(missing.async === true && missing.value instanceof Promise, "missing timeout is infinite");
Atomics.notify(view, 3, 1);

// The result object uses CreateDataPropertyOrThrow, not [[Set]]: an inherited
// setter must not run for either property.
var setterRan = false;
Object.defineProperty(Object.prototype, "async", {
    set: function () { setterRan = true; },
    configurable: true
});
var inherited = Atomics.waitAsync(view, 0, 2);
check(!setterRan, "inherited async setter did not run");
check(Object.prototype.hasOwnProperty.call(inherited, "async"), "result has own async property");
delete Object.prototype.async;

// Validation / coercion order.
throws(TypeError, function () { Atomics.waitAsync(1, 0, 0, 0); }, "non-object throws TypeError");
throws(TypeError, function () { Atomics.waitAsync({}, 0, 0, 0); }, "non-TypedArray throws TypeError");
throws(TypeError, function () { Atomics.waitAsync(new Int8Array(new SharedArrayBuffer(8)), 0, 0, 0); }, "non-Int32 throws TypeError");
throws(TypeError, function () { Atomics.waitAsync(new Int32Array(new ArrayBuffer(8)), 0, 0, 0); }, "non-shared Int32Array throws TypeError");
throws(RangeError, function () { Atomics.waitAsync(view, -1, 0, 0); }, "negative index throws RangeError");
throws(RangeError, function () { Atomics.waitAsync(view, 4, 0, 0); }, "out-of-range index throws RangeError");
Atomics.store(view, 0, 0);
var undefinedIndex = Atomics.waitAsync(view, undefined, 0, 0);
check(undefinedIndex.async === false && undefinedIndex.value === "timed-out", "undefined index defaults to zero");

var order = [];
var typeError = { valueOf: function () { order.push("index"); return 0; } };
throws(TypeError, function () { Atomics.waitAsync(new Int8Array(new SharedArrayBuffer(8)), typeError, 0, 0); }, "type validated before index");
check(order.length === 0, "index not coerced after type failure");

order = [];
throws(RangeError, function () {
    Atomics.waitAsync(view, { valueOf: function () { order.push("index"); return 99; } },
        { valueOf: function () { order.push("value"); return 0; } },
        { valueOf: function () { order.push("timeout"); return 0; } });
}, "index range error");
check(order.join(",") === "index", "value/timeout not coerced after index error, got " + order.join(","));

order = [];
throws(TypeError, function () {
    Atomics.waitAsync(view, 0,
        { valueOf: function () { order.push("value"); throw new TypeError("value"); } },
        { valueOf: function () { order.push("timeout"); return 0; } });
}, "value coercion error");
check(order.join(",") === "value", "timeout not coerced after value error, got " + order.join(","));

order = [];
throws(Error, function () {
    Atomics.waitAsync(view, 0, 0,
        { valueOf: function () { order.push("timeout"); throw new Error("timeout"); } });
}, "timeout coercion error");
check(order.join(",") === "timeout", "timeout coerced after value");

// Even the not-equal fast path coerces timeout first.
order = [];
throws(Error, function () {
    Atomics.store(view, 0, 1);
    Atomics.waitAsync(view, 0, 2, { valueOf: function () { order.push("timeout"); throw new Error("poison"); } });
}, "not-equal still coerces timeout");
check(order.join(",") === "timeout", "not-equal timeout coercion order");

// TypedArray length is captured before index coercion (2025 test262).
var growable = new SharedArrayBuffer(0, { maxByteLength: 4 });
var growView = new Int32Array(growable);
throws(RangeError, function () {
    Atomics.waitAsync(growView, { valueOf: function () { growable.grow(4); return 0; } }, 0, 0);
}, "length captured before index coercion");
check(growable.byteLength === 4, "index getter still grew the buffer");

// Detached non-shared view is rejected before any argument coercion.
var detached = new Int32Array(new ArrayBuffer(8));
ArrayBuffer.prototype.transfer.call(detached.buffer);
var poisoned = { valueOf: function () { check(false, "poisoned coercion ran"); return 0; } };
throws(TypeError, function () { Atomics.waitAsync(detached, poisoned, poisoned, poisoned); }, "detached view throws TypeError");

// --- Short calls and argument defaults -------------------------------------
throws(TypeError, function () { Atomics.waitAsync(); }, "waitAsync() throws TypeError");
throws(TypeError, function () { Atomics.waitAsync(null); }, "waitAsync(null) throws TypeError");
throws(TypeError, function () { Atomics.waitAsync(undefined); }, "waitAsync(undefined) throws TypeError");

var shortView = new Int32Array(new SharedArrayBuffer(8));
var shortA = Atomics.waitAsync(shortView);
var shortB = Atomics.waitAsync(shortView, 0);
var shortC = Atomics.waitAsync(shortView, 0, 0);
check(shortA.async === true && shortA.value instanceof Promise, "waitAsync(ta) defaults index/value to 0");
check(shortB.async === true && shortB.value instanceof Promise, "waitAsync(ta,0) defaults value to 0");
check(shortC.async === true && shortC.value instanceof Promise, "waitAsync(ta,0,0) defaults timeout to infinite");
check(Atomics.notify(shortView, 0, 3) === 3, "three short-call waiters are registered");
check(Atomics.waitAsync(shortView, 0, 0, 0).value === "timed-out", "explicit timeout 0 is timed-out");

// --- Count conversion and wake-all boundaries ------------------------------
var countView = new Int32Array(new SharedArrayBuffer(8));
function addCountWaiters(count) {
    var waiters = [];
    for (var i = 0; i < count; i++) {
        waiters.push(Atomics.waitAsync(countView, 0, 0, Infinity));
    }
    return waiters;
}

addCountWaiters(3);
check(Atomics.notify(countView, 0, Math.pow(2, 31)) === 3, "count 2^31 wakes all");
addCountWaiters(3);
check(Atomics.notify(countView, 0, Math.pow(2, 32) + 1) === 3, "count 2^32+1 wakes all");
addCountWaiters(3);
check(Atomics.notify(countView, 0, Number.MAX_VALUE) === 3, "count Number.MAX_VALUE wakes all");
addCountWaiters(1);
check(Atomics.notify(countView, 0, Number.POSITIVE_INFINITY) === 1, "count +Infinity wakes all");
check(Atomics.notify(countView, 0, 1) === 0, "wake-all consumed the waiter");
addCountWaiters(1);
check(Atomics.notify(countView, 0, NaN) === 0, "NaN count is ToIntegerOrInfinity 0");
check(Atomics.notify(countView, 0, 1) === 1, "NaN did not consume the waiter");
addCountWaiters(1);
check(Atomics.notify(countView, 0, Number.NEGATIVE_INFINITY) === 0, "-Infinity count is 0");
check(Atomics.notify(countView, 0, -1) === 0, "negative count is 0");
check(Atomics.notify(countView, 0, 1) === 1, "negative count did not consume the waiter");

// --- Same-thread cross-realm notify ----------------------------------------
var realmOutcome = "pending";
var realmView = null;
if (typeof $262 !== "undefined" && typeof $262.createRealm === "function") {
    var realm = $262.createRealm();
    var realmSab = new SharedArrayBuffer(16);
    var realmArray = new realm.global.Int32Array(realmSab);
    realmView = new Int32Array(realmSab);
    var realmWaiter = realm.global.Atomics.waitAsync(realmArray, 0, 0, Infinity);
    check(realmWaiter.async === true, "cross-realm waitAsync is asynchronous");
    realmWaiter.value.then(function (value) { realmOutcome = value; });
    check(Atomics.notify(realmView, 0, 1) === 1, "notify from realm A wakes realm B waiter");
    check(Atomics.notify(realmView, 0, 1) === 0, "cross-realm waiter is not double-resolved");
} else {
    check(false, "$262.createRealm is required for the cross-realm regression");
}

// --- FIFO notify order ------------------------------------------------------
var fifoView = new Int32Array(new SharedArrayBuffer(8));
var fifoOrder = [];
for (var fifoIndex = 0; fifoIndex < 5; fifoIndex++) {
    (function (index) {
        Atomics.waitAsync(fifoView, 0, 0, Infinity).value.then(function () { fifoOrder.push(index); });
    })(fifoIndex);
}
check(Atomics.notify(fifoView, 0, 5) === 5, "notify wakes five FIFO waiters");

// --- Finite positive timeouts (S6d) ----------------------------------------
// The ch host routes delayed timeout tasks into its message queue, so a
// finite positive timeout resolves "timed-out" after ceil(timeout) ms.
var finiteView = new Int32Array(new SharedArrayBuffer(32));

// A 1ms timeout resolves "timed-out".
var oneMs = Atomics.waitAsync(finiteView, 0, 0, 1);
check(oneMs.async === true && oneMs.value instanceof Promise, "1ms timeout is asynchronous");
var oneMsOutcome = "pending";
oneMs.value.then(function (v) { oneMsOutcome = v; }, function (e) { oneMsOutcome = "rejected:" + e; });

// Sub-millisecond timeouts round up to 1ms: async, never the sync fast path.
var subA = Atomics.waitAsync(finiteView, 1, 0, 0.5);
var subB = Atomics.waitAsync(finiteView, 2, 0, 1e-7);
check(subA.async === true && subA.value instanceof Promise, "0.5ms timeout is asynchronous");
check(subB.async === true && subB.value instanceof Promise, "1e-7ms timeout is asynchronous");
var subOutcomeA = "pending", subOutcomeB = "pending";
subA.value.then(function (v) { subOutcomeA = v; });
subB.value.then(function (v) { subOutcomeB = v; });

// Multiple finite waiters on one index all time out; a later notify counts 0.
var multiOutcomes = ["pending", "pending", "pending"];
for (var m = 0; m < 3; m++) {
    (function (i) {
        Atomics.waitAsync(finiteView, 4, 0, 2).value.then(function (v) { multiOutcomes[i] = v; });
    })(m);
}

// A pending timeout task survives collection: the host-held task roots the
// waiter -> wrapper -> registration chain. 30ms leaves room for the GCs.
var gcOutcome = "pending";
Atomics.waitAsync(finiteView, 5, 0, 30).value.then(function (v) { gcOutcome = v; });
if (typeof CollectGarbage === "function") {
    CollectGarbage();
    CollectGarbage();
}

// Notify-before-timeout: notify wins synchronously; the later timeout task
// no-ops without touching the released registration.
var notifyWinOutcome = "pending";
Atomics.waitAsync(finiteView, 6, 0, 60).value.then(function (v) { notifyWinOutcome = v; });
check(Atomics.notify(finiteView, 6, 1) === 1, "notify wins over pending 60ms timeout");

// Timeout-before-notify: a 1ms timeout unregisters the waiter, so the notify
// at ~10ms counts 0 and cannot resolve.
var timeoutWinOutcome = "pending";
Atomics.waitAsync(finiteView, 7, 0, 1).value.then(function (v) { timeoutWinOutcome = v; });
var timeoutWinNotifyCount = -1;
WScript.SetTimeout(function () {
    timeoutWinNotifyCount = Atomics.notify(finiteView, 7, 1);
}, 10);

// NaN / +Infinity / omitted timeouts are infinite: no timer is scheduled, the
// promises are still pending at 50ms and resolve through notify.
var infView = new Int32Array(new SharedArrayBuffer(16));
var pendOutcomes = ["pending", "pending", "pending"];
[Atomics.waitAsync(infView, 0, 0, NaN),
 Atomics.waitAsync(infView, 1, 0, Infinity),
 Atomics.waitAsync(infView, 2, 0)].forEach(function (w, i) {
    check(w.async === true && w.value instanceof Promise, "NaN/+Infinity/omitted timeout is asynchronous");
    w.value.then(function (v) { pendOutcomes[i] = v; });
});
WScript.SetTimeout(function () {
    check(pendOutcomes[0] === "pending" && pendOutcomes[1] === "pending" && pendOutcomes[2] === "pending",
        "NaN/+Infinity/omitted stay pending at 50ms, got " + pendOutcomes.join(","));
    check(Atomics.notify(infView, 0, 1) === 1, "NaN waiter still registered at 50ms");
    check(Atomics.notify(infView, 1, 1) === 1, "+Infinity waiter still registered at 50ms");
    check(Atomics.notify(infView, 2, 1) === 1, "omitted-timeout waiter still registered at 50ms");
}, 50);

// --- Single-agent notify resolves the promise to "ok" ----------------------
var notifyView = new Int32Array(new SharedArrayBuffer(16));
var waiter = Atomics.waitAsync(notifyView, 0, 0, Infinity);
check(waiter.async === true, "notify waiter async is true");
check(waiter.value instanceof Promise, "notify waiter value is a promise");
check(Object.getPrototypeOf(waiter.value) === Promise.prototype, "notify waiter is a real %Promise%");
check(Object.getOwnPropertyDescriptor(waiter, "async").enumerable === true, "result async is enumerable");
check(Object.getOwnPropertyDescriptor(waiter, "value").enumerable === true, "result value is enumerable");

var outcome = "pending";
waiter.value.then(function (value) { outcome = value; }, function (e) { outcome = "rejected:" + e; });

// A pending waiter must survive a collection while the wrapper and promise
// are live (the record is rooted by the SAB wrapper, not the native list).
if (typeof CollectGarbage === "function") {
    CollectGarbage();
    CollectGarbage();
}

check(Atomics.notify(notifyView, 0, 1) === 1, "notify sees the async waiter");
check(Atomics.notify(notifyView, 0, 1) === 0, "notify does not see a settled waiter");
check(Atomics.load(notifyView, 0) === 0, "waitAsync does not mutate the SAB");

// NaN and +Infinity are infinite timeouts; notify is the only resolution.
var nanWaiter = Atomics.waitAsync(notifyView, 1, 0, NaN);
check(nanWaiter.async === true && nanWaiter.value instanceof Promise, "NaN timeout is infinite");
var nanOutcome = "pending";
nanWaiter.value.then(function (value) { nanOutcome = value; });
check(Atomics.notify(notifyView, 1, 1) === 1, "notify NaN waiter");
check(nanWaiter.value instanceof Promise, "promise double-resolution is not observable");

WScript.SetTimeout(function () {
    try {
        check(outcome === "ok", "notified promise resolved to ok, got " + outcome);
        check(nanOutcome === "ok", "NaN waiter resolved to ok, got " + nanOutcome);
        check(realmOutcome === "ok", "cross-realm waiter resolved to ok, got " + realmOutcome);
        check(fifoOrder.join(",") === "0,1,2,3,4", "FIFO notify order, got " + fifoOrder.join(","));
        // Finite-timeout completion checks (S6d).
        check(oneMsOutcome === "timed-out", "1ms timeout resolved to timed-out, got " + oneMsOutcome);
        check(subOutcomeA === "timed-out", "0.5ms timeout resolved to timed-out (ceil to 1ms), got " + subOutcomeA);
        check(subOutcomeB === "timed-out", "1e-7ms timeout resolved to timed-out (ceil to 1ms), got " + subOutcomeB);
        check(multiOutcomes.join(",") === "timed-out,timed-out,timed-out",
            "three finite waiters all timed out, got " + multiOutcomes.join(","));
        check(Atomics.notify(finiteView, 4, 3) === 0, "timed-out waiters are unregistered");
        check(gcOutcome === "timed-out", "timeout task survived collection, got " + gcOutcome);
        check(notifyWinOutcome === "ok", "notify-before-timeout resolved ok exactly once, got " + notifyWinOutcome);
        check(timeoutWinOutcome === "timed-out", "timeout-before-notify resolved timed-out, got " + timeoutWinOutcome);
        check(timeoutWinNotifyCount === 0, "notify after a timeout counts 0, got " + timeoutWinNotifyCount);
        check(pendOutcomes.join(",") === "ok,ok,ok",
            "NaN/+Infinity/omitted resolved via notify after staying pending, got " + pendOutcomes.join(","));
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
        if (failures.length) {
            print("s6-waitasync-pause: " + failures.length + " failure(s)");
            WScript.Quit(1);
        }
        print("pass");
    } catch (e) {
        print("s6-waitasync-pause: unexpected failure: " + (e.stack || e));
        WScript.Quit(1);
    }
}, 100);
} catch (e) {
    print("s6-waitasync-pause: unexpected failure: " + (e.stack || e));
    WScript.Quit(1);
}
