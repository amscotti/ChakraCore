//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// JIT write-barrier regression (ARM64 full JIT + concurrent GC):
// field/element stores from JIT code must set the card-table byte or a
// concurrent marker that already scanned the container sweeps the stored
// target. Before the ARM64 GenerateWriteBarrier implementation, the
// Octane splay benchmark crashed full-JIT-only under concurrent GC.
// The loops below churn object graphs so tiered-up JIT stores race
// concurrent marking across many collections (-CollectGarbage), then
// verify graph integrity. Deterministic output via a fixed seed.

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
    cur.right = nd;   // inline/aux field store of a fresh object
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
      throw new Error("payload mismatch at " + cur.key);
    }
    cur = cur.right;
  }
  if (count !== n) throw new Error("count " + count + " != " + n);
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

var total = 0;
for (var r = 0; r < 120; r++) {
  total += verifyList(buildList(1500), 1500);
  total += arrayChurn(1200);
  if (r % 17 === 0) {
    // Relink a retained graph while collections run: stores into old
    // (already-marked) containers are the barrier-critical case.
    var keep = buildList(800);
    for (var i = 0; i < 400; i++) {
      keep.right.left = new Node(rnd() * 1000 | 0, null);
      keep = keep.right;
    }
    total += keep.key;
  }
}
WScript.Echo("pass:" + (total > 0));
