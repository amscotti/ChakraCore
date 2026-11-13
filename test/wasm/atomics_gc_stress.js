//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// W9 threads/atomics GC stress probe (doc/wasm-arm64-plan.md W9 DoD):
// looped atomic store + plain load over a module-defined SHARED memory
// while forcing collections and churning a verifiable JS object graph.
// Proves ARM64 JIT (LDAR/STLR) and interpreter atomic paths keep the
// shared buffer alive and value-correct across concurrent-GC cycles.
// Bytes are the WAT
//   (module (memory 1 1 shared)
//     (func (export "s") (param i32) (result i32)
//       i32.const 0 local.get 0 i32.atomic.store8
//       i32.const 0 i32.load))
// assembled by hand (LEB128, memargs inline). Run under -wasmthreads
// -ESSharedArrayBuffer -CollectGarbage (see rlexe entries, interp + FullJIT).
var storeBytes = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0,
    1, 6, 1, 96, 1, 127, 1, 127,
    3, 2, 1, 0,
    5, 4, 1, 3, 1, 1,
    7, 5, 1, 1, 115, 0, 0,
    10, 17, 1, 15, 0, 65, 0, 32, 0, 254, 25, 0, 0, 65, 0, 40, 2, 0, 11
]);

if (typeof CollectGarbage !== "function") {
    throw new Error("atomics_gc_stress: -CollectGarbage flag not enabled");
}
if (WebAssembly.validate(storeBytes.buffer) !== true) {
    throw new Error("atomics_gc_stress: module failed validation with threads ON");
}
var module = new WebAssembly.Module(storeBytes.buffer);
var instance = new WebAssembly.Instance(module);
var s = instance.exports.s;
if (typeof s !== "function") {
    throw new Error("atomics_gc_stress: export 's' missing");
}
if (!(instance.exports.memory === undefined)) {
    throw new Error("atomics_gc_stress: memory should not be exported");
}

function Node(k, v) { this.key = k; this.value = v; }
Node.prototype.left = null;
Node.prototype.right = null;

var seed = 19770421;
function rnd() {
    seed = ((seed + 0x7ed55d16) + (seed << 12)) & 0xffffffff;
    seed = ((seed ^ 0xc761c23c) ^ (seed >>> 19)) & 0xffffffff;
    seed = ((seed + 0x165667b1) + (seed << 5)) & 0xffffffff;
    seed = ((seed + 0xd3a2646c) ^ (seed << 9)) & 0xffffffff;
    seed = ((seed + 0xfd7046c5) + (seed << 3)) & 0xffffffff;
    seed = ((seed ^ 0xb55a4f09) ^ (seed >>> 16)) & 0xffffffff;
    return (seed & 0xfffffff) / 0x10000000;
}

var ROUNDS = 200;
var live = null;
for (var i = 0; i < ROUNDS; i++) {
    // Deterministic byte value; store8 writes byte 0, plain i32.load
    // reads it back zero-extended (bytes 1..3 stay zero).
    var v = (i * 37 + 11) & 0xff;
    var got = s(v) | 0;
    if (got !== v) {
        throw new Error("atomics_gc_stress: round " + i + " got " + got + " expected " + v);
    }
    // Churn a small verifiable graph, then force a collection while the
    // shared-memory instance is live.
    var head = new Node(i, { tag: i });
    var cur = head;
    var n = 1 + ((rnd() * 7) | 0);
    for (var k = 1; k < n; k++) {
        var nd = new Node(i * 1000 + k, { tag: k });
        cur.right = nd;
        cur.left = nd;
        cur = nd;
    }
    live = head;
    CollectGarbage();
    // Verify the store survived the collection and the graph is intact.
    var got2 = s(v) | 0;
    if (got2 !== v) {
        throw new Error("atomics_gc_stress: post-GC round " + i + " got " + got2 + " expected " + v);
    }
    var c = live;
    var count = 0;
    while (c !== null && count <= n) {
        if (c.key !== i && !(c.key >= i * 1000)) {
            throw new Error("atomics_gc_stress: graph corrupt at round " + i);
        }
        c = c.right;
        count++;
    }
    if (count !== n) {
        throw new Error("atomics_gc_stress: graph length " + count + " expected " + n);
    }
}
live = null;
CollectGarbage();
print("PASS");
