//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// W4 DoD negative test (doc/wasm-arm64-plan.md W4 DoD): a module using wasm
// atomics must be rejected at compile/validate time with threads OFF
// (-WasmThreads- -ESSharedArrayBuffer-), in the interpreter (-nonative) and
// FullJIT (-ForceNative -off:simpleJit) tiers. The rlexe entry order is
// interpreter first, FullJIT second.
// Bytes are the WAT `(module (memory 1 1 shared) (func (export "a")
// (result i32) i32.const 0 i32.atomic.load))` converted with threads ON,
// then embedded, so the rejection is proven by the engine validator
// (WebAssembly.validate / WebAssembly.Module), not by the feature-gated
// WABT converter (which itself throws TypeError with threads OFF, so a
// convert-then-construct probe would prove nothing about the validator).
// The same bytes validate/construct successfully with threads ON, so the
// rejection below is threads-specific, not malformed-binary fallout.
var bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,2,1,0,5,4,1,3,1,1,7,5,1,1,97,0,0,10,10,1,8,0,65,0,254,16,2,0,11]);
if (WebAssembly.validate(bytes) !== false) {
    throw new Error("FAIL: WebAssembly.validate accepted an atomics module with threads OFF");
}
var rejected = false;
try {
    new WebAssembly.Module(bytes);
} catch (e) {
    if (e.name !== "CompileError" || !(e instanceof WebAssembly.CompileError)) {
        throw new Error("FAIL: wrong rejection type: " + e.name + " (" + e.message + ")");
    }
    if (e.message.indexOf("Threads") < 0) {
        throw new Error("FAIL: rejection lacks the Threads cause: " + e.message);
    }
    rejected = true;
}
if (!rejected) {
    throw new Error("FAIL: new WebAssembly.Module accepted an atomics module with threads OFF");
}
print("PASS");
