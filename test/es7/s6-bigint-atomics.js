//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S6c flag-on surface: BigInt64Array/BigUint64Array Atomics. Runs with
// -ESBigInt -ESSharedArrayBuffer (doc/c2-s6-bigint-atomics.md). Covers the
// RMW/load/store family on shared and non-shared buffers, coercion order,
// wrapping/signedness, wait/notify/waitAsync value semantics for
// BigInt64Array (BigUint64Array waitable rejection) and isLockFree(8).

var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("s6-bigint-atomics: FAIL: " + message);
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

try {
// --- Preconditions ---------------------------------------------------------
check(typeof SharedArrayBuffer === "function", "SharedArrayBuffer exists");
check(typeof Atomics === "object", "Atomics exists");
check(typeof BigInt64Array === "function", "BigInt64Array exists");
check(typeof BigUint64Array === "function", "BigUint64Array exists");

var U64MAX = 18446744073709551615n;   // 2n**64n - 1n
var I64MIN = -9223372036854775808n;   // -2n**63n
var I64MAX = 9223372036854775807n;    // 2n**63n - 1n

// --- RMW on shared and non-shared BigInt64Array/BigUint64Array ------------
function runRmw(makeBuffer) {
    // add/sub wrap modulo 2**64 (signed result for BigInt64).
    var b64 = new BigInt64Array(makeBuffer(8), 0, 1);
    check(Atomics.store(b64, 0, I64MAX) === I64MAX, "store returns the ToBigInt value (max)");
    check(Atomics.add(b64, 0, 1n) === I64MAX, "add returns the previous value");
    check(Atomics.load(b64, 0) === I64MIN, "add wrapped 2**63-1 + 1 to -2**63");
    check(Atomics.add(b64, 0, -1n) === I64MIN, "add returns the wrapped previous value");
    check(Atomics.load(b64, 0) === I64MAX, "add wrapped -2**63 - 1 back to 2**63-1");
    Atomics.store(b64, 0, I64MIN);
    check(Atomics.sub(b64, 0, 1n) === I64MIN, "sub returns the previous value");
    check(Atomics.load(b64, 0) === I64MAX, "sub wrapped -2**63 - 1 to 2**63-1");

    // unsigned add/sub wrap in [0, 2**64).
    var u64 = new BigUint64Array(makeBuffer(16), 0, 1);
    check(Atomics.store(u64, 0, U64MAX) === U64MAX, "store returns the ToBigInt value (uint max)");
    check(Atomics.add(u64, 0, 1n) === U64MAX, "unsigned add returns the previous value");
    check(Atomics.load(u64, 0) === 0n, "unsigned add wrapped to 0");
    check(Atomics.sub(u64, 0, 1n) === 0n, "unsigned sub borrows from 0");
    check(Atomics.load(u64, 0) === U64MAX, "unsigned sub wrapped to 2**64-1");

    // bitwise family and exchange.
    var bits = new BigInt64Array(makeBuffer(64), 0, 4);
    Atomics.store(bits, 0, 0xff00ff00ff00ff00n - 2n ** 64n); // negative pattern
    check(Atomics.and(bits, 0, 0xf0f0f0f0f0f0f0f0n) === 0xff00ff00ff00ff00n - 2n ** 64n, "and returns the previous signed value");
    check(Atomics.load(bits, 0) === (0xff00ff00ff00ff00n & 0xf0f0f0f0f0f0f0f0n) - 2n ** 64n, "and stored the intersection");
    Atomics.store(bits, 1, 0n);
    check(Atomics.or(bits, 1, 0x0f0f0f0f0f0f0f0fn) === 0n, "or returns the previous zero");
    check(Atomics.load(bits, 1) === 0x0f0f0f0f0f0f0f0fn, "or stored the union");
    Atomics.store(bits, 2, 0n);
    check(Atomics.xor(bits, 2, 0xffffffffffffffffn - 2n ** 64n) === 0n, "xor returns the previous zero");
    check(Atomics.load(bits, 2) === -1n, "xor stored the complement");
    check(Atomics.exchange(bits, 3, 0x1234n) === 0n, "exchange returns the previous value");
    check(Atomics.load(bits, 3) === 0x1234n, "exchange stored the new value");

    // compareExchange: comparand coerced before replacement; both ToBigInt.
    var cx = new BigInt64Array(makeBuffer(16), 0, 2);
    Atomics.store(cx, 0, 7n);
    check(Atomics.compareExchange(cx, 0, 7n, 42n) === 7n, "compareExchange match returns the old value");
    check(Atomics.load(cx, 0) === 42n, "compareExchange match stored the replacement");
    check(Atomics.compareExchange(cx, 0, 7n, 43n) === 42n, "compareExchange miss returns the current value");
    check(Atomics.load(cx, 0) === 42n, "compareExchange miss does not store");

    // ToBigInt coercions: wrappers, strings, booleans.
    var coerce = new BigUint64Array(makeBuffer(16), 0, 2);
    check(Atomics.store(coerce, 0, Object(10n)) === 10n, "BigInt wrapper object converts via valueOf");
    check(Atomics.load(coerce, 0) === 10n, "wrapper store landed");
    check(Atomics.store(coerce, 1, "0x10") === 16n, "hex string stores without the trailing n");
    check(Atomics.load(coerce, 1) === 16n, "hex string value stored");
    check(Atomics.store(coerce, 1, true) === 1n, "boolean true converts to 1n");
    var order = [];
    check(Atomics.add(coerce, 1, { valueOf: function () { order.push("value"); return 1n; } }) === 1n, "valueOf object converts to BigInt");
    check(order.join(",") === "value", "valueOf ran once");
    check(Atomics.load(coerce, 1) === 2n, "valueOf result added");
    // The StringToBigInt grammar rejects a trailing n and decimal-junk strings.
    throws(SyntaxError, function () { Atomics.add(coerce, 0, "0x10n"); }, "trailing-n string is a SyntaxError");
    throws(SyntaxError, function () { Atomics.add(coerce, 0, "junk"); }, "unparseable string is a SyntaxError");

    // Mixed BigInt/Number rejection in both directions.
    throws(TypeError, function () { Atomics.add(coerce, 0, 1); }, "Number value rejected on a BigInt array");
    throws(TypeError, function () { Atomics.store(coerce, 0, undefined); }, "undefined value rejected on a BigInt array");
    throws(TypeError, function () { Atomics.compareExchange(coerce, 0, 1, 2n); }, "Number comparand rejected");
    throws(TypeError, function () { Atomics.compareExchange(coerce, 0, 1n, 2); }, "Number replacement rejected");
    var i32 = new Int32Array(makeBuffer(32), 0, 4);
    throws(TypeError, function () { Atomics.add(i32, 0, 1n); }, "BigInt value rejected on an Int32Array");
    throws(TypeError, function () { Atomics.store(i32, 0, 1n); }, "BigInt store rejected on an Int32Array");

    // store stores only the low 64 bits but returns the full BigInt.
    var wide = new BigUint64Array(makeBuffer(16), 0, 1);
    var wideValue = 2n ** 100n + 5n;
    check(Atomics.store(wide, 0, wideValue) === wideValue, "store returns the untruncated ToBigInt value");
    check(Atomics.load(wide, 0) === 5n, "store kept only the low 64 bits");

    // Index validation and non-integer index coercion.
    throws(RangeError, function () { Atomics.load(bits, -1); }, "negative index throws RangeError");
    throws(RangeError, function () { Atomics.load(bits, 4); }, "out-of-range index throws RangeError");
    check(Atomics.load(bits, { valueOf: function () { return 0; } }) === Atomics.load(bits, 0), "object index coerces");
    throws(TypeError, function () { Atomics.add(new Float64Array(makeBuffer(16), 0, 2), 0, 1n); }, "Float64Array rejected");
    throws(TypeError, function () { Atomics.load(new Float64Array(makeBuffer(16), 0, 2), 0); }, "Float64Array load rejected");

    // Validation order: type before index before value.
    var poison = { valueOf: function () { check(false, "coercion ran after a validation failure"); return 0n; } };
    throws(TypeError, function () { Atomics.add(new Float64Array(makeBuffer(16), 0, 2), poison, poison); }, "type validated before coercion");
    var seq = [];
    throws(RangeError, function () {
        Atomics.add(bits, { valueOf: function () { seq.push("index"); return 99; } },
            { valueOf: function () { seq.push("value"); return 1n; } });
    }, "index range error precedes value");
    check(seq.join(",") === "index", "value not coerced after index error, got " + seq.join(","));

    // Revalidation after value conversion on a resizable non-shared buffer.
    var rab = new ArrayBuffer(32, { maxByteLength: 64 });
    var rabView = new BigInt64Array(rab, 0, 4);
    var shrinkThenGrow = {
        valueOf: function () {
            rab.resize(8);
            rab.resize(32);
            return 1n;
        }
    };
    check(Atomics.add(rabView, 1, shrinkThenGrow) === 0n, "shrink+regrow during value coercion revalidates");
    check(Atomics.load(rabView, 1) === 1n, "revalidated add stored the value");
    var shrinkOnly = {
        valueOf: function () {
            rab.resize(0);
            return 1n;
        }
    };
    throws(TypeError, function () { Atomics.add(rabView, 1, shrinkOnly); }, "view out of bounds after conversion throws");
    rab.resize(32);
    var detachDuringValue = {
        valueOf: function () {
            ArrayBuffer.prototype.transfer.call(rab);
            return 1n;
        }
    };
    throws(TypeError, function () { Atomics.store(rabView, 0, detachDuringValue); }, "detach during value coercion throws");

    // Growable SAB views: length captured before index; growth keeps values.
    var growSab = new SharedArrayBuffer(8, { maxByteLength: 64 });
    var growView = new BigInt64Array(growSab);
    throws(RangeError, function () {
        Atomics.add(growView, { valueOf: function () { growSab.grow(64); return 1; } }, 1n);
    }, "growable length captured before index coercion");
    check(growSab.byteLength === 64, "index getter still grew the SAB");
    check(Atomics.store(growView, 7, 11n) === 11n, "store after growth reaches the new extent");
    check(Atomics.load(growView, 7) === 11n, "load after growth");
}

runRmw(function (bytes) { return new SharedArrayBuffer(bytes); });
runRmw(function (bytes) { return new ArrayBuffer(bytes); });

// Non-zero byteOffset addressing (8-byte elements).
var offsetSab = new SharedArrayBuffer(8 * 4);
var offsetView = new BigInt64Array(offsetSab, 8, 2);
check(Atomics.store(offsetView, 1, 0xabcn) === 0xabcn, "store with byteOffset");
check(Atomics.load(offsetView, 1) === 0xabcn, "load with byteOffset");
check(new BigInt64Array(offsetSab)[2] === 0xabcn, "offset store hit element 2 of the SAB");

// --- wait / notify / waitAsync ---------------------------------------------
var sab = new SharedArrayBuffer(8 * 4);
var b64 = new BigInt64Array(sab);
var u64 = new BigUint64Array(sab);
var i32 = new Int32Array(sab);

// BigUint64Array is not waitable: brand check before any coercion.
var waitPoison = { valueOf: function () { check(false, "wait coercion ran for a BigUint64 view"); return 0n; } };
throws(TypeError, function () { Atomics.wait(u64, 0, 0n); }, "wait rejects BigUint64Array");
throws(TypeError, function () { Atomics.wait(u64, waitPoison, waitPoison); }, "wait brand check precedes coercion");
throws(TypeError, function () { Atomics.waitAsync(u64, 0, 0n); }, "waitAsync rejects BigUint64Array");
throws(TypeError, function () { Atomics.notify(u64, 0, 1); }, "notify rejects BigUint64Array");

// wait on BigInt64Array: shared-only, ToBigInt64 value, timeout coercion.
throws(TypeError, function () { Atomics.wait(new BigInt64Array(new ArrayBuffer(8)), 0, 0n, 0); }, "wait is shared-only");
check(Atomics.wait(b64, 0, 1n, 0) === "not-equal", "wait not-equal for a BigInt witness");
check(Atomics.wait(b64, 0, 0n, 0) === "timed-out", "wait equal witness with zero timeout");
throws(SyntaxError, function () { Atomics.wait(b64, 0, "0x0n", 0); }, "wait value uses the StringToBigInt grammar");
[0, -1, -1000.5, -Infinity, false, null].forEach(function (timeout) {
    check(Atomics.wait(b64, 0, 0n, timeout) === "timed-out", String(timeout) + " wait timeout is immediate");
});

// Value coercion order: value (ToBigInt) before timeout (ToNumber).
var waitOrder = [];
throws(TypeError, function () {
    Atomics.wait(b64, 0,
        { valueOf: function () { waitOrder.push("value"); throw new TypeError("value"); } },
        { valueOf: function () { waitOrder.push("timeout"); return 0; } });
}, "wait value coercion error");
check(waitOrder.join(",") === "value", "wait timeout not coerced after value error, got " + waitOrder.join(","));

waitOrder = [];
throws(Error, function () {
    Atomics.wait(b64, 0, 0n,
        { valueOf: function () { waitOrder.push("timeout"); throw new Error("timeout"); } });
}, "wait timeout coercion error");
check(waitOrder.join(",") === "timeout", "wait timeout coerced after value");

// An infinite sync wait cannot suspend on this host and reports timed-out.
check(Atomics.wait(b64, 0, 0n, Infinity) === "timed-out", "infinite sync wait is a non-suspending timeout");

// waitAsync on BigInt64Array: fast paths and the promise path.
var notEqual = Atomics.waitAsync(b64, 1, 1n, 0);
check(notEqual.async === false && notEqual.value === "not-equal", "waitAsync not-equal fast path");
var timedOut = Atomics.waitAsync(b64, 1, 0n, 0);
check(timedOut.async === false && timedOut.value === "timed-out", "waitAsync zero-timeout fast path");

// Missing value is a TypeError for BigInt64 (ToBigInt(undefined)).
throws(TypeError, function () { Atomics.waitAsync(b64, 1); }, "waitAsync missing value throws for BigInt64");

var waiter = Atomics.waitAsync(b64, 2, 0n, Infinity);
check(waiter.async === true, "waitAsync async path");
check(waiter.value instanceof Promise, "waitAsync async value is a promise");
check(Atomics.load(b64, 2) === 0n, "waitAsync does not mutate the view");

var outcome = "pending";
waiter.value.then(function (value) { outcome = value; }, function (e) { outcome = "rejected:" + e; });

if (typeof CollectGarbage === "function") {
    CollectGarbage();
    CollectGarbage();
}

// notify wakes the async BigInt64 waiter; count is evaluated first.
var notifyCount = 0;
check(Atomics.notify(b64, 2, { valueOf: function () { notifyCount++; return 1; } }) === 1, "notify wakes the BigInt64 waiter");
check(notifyCount === 1, "notify count coerced once");
check(Atomics.notify(b64, 2, 1) === 0, "notify does not double-wake");

// notify on a non-shared BigInt64Array evaluates count, then returns 0.
var nonSharedB64 = new BigInt64Array(new ArrayBuffer(8));
var nonSharedCount = 0;
check(Atomics.notify(nonSharedB64, 0, { valueOf: function () { nonSharedCount++; return 42; } }) === 0, "non-shared notify returns 0");
check(nonSharedCount === 1, "non-shared notify evaluated count");

// Index validation before count.
throws(RangeError, function () { Atomics.notify(b64, 9, 1); }, "notify index validated");

// --- isLockFree ------------------------------------------------------------
check(Atomics.isLockFree(8) === true, "isLockFree(8) is true on this 64-bit build");
check(Atomics.isLockFree(8) === Atomics.isLockFree(8), "isLockFree(8) is deterministic");
check(Atomics.isLockFree(1) === true && Atomics.isLockFree(2) === true && Atomics.isLockFree(4) === true, "sizes 1/2/4 unchanged");
check(Atomics.isLockFree(3) === false && Atomics.isLockFree(5) === false && Atomics.isLockFree(16) === false, "other sizes false");
// Spec isLockFree applies ToIntegerOrInfinity(size) before comparing, so
// fractional sizes truncate: 4.9 -> 4 (true) and 8.9 -> 8 (true on 64-bit).
// Bun 1.4.3/JSC agree; Node 26 is the outlier for 8.9 (and 4.9), returning
// false because it compares the untruncated value.
check(Atomics.isLockFree(4.9) === true, "isLockFree(4.9) truncates to size 4");
check(Atomics.isLockFree(8.9) === true, "isLockFree(8.9) truncates to size 8 on 64-bit");

// --- Async completion ------------------------------------------------------
WScript.SetTimeout(function () {
    try {
        // The waiter promise resolves to "ok" after notify (the notify above
        // already ran; the reaction is observed here).
        check(outcome === "ok", "notified BigInt64 waitAsync resolved to ok, got " + outcome);
        if (typeof CollectGarbage === "function") {
            CollectGarbage();
        }
        if (failures.length) {
            print("s6-bigint-atomics: " + failures.length + " failure(s)");
            WScript.Quit(1);
        }
        print("pass");
    } catch (e) {
        print("s6-bigint-atomics: unexpected failure: " + (e.stack || e));
        WScript.Quit(1);
    }
}, 100);
} catch (e) {
    print("s6-bigint-atomics: unexpected failure: " + (e.stack || e));
    WScript.Quit(1);
}
