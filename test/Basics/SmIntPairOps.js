//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression pin for the ARM64 SmInt pair test
// (LowererMD::GenerateSmIntPairTest, lib/Backend/arm64/LowerMD.cpp;
// see doc/a6-smint-pair-test.md).
//
// The var-var int fast paths (Add, Sub, Mul, ShiftRight, and the tagged-int
// comparison path) share one pair-tag check. This test hammers those paths
// with hot loops so JIT tiers compile them, then asserts exact results.
// It passes both with the pair test disabled (helper fallback) and enabled
// (fast path): it guards the restored fast path against future behavioral
// regressions. The original defect was perf-only (the check could never
// pass, so every var-var int op fell through to helpers) and never produced
// a wrong result; the mixed-type section below pins the fail branch so a
// future mis-fix that wrongly passes non-int pairs cannot go unnoticed.

function add(a, b) { return a + b; }
function sub(a, b) { return a - b; }
function mul(a, b) { return a * b; }
function andi(a, b) { return a & b; }
function ori(a, b) { return a | b; }
function xori(a, b) { return a ^ b; }
function shl(a, b) { return a << b; }
function shr(a, b) { return a >> b; }
function shru(a, b) { return a >>> b; }
function lt(a, b) { return a < b; }
function eq(a, b) { return a == b; }

function fail(msg) { print("FAIL: " + msg); quit(1); }

var N = 20000;
var i;

// Add: 0 + 1 + ... + (N-1) = N*(N-1)/2
var sum = 0;
for (i = 0; i < N; i++) { sum = add(sum, i); }
if (sum !== N * (N - 1) / 2) { fail("add loop: " + sum); }

// Sub: N*(N-1)/2 - (0 + ... + (N-1)) = 0
var diff = N * (N - 1) / 2;
for (i = 0; i < N; i++) { diff = sub(diff, i); }
if (diff !== 0) { fail("sub loop: " + diff); }

// Mul: 3 * 5, and repeated doubling 2^20
if (mul(3, 5) !== 15) { fail("mul const"); }
var p = 1;
for (i = 0; i < 20; i++) { p = mul(p, 2); }
if (p !== 1048576) { fail("mul loop: " + p); }

// Bitwise ops on var-var ints
var acc = 0;
for (i = 0; i < N; i++) {
    acc = andi(0xFF, 0x0F) + acc;      // 15
    acc = ori(acc, 0);                 // unchanged
    acc = xori(acc, 0);                // unchanged
}
if (acc !== 15 * N) { fail("bitwise loop: " + acc); }
if (andi(-1, 12345) !== 12345) { fail("and"); }
if (ori(0xF0, 0x0F) !== 0xFF) { fail("or"); }
if (xori(0xFF, 0x0F) !== 0xF0) { fail("xor"); }

// Shifts on var-var ints (shift count is a var too)
var s = 0;
for (i = 0; i < N; i++) { s = add(s, shl(1, 2)); }   // +4 each
if (s !== 4 * N) { fail("shl loop: " + s); }
if (shr(16, 2) !== 4) { fail("shr"); }
if (shru(-1, 1) !== 2147483647) { fail("shru"); }
if (shl(-1, 1) !== -2) { fail("shl neg"); }

// Tagged-int comparisons through the pair-test call site
var c = 0;
for (i = 0; i < N; i++) { if (lt(i, add(i, 1))) { c = add(c, 1); } }
if (c !== N) { fail("lt loop: " + c); }
if (eq(42, 42) !== true) { fail("eq true"); }
if (eq(42, 43) !== false) { fail("eq false"); }

// Fail branch: non-int pairs must NOT take the int fast path.
// 'evil' is a float64 whose bit pattern aliases the int-pair tag
// (high word 0x00010001); int+float and int+string must still be exact.
var buf = new ArrayBuffer(8);
var f64 = new Float64Array(buf);
var u32 = new Uint32Array(buf);
u32[1] = 0x00010001; u32[0] = 0;
var evil = f64[0];
if (typeof evil !== "number") { fail("evil setup"); }
var zeros = [0];
var z = zeros[0];
for (i = 0; i < N; i++) { add(1, 2); add(1.5, 2.5); }
var r1 = add(evil, z);
if (r1 !== evil) { fail("add(evil,int): " + r1); }
if (typeof r1 !== "number") { fail("add(evil,int) type"); }
var r2 = mul(evil, 1);
if (r2 !== evil) { fail("mul(evil,1): " + r2); }
if (add(1.5, 2.5) !== 4) { fail("add floats"); }
if (add("a", "b") !== "ab") { fail("add strings"); }
if (eq(1, "1") !== true) { fail("eq coercion"); }
if (eq(1, "2") !== false) { fail("eq coercion false"); }

print("PASS");
