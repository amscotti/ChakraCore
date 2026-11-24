//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify f32x4.abs and f64x2.abs bit-exact sign-bit clearing across
// interpreter (-nonative) and JIT (-ForceNative / tier-up), including -0.0 and negative NaNs.

function u32leb(v) {
    v = v >>> 0;
    const out = [];
    do {
        let b = v & 0x7f;
        v >>>= 7;
        if (v !== 0) b |= 0x80;
        out.push(b);
    } while (v !== 0);
    return out;
}

function strBytes(s) {
    const out = u32leb(s.length);
    for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i) & 0xff);
    return out;
}

function vec(items) {
    const out = u32leb(items.length);
    for (const item of items) out.push(...item);
    return out;
}

function section(id, payload) {
    return [id, ...u32leb(payload.length), ...payload];
}

function funcType(params, results) {
    return [0x60, ...vec(params.map(p => [p])), ...vec(results.map(r => [r]))];
}

const typeSec = section(1, vec([funcType([], [])]));
const importSec = section(2, vec([
    [...strBytes("env"), ...strBytes("mem"), 0x02, 0x00, 0x01]
]));
const funcSec = section(3, vec([u32leb(0), u32leb(0)]));
const exportSec = section(7, vec([
    [...strBytes("absF32"), 0x00, ...u32leb(0)],
    [...strBytes("absF64"), 0x00, ...u32leb(1)]
]));

function absFuncBody(simdSubOp) {
    const b = [
        0x41, 0x00,
        0x41, 0x00, 0xfd, 0x00, 0x02, 0x00,
        0xfd, ...u32leb(simdSubOp),
        0xfd, 0x0b, 0x02, 0x00,
        0x0b
    ];
    const body = [...vec([]), ...b];
    return [...u32leb(body.length), ...body];
}

const codeSec = section(10, vec([absFuncBody(0xe0), absFuncBody(0xec)]));
const wasmBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...typeSec, ...importSec, ...funcSec, ...exportSec, ...codeSec
]);

const mem = new WebAssembly.Memory({ initial: 1 });
const u32 = new Uint32Array(mem.buffer);
const inst = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), { env: { mem } });

function assertWords(label, expected) {
    for (let i = 0; i < 4; i++) {
        const got = u32[i] >>> 0;
        if (got !== expected[i]) {
            throw new Error(`${label} word ${i} mismatch: got=0x${got.toString(16)} exp=0x${expected[i].toString(16)}`);
        }
    }
}

function runChecks() {
    // f32x4.abs: [-0.0, +0.0, -1.5, -Inf]
    u32[0] = 0x80000000; u32[1] = 0x00000000; u32[2] = 0xbfc00000; u32[3] = 0xff800000;
    inst.exports.absF32();
    assertWords("f32x4.abs basic", [0x00000000, 0x00000000, 0x3fc00000, 0x7f800000]);

    // f32x4.abs: [-qNaN, -sNaN, -subnormal, +1.0]
    u32[0] = 0xffc00001; u32[1] = 0xff800001; u32[2] = 0x80000001; u32[3] = 0x3f800000;
    inst.exports.absF32();
    assertWords("f32x4.abs NaN/subnormal", [0x7fc00001, 0x7f800001, 0x00000001, 0x3f800000]);

    // f64x2.abs: [-0.0, -qNaN]
    u32[0] = 0x00000000; u32[1] = 0x80000000;
    u32[2] = 0x00000001; u32[3] = 0xfff80000;
    inst.exports.absF64();
    assertWords("f64x2.abs -0/-qNaN", [0x00000000, 0x00000000, 0x00000001, 0x7ff80000]);

    // f64x2.abs: [-sNaN, -subnormal]
    u32[0] = 0x00000001; u32[1] = 0xfff00000;
    u32[2] = 0x00000001; u32[3] = 0x80000000;
    inst.exports.absF64();
    assertWords("f64x2.abs -sNaN/-subnormal", [0x00000001, 0x7ff00000, 0x00000001, 0x00000000]);
}

// Run enough iterations to cover both cold interpreter and tiered-up JIT paths.
for (let i = 0; i < 200; i++) {
    runChecks();
}
print("Passed");
