//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Strict generator `arguments` under the JIT (doc/arm64-macos-jit.md
// "Strict generator arguments test coverage"): the coroutine
// LoadHeapArguments path must give strict generators an UNMAPPED arguments
// object with the %ThrowTypeError% callee accessor on the interpreter and
// the JIT alike, while a sloppy generator keeps arguments.callee === the
// generator function. Per the ES6+ unmapped-arguments shape, "caller" is
// simply absent (typeof "undefined"); only "callee" is the throwing
// accessor.
//
// The warm generators deliberately contain no try/catch: on this port a
// try/catch in a generator body keeps the coroutine interpreted, and the
// point of this test is the JIT-compiled body. The throwing callee access
// is driven separately with a fresh generator per iteration so the throw
// escapes next() without terminating the warm loops.
//
// Every driver loop runs 3000 iterations so the generator bodies tier up
// under -JitES6Generators (BailOutForGeneratorYield records name the JIT'd
// bodies). The test passes exit 0 in every tier (interpreted, tier-up, and
// full JIT) and needs no baseline.

var failures = 0;

function fail(what) {
    failures++;
    if (failures <= 3) {
        print("FAIL: " + what);
    }
}

// --- strict generator: unmapped arguments (warm, JIT-compiled body) -------
function* strictGen(a, b) {
    "use strict";
    while (true) {
        var lenOk = (arguments.length === 2);

        // Unmapped read: arguments[0] keeps the value passed at the call
        // (7), even after the formal's binding has changed.
        a = a + 1;
        var readUnmapped = (arguments[0] === 7);

        // Unmapped write: storing into arguments[0] must not change the
        // formal's binding.
        var before = a;
        arguments[0] = 999;
        var writeUnmapped = (a === before);
        arguments[0] = 7;

        // ES6 unmapped arguments: "caller" is not installed.
        var callerAbsent = (typeof arguments.caller === "undefined");

        yield (lenOk && readUnmapped && writeUnmapped && callerAbsent)
            ? { ok: true }
            : { ok: false, lenOk: lenOk, readUnmapped: readUnmapped, writeUnmapped: writeUnmapped,
                callerAbsent: callerAbsent };
    }
}

var it = strictGen(7, "x");
for (var i = 0; i < 3000; i++) {
    var r = it.next().value;
    if (!r.ok) {
        fail("strict generator arguments (iteration " + i + "): " + JSON.stringify(r));
    }
}

// --- strict generator: callee is the %ThrowTypeError% accessor ------------
// Yield first so a JIT'd body is observable (BailOutForGeneratorYield);
// the callee read then throws out of the second next() call.
function* calleeGen() {
    "use strict";
    yield 1;
    arguments.callee;
}

for (var i = 0; i < 3000; i++) {
    var g = calleeGen();
    g.next();
    var threw = false;
    try { g.next(); } catch (e) { threw = (e instanceof TypeError); }
    if (!threw) {
        fail("strict generator arguments.callee did not throw TypeError (iteration " + i + ")");
    }
}

// --- sloppy generator control: callee is the generator function -----------
function* sloppyGen(a) {
    while (true) {
        var calleeOk = (arguments.callee === sloppyGen);
        var lenOk = (arguments.length === 1);
        var callerUndefined = (typeof arguments.caller === "undefined");
        a = a + 1;
        yield (calleeOk && lenOk && callerUndefined)
            ? { ok: true }
            : { ok: false, calleeOk: calleeOk, lenOk: lenOk, callerUndefined: callerUndefined };
    }
}

var it2 = sloppyGen(1);
for (var i = 0; i < 3000; i++) {
    var r2 = it2.next().value;
    if (!r2.ok) {
        fail("sloppy generator arguments (iteration " + i + "): " + JSON.stringify(r2));
    }
}

// --- strict generator: arguments length across yield and return ----------
function* strictOnce() {
    "use strict";
    yield arguments.length;
    return arguments.length;
}

var it3 = strictOnce(10, 20);
var first = it3.next();
var last = it3.next();
if (first.value !== 2 || first.done !== false || last.value !== 2 || last.done !== true) {
    fail("strictOnce yield/return length: " + first.value + "/" + last.value + "/" + last.done);
}

if (failures !== 0) {
    throw new Error("strict-generator-args-jit: " + failures + " failing checks");
}
print("pass");
