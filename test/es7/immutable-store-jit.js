//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Indexed stores to immutable-backed TypedArray views under the JIT
// (doc/c2-jit-immutable-store.md; the jit-closure-1 audit's holes 1+2).
//
// Every class runs in its own block so each store site is monomorphic and
// profile-fed: four sites (sloppy/strict single store, sloppy/strict
// 8-element loop — the loop shape also covers the Memset/Memcopy fusions)
// are warmed 20000 iterations on a MUTABLE view, then fed an
// immutable-backed view of the same class 1000 times. The fast path and
// the fusions must match the interpreter: sloppy stores are silent no-ops
// (bytes unchanged) and strict stores throw TypeError on every iteration.
// BigInt64/BigUint64 and Float16 have no backend store specialization
// (always helper) and are locked in as controls.
//
// Deterministic in every tier — interpreter (-nonative), default tier-up,
// -mic:1 -bgjit-, the interpreted-variant flags, dynapogo and forceNative
// all pass after the fix; before it, the profile-fed variants wrote the
// buffers and skipped the TypeErrors for all nine specialized classes.
// No baseline: exit 0 and a single "pass".

var failures = 0;

function fail(what) {
    failures++;
    if (failures <= 3) {
        print("FAIL: " + what);
    }
}

function checkBlock(name, bad1, allZero, threw, loopThrew, bytes) {
    if (bad1) {
        fail(name + ": sloppy single store wrote the immutable view");
    }
    if (!allZero) {
        fail(name + ": sloppy loop store wrote immutable element " + bytes);
    }
    if (threw !== 1000) {
        fail(name + ": strict single store threw " + threw + "/1000 TypeErrors");
    }
    if (loopThrew !== 1000) {
        fail(name + ": strict loop store threw " + loopThrew + "/1000 TypeErrors");
    }
}

// ---- Int8 ------------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Int8Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Int8Array((new ArrayBuffer(8)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Int8", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Uint8 -----------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Uint8Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Uint8Array((new ArrayBuffer(8)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Uint8", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Uint8Clamped ----------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Uint8ClampedArray(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Uint8ClampedArray((new ArrayBuffer(8)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Uint8Clamped", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Int16 -----------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Int16Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Int16Array((new ArrayBuffer(16)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Int16", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Uint16 ----------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Uint16Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Uint16Array((new ArrayBuffer(16)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Uint16", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Int32 -----------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Int32Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Int32Array((new ArrayBuffer(32)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Int32", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Uint32 ----------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Uint32Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9);
    var view = new Uint32Array((new ArrayBuffer(32)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Uint32", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Float32 ---------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Float32Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9.25);
    var view = new Float32Array((new ArrayBuffer(32)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9.25);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9.25);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Float32", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Float64 ---------------------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Float64Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9.25);
    var view = new Float64Array((new ArrayBuffer(64)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9.25);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9.25);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Float64", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- BigInt64 (control: helper path, no backend specialization) -------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new BigInt64Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 1n);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 1n);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 1n);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 1n);
    var view = new BigInt64Array((new ArrayBuffer(64)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 1n);
    var bad1 = view[1] !== 0n;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 1n);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0n) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 1n); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 1n); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("BigInt64", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- BigUint64 (control) ----------------------------------------------------
(function () {
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new BigUint64Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 1n);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 1n);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 1n);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 1n);
    var view = new BigUint64Array((new ArrayBuffer(64)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 1n);
    var bad1 = view[1] !== 0n;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 1n);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0n) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 1n); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 1n); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("BigUint64", bad1, allZero, threw, loopThrew, firstBad);
})();

// ---- Float16 (control; skipped when Float16Array is absent) -----------------
(function () {
    if (typeof Float16Array === "undefined") {
        return;
    }
    function storeSloppy(ta, v) { ta[1] = v; }
    function storeStrict(ta, v) { "use strict"; ta[1] = v; }
    function loopSloppy(ta, v) { for (var i = 0; i < 8; i++) ta[i] = v; }
    function loopStrict(ta, v) { "use strict"; for (var i = 0; i < 8; i++) ta[i] = v; }
    var mut = new Float16Array(8);
    for (var k = 0; k < 20000; k++) storeSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) storeStrict(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopSloppy(mut, 9.25);
    for (var k = 0; k < 20000; k++) loopStrict(mut, 9.25);
    var view = new Float16Array((new ArrayBuffer(16)).transferToImmutable());
    for (var k = 0; k < 1000; k++) storeSloppy(view, 9.25);
    var bad1 = view[1] !== 0;
    for (var k = 0; k < 1000; k++) loopSloppy(view, 9.25);
    var allZero = true, firstBad = -1;
    for (var i = 0; i < 8; i++) if (view[i] !== 0) { allZero = false; firstBad = i; }
    var threw = 0;
    for (var k = 0; k < 1000; k++) { try { storeStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) threw++; } }
    var loopThrew = 0;
    for (var k = 0; k < 1000; k++) { try { loopStrict(view, 9.25); } catch (e) { if (e instanceof TypeError) loopThrew++; } }
    checkBlock("Float16", bad1, allZero, threw, loopThrew, firstBad);
})();

if (failures !== 0) {
    throw new Error("immutable-store-jit: " + failures + " failing checks");
}
print("pass");
