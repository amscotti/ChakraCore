//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// W6 interpreter-scope GC churn probe (doc/wasm-arm64-plan.md W6.4): loops
// instantiate/call/memory.grow/export-read over a small MVP wasm module
// while building and verifying a JS object graph, under -CollectGarbage.
// In the style of test/es7/jit-write-barrier.js: deterministic seed,
// graph-integrity checks every round, "pass:true" only if all hold.
// Proves the ARM64 interpreted wasm path does not trace unboxed values,
// miss roots, or corrupt the heap across repeated allocations and forced
// collections. Run under -nonative -CollectGarbage (see rlexe entry).

var churnBytes = [
    0, 97, 115, 109, 1, 0, 0, 0, 1, 6, 1, 96, 1, 127, 1, 127,
    2, 11, 1, 3, 101, 110, 118, 3, 105, 110, 99, 0, 0, 3, 3, 2,
    0, 0, 5, 4, 1, 1, 1, 4, 7, 19, 3, 3, 97, 100, 100, 0,
    1, 3, 115, 117, 109, 0, 2, 3, 109, 101, 109, 2, 0, 10, 44, 2,
    6, 0, 32, 0, 16, 0, 11, 35, 1, 2, 127, 2, 64, 3, 64, 32,
    1, 32, 0, 79, 13, 1, 32, 2, 32, 1, 106, 33, 2, 32, 1, 65,
    1, 106, 33, 1, 12, 0, 11, 11, 32, 2, 11,
];

if (typeof CollectGarbage !== "function") {
  throw new Error("gc_churn: -CollectGarbage flag not enabled");
}

var buf = new Uint8Array(churnBytes).buffer;
if (WebAssembly.validate(buf) !== true) {
  throw new Error("gc_churn: module failed validation");
}
var module = new WebAssembly.Module(buf);

// Deterministic seed (same generator family as jit-write-barrier.js).
var seed = 49734321;
function rnd() {
  seed = ((seed + 0x7ed55d16) + (seed << 12)) & 0xffffffff;
  seed = ((seed ^ 0xc761c23c) ^ (seed >>> 19)) & 0xffffffff;
  seed = ((seed + 0x165667b1) + (seed << 5)) & 0xffffffff;
  seed = ((seed + 0xd3a2646c) ^ (seed << 9)) & 0xffffffff;
  seed = ((seed + 0xfd7046c5) + (seed << 3)) & 0xffffffff;
  seed = ((seed ^ 0xb55a4f09) ^ (seed >>> 16)) & 0xffffffff;
  return (seed & 0xfffffff) / 0x10000000;
}

function Node(k, v) { this.key = k; this.value = v; }
Node.prototype.left = null;
Node.prototype.right = null;

function buildList(n) {
  var head = new Node(0, null);
  var cur = head;
  for (var i = 1; i < n; i++) {
    var nd = new Node(i, { tag: i });
    cur.right = nd;
    cur.left = nd;
    cur = nd;
  }
  return head;
}

function verifyList(head, n) {
  var count = 0, sum = 0, cur = head;
  while (cur) {
    count++;
    sum += cur.key;
    if (cur.value !== null && cur.value.tag !== cur.key) {
      throw new Error("gc_churn: payload mismatch at " + cur.key);
    }
    cur = cur.right;
  }
  if (count !== n) throw new Error("gc_churn: count " + count + " != " + n);
  // sum(0..n-1) = n(n-1)/2
  var want = n * (n - 1) / 2;
  if (sum !== want) throw new Error("gc_churn: sum " + sum + " != " + want);
  return sum;
}

function arrayChurn(n) {
  var a = [];
  for (var i = 0; i < n; i++) a.push(new Node(i, null));
  var s = 0;
  for (var i = 0; i < n; i++) {
    a[i].left = a[(i + 1) % n];
    s += a[i].key + a[i].left.key;
  }
  return s;
}

function wasmSum(n) {
  // Independent oracle for the wasm loop export: it sums 0..n-1.
  return n * (n - 1) / 2;
}

var total = 0;
var ROUNDS = 48;
for (var r = 0; r < ROUNDS; r++) {
  // --- instantiate/call/memory.grow/export-read churn ---
  var instance;
  if (r % 2 === 0) {
    // Fresh compile+instantiate path (async WebAssembly.instantiate is
    // avoided so the probe stays synchronous and deterministic).
    var freshModule = new WebAssembly.Module(buf);
    instance = new WebAssembly.Instance(freshModule, {
      env: { inc: function (x) { return x + 1; } }
    });
  } else {
    // Shared compiled module, fresh instance.
    instance = new WebAssembly.Instance(module, {
      env: { inc: function (x) { return x + 1; } }
    });
  }
  var ex = instance.exports;
  var arg = (rnd() * 60) | 0;              // 0..59
  var gotAdd = ex.add(arg);
  if (gotAdd !== arg + 1) {
    throw new Error("gc_churn: add(" + arg + ") = " + gotAdd);
  }
  var gotSum = ex.sum(arg);
  var wantSum = wasmSum(arg);
  if (gotSum !== wantSum) {
    throw new Error("gc_churn: sum(" + arg + ") = " + gotSum + " != " + wantSum);
  }
  // Deterministic memory churn: 1..3 pages of growth, pattern write,
  // checksum readback of exported memory bytes.
  var pages = 1 + ((rnd() * 3) | 0);       // 1..3 extra pages (max 3)
  var mem = ex.mem;
  var prev = mem.grow(pages);
  if (prev !== 1) throw new Error("gc_churn: grow returned " + prev + " != 1");
  var bytes = new Uint8Array(mem.buffer);
  if (bytes.length !== (1 + pages) * 65536) {
    throw new Error("gc_churn: bytes " + bytes.length);
  }
  var stamp = (arg * 131 + pages * 17 + r) % 251;
  for (var k = 0; k < 512; k++) {
    bytes[k] = (stamp + k) % 251;
  }
  var check = 0;
  for (var k2 = 0; k2 < 512; k2++) {
    check += bytes[k2];
  }
  var wantCheck = 0;
  for (var k3 = 0; k3 < 512; k3++) {
    wantCheck += ((stamp + k3) % 251);
  }
  if (check !== wantCheck) {
    throw new Error("gc_churn: memory checksum " + check + " != " + wantCheck);
  }
  total += gotSum + gotAdd + check;

  // --- JS object-graph churn + integrity (barrier/retention shape) ---
  total += verifyList(buildList(400), 400);
  total += arrayChurn(300);
  if (r % 8 === 0) {
    var keep = buildList(200);
    for (var i = 0; i < 100; i++) {
      keep.right.left = new Node((rnd() * 1000) | 0, null);
      keep = keep.right;
    }
    total += keep.key;
  }
  if (r % 4 === 0) CollectGarbage();
}
CollectGarbage();

if (!(total > 0)) {
  throw new Error("gc_churn: total not positive: " + total);
}
console.log("pass:true");
