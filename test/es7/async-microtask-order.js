//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// gc-async-continuation regression: the ch host must run promise-continuation
// microtasks queued by a finished callback BEFORE any pending timer message.
// Previously microtasks were stamped "now" into the timer priority queue, so
// (a) any already-queued timer with an equal or earlier stamp jumped ahead of
// the just-enqueued continuation (test 1: deterministic with delay-0 timers),
// and (b) when a slow callback let wall clock pass a later timer's absolute
// deadline, the continuation landed after that timer — observed as async
// functions silently never resuming while GC churn ran between suspension and
// resolve (tests 2 and 3; debug-static dropped 3/3 pre-fix). The engine was
// exonerated: the suspended frame, reaction list, handler and generator all
// survive the collection; only the host message order was wrong.
// Fix + receipt: doc/e2-gc-async-continuation.md.

var out = [];

// Test 1: deterministic (timing-free) microtask-vs-timer order.
// f's continuation is queued while timer2/timer3 are already pending with
// equal delay-0 stamps; it must still run first.
async function f1() {
    out.push("f1-start");
    var v = await Promise.resolve(1);
    out.push("f1-resumed:" + v);
}
WScript.SetTimeout(function () { f1(); }, 0);
WScript.SetTimeout(function () { out.push("timer2"); }, 0);
WScript.SetTimeout(function () { out.push("timer3"); }, 0);

// Test 2 (the filed h0 repro): churn while an async function is suspended,
// resolve in a later host message. The resume must be observed by the final
// dump even if the churn callback overran the dump timer's deadline.
var r1 = null;
function PR() { return new Promise(function (x) { r1 = x; }); }
async function f2() {
    out.push("f2-start");
    var v2 = await PR();
    out.push("f2-resumed:" + v2);
}
f2();
WScript.SetTimeout(function () {
    var sink; for (var g = 0; g < 15; g++) { sink = []; for (var k = 0; k < 12000; k++) sink.push({ q: k }); }
}, 80);
WScript.SetTimeout(function () { r1(7); }, 300);

// Test 3 (the filed h5 repro): after the churn, even a NEW async function
// awaiting an ALREADY-RESOLVED promise must resume — the microtask machinery
// itself must not depend on the churn callback's wall-clock duration.
async function f3() {
    out.push("f3-start");
    var v3 = await Promise.resolve(99);
    out.push("f3-resumed:" + v3);
}
WScript.SetTimeout(function () { r1(7); f3(); }, 300);

WScript.SetTimeout(function () {
    out.forEach(function (s) { WScript.Echo(s); });
    WScript.Echo("END");
}, 500);
WScript.Echo("scriptend");
