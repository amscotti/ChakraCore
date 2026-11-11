//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// W6 pointer-lookalike probe (doc/wasm-arm64-plan.md W6 DoD): wasm exports
// hold a pointer-shaped unboxed i32 and an unboxed f64 NaN live across an
// import that forces a full collection, and the probe asserts:
//   (i)   the process exits 0 (no crash tracing the lookalikes),
//   (ii)  a WeakRef to an unrelated object reads undefined after GC (the
//         collection ran and did not retain it),
//   (iii) returned values are uncorrupted (probe/getLook/memory readback
//         exactly as computed).
//
// EXACT-ADDRESS LIMITATION (honest scope): from JS there is no way to learn
// a Chakra heap address, so the i32 is pointer-SHAPED (0x7F0xxxx, the low
// half a 32-bit wasm value can express) but not pointer-EXACT: it cannot
// equal any live object address, hence it cannot be used to prove that a
// specific unboxed slot did not retain that specific object. Assertion (ii)
// therefore proves the collection actually executed and cleared an
// otherwise-unreachable object (which is what would expose a collector that
// fails to trace real roots or crashes on garbage), while (i)+(iii) prove
// the GC-during-wasm-import path neither crashed nor corrupted wasm state.
// A future harness with address observability would be needed for the
// stronger exact-address retention proof. Run under
// -nonative -CollectGarbage (see rlexe entry).

var lookalikeBytes = [
    0, 97, 115, 109, 1, 0, 0, 0, 1, 13, 3, 96, 0, 0, 96, 1,
    127, 1, 127, 96, 0, 1, 127, 2, 15, 1, 3, 101, 110, 118, 7, 102,
    111, 114, 99, 101, 71, 67, 0, 0, 3, 3, 2, 1, 2, 5, 3, 1,
    0, 1, 6, 6, 1, 127, 1, 65, 0, 11, 7, 25, 3, 5, 112, 114,
    111, 98, 101, 0, 1, 7, 103, 101, 116, 76, 111, 111, 107, 0, 2, 3,
    109, 101, 109, 2, 0, 10, 79, 2, 72, 2, 1, 127, 1, 124, 65, 255,
    0, 65, 24, 116, 65, 180, 36, 114, 33, 1, 68, 0, 0, 0, 0, 0,
    0, 0, 0, 68, 0, 0, 0, 0, 0, 0, 0, 0, 163, 33, 2, 32,
    1, 36, 0, 65, 0, 32, 1, 54, 2, 0, 16, 0, 32, 0, 32, 2,
    32, 2, 98, 4, 127, 65, 232, 7, 5, 65, 0, 11, 106, 65, 1, 106,
    11, 4, 0, 35, 0, 11,
];

if (typeof CollectGarbage !== "function") {
  throw new Error("gc_lookalike: -CollectGarbage flag not enabled");
}

var LOOK = (127 << 24) | 4660; // == 2130711092 (0x7F001234)
var buf = new Uint8Array(lookalikeBytes).buffer;
if (WebAssembly.validate(buf) !== true) {
  throw new Error("gc_lookalike: module failed validation");
}

var gcCalls = 0;
var instance = new WebAssembly.Instance(new WebAssembly.Module(buf), {
  env: {
    forceGC: function () {
      gcCalls++;
      CollectGarbage();
    }
  }
});
var ex = instance.exports;

// Unrelated victims: strong refs die with this IIFE frame; the refs array
// is all that survives (plus in-job KeepDuringJob, which expires at the job
// boundary before the cleared-check runs below).
var VICTIMS = 8;
var refs = (function () {
  var a = [];
  for (var i = 0; i < VICTIMS; i++) {
    a.push(new WeakRef({ tag: 4100 + i }));
  }
  return a;
})();

// (i)+(iii): wasm values held across the GC-forcing import call.
var before = gcCalls;
var r1 = ex.probe(7);
if (gcCalls !== before + 1) {
  throw new Error("gc_lookalike: import not called, calls=" + gcCalls);
}
if (r1 !== 1008) {
  throw new Error("gc_lookalike: probe(7) = " + r1 + " != 1008");
}
var r2 = ex.probe(-3);
if (r2 !== 998) {
  throw new Error("gc_lookalike: probe(-3) = " + r2 + " != 998");
}
if (ex.getLook() !== LOOK) {
  throw new Error("gc_lookalike: getLook() = " + ex.getLook() + " != " + LOOK);
}
var view = new DataView(ex.mem.buffer);
if (view.getInt32(0, true) !== LOOK) {
  throw new Error("gc_lookalike: memory i32 = " + view.getInt32(0, true));
}

// (ii): after the job boundary, forced collections must clear the weakly
// held victims. Clobber the stack so a dead slot cannot conservatively
// retain a victim by accident, then collect twice and check every ref.
function fail(message) {
  console.log("FAIL: " + message);
  WScript.Quit(1);
}

WScript.SetTimeout(function () {
  var scratch = [];
  for (var k = 0; k < 4096; k++) {
    scratch.push({ k: k });
  }
  scratch = null;
  CollectGarbage();
  CollectGarbage();
  for (var i = 0; i < refs.length; i++) {
    if (refs[i].deref() !== undefined) {
      fail("gc_lookalike: victim " + i + " still alive after GC");
    }
  }
  console.log("pass:true");
}, 10);
