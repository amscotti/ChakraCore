//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify ARM64 JIT preserves full 128-bit v128 values live across scalar WASM
// and imported JS/asm.js calls (D8-D15 only save low 64 bits in scalar callees).

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

function s32leb(v) {
    v = v | 0;
    const out = [];
    while (true) {
        const b = v & 0x7f;
        v >>= 7;
        if ((v === 0 && (b & 0x40) === 0) || (v === -1 && (b & 0x40) !== 0)) {
            out.push(b);
            break;
        }
        out.push(b | 0x80);
    }
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

function f64Bytes(val) {
    const ab = new ArrayBuffer(8);
    new DataView(ab).setFloat64(0, val, true);
    return Array.from(new Uint8Array(ab));
}

function funcType(params, results) {
    return [0x60, ...vec(params.map(p => [p])), ...vec(results.map(r => [r]))];
}

const T_I32 = 0x7f;
const T_F64 = 0x7c;
const T_V128 = 0x7b;

// Type 0: (i32, f64, f64) -> f64 (imported JS call)
// Type 1: (f64 x 8, i32) -> f64 (recursive scalar WASM helper using d8-d15)
// Type 2: (i32 delta, i32 iters, i32 outOffset) -> i32
const typeSec = section(1, vec([
    funcType([T_I32, T_F64, T_F64], [T_F64]),
    funcType([T_F64, T_F64, T_F64, T_F64, T_F64, T_F64, T_F64, T_F64, T_I32], [T_F64]),
    funcType([T_I32, T_I32, T_I32], [T_I32])
]));

const importSec = section(2, vec([
    [...strBytes("env"), ...strBytes("jsCall"), 0x00, ...u32leb(0)],
    [...strBytes("env"), ...strBytes("mem"), 0x02, 0x00, 0x01]
]));

const funcSec = section(3, vec([u32leb(1), u32leb(2), u32leb(2)]));
const exportSec = section(7, vec([
    [...strBytes("run12"), 0x00, ...u32leb(2)],
    [...strBytes("run16"), 0x00, ...u32leb(3)]
]));

// Func 1: recursive scalar WASM function with 24 live f64 locals across a call.
const scalarBody = (() => {
    const locals = vec([[24, T_F64]]);
    const b = [];
    for (let i = 0; i < 24; i++) {
        b.push(
            0x20, ...u32leb(i % 8),
            0x44, ...f64Bytes(1.125 + i * 0.25),
            0xa2,
            0x20, ...u32leb((i + 3) % 8),
            0xa0,
            0x21, ...u32leb(9 + i)
        );
    }
    b.push(0x20, ...u32leb(8), 0x41, 0x00, 0x4a, 0x04, 0x40);
    for (let i = 0; i < 8; i++) b.push(0x20, ...u32leb(9 + i));
    b.push(
        0x20, ...u32leb(8), 0x41, 0x01, 0x6b,
        0x10, ...u32leb(1),
        0x20, ...u32leb(17), 0xa0, 0x21, ...u32leb(17),
        0x0b
    );
    b.push(0x20, ...u32leb(9));
    for (let i = 1; i < 24; i++) b.push(0x20, ...u32leb(9 + i), 0xa0);
    b.push(0x0b);
    const body = [...locals, ...b];
    return [...u32leb(body.length), ...body];
})();

function makeSimdBody(numVecs) {
    const locals = vec([[numVecs, T_V128], [2, T_I32], [1, T_F64]]);
    const L_DELTA = 0;
    const L_ITERS = 1;
    const L_OUT = 2;
    const L_V0 = 3;
    const L_I = 3 + numVecs;
    const L_ACC = L_I + 1;
    const L_FTMP = L_I + 2;

    const b = [];
    for (let k = 0; k < numVecs; k++) {
        b.push(
            0x41, ...s32leb(k * 16),
            0xfd, 0x00, 0x02, 0x00,
            0x20, ...u32leb(L_DELTA),
            0xfd, 0x11,
            0xfd, 0xae, 0x01,
            0x21, ...u32leb(L_V0 + k)
        );
    }

    b.push(0x41, 0x00, 0x21, ...u32leb(L_I));
    b.push(0x41, 0x00, 0x21, ...u32leb(L_ACC));

    b.push(0x03, 0x40);
    for (let k = 0; k < numVecs; k++) {
        b.push(
            0x20, ...u32leb(L_ACC),
            0x20, ...u32leb(L_V0 + k), 0xfd, 0x1b, 2,
            0x20, ...u32leb(L_V0 + k), 0xfd, 0x1b, 3,
            0x6a, 0x6a,
            0x21, ...u32leb(L_ACC)
        );
    }

    for (let p = 0; p < 8; p++) b.push(0x44, ...f64Bytes(p + 1.5));
    b.push(0x41, 0x03, 0x10, ...u32leb(1), 0x21, ...u32leb(L_FTMP));

    b.push(0x20, ...u32leb(L_I), 0x20, ...u32leb(L_FTMP), 0x44, ...f64Bytes(3.5));
    b.push(0x10, ...u32leb(0), 0x21, ...u32leb(L_FTMP));

    b.push(0x20, ...u32leb(L_I), 0x41, 0x01, 0x6a, 0x22, ...u32leb(L_I));
    b.push(0x20, ...u32leb(L_ITERS), 0x48, 0x0d, 0x00);
    b.push(0x0b);

    for (let k = 0; k < numVecs; k++) {
        b.push(
            0x20, ...u32leb(L_OUT),
            0x41, ...s32leb(k * 16),
            0x6a,
            0x20, ...u32leb(L_V0 + k),
            0xfd, 0x0b, 0x02, 0x00
        );
    }
    b.push(0x20, ...u32leb(L_ACC), 0x0b);
    const body = [...locals, ...b];
    return [...u32leb(body.length), ...body];
}

const codeSec = section(10, vec([scalarBody, makeSimdBody(12), makeSimdBody(16)]));
const wasmBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...typeSec, ...importSec, ...funcSec, ...exportSec, ...codeSec
]);

function asmModule(stdlib, foreign, heap) {
    "use asm";
    const sin = stdlib.Math.sin;
    function leaf(a, b, depth) {
        a = +a;
        b = +b;
        depth = depth | 0;
        var d0 = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;
        var d4 = 0.0, d5 = 0.0, d6 = 0.0, d7 = 0.0;
        var d8 = 0.0, d9 = 0.0, d10 = 0.0, d11 = 0.0;
        d0 = +(a * 1.25); d1 = +(b * 1.5); d2 = +(a + b); d3 = +(a - b);
        d4 = +(d0 + d2); d5 = +(d1 + d3); d6 = +(d4 * 0.5); d7 = +(d5 * 0.25);
        d8 = +(d6 + 1.0); d9 = +(d7 + 2.0); d10 = +(d8 + d9); d11 = +(d10 * 1.125);
        if ((depth | 0) > 0) {
            d0 = +(+leaf(d0, d1, (depth - 1) | 0) + d0);
        }
        return +(d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9 + d10 + d11 + +sin(a));
    }
    return { leaf: leaf };
}

const asmLeaf = asmModule({ Math: Math }, {}, new ArrayBuffer(0x10000)).leaf;

const mem = new WebAssembly.Memory({ initial: 1 });
const u32 = new Uint32Array(mem.buffer);

function jsCall(iter, a, b) {
    let x0 = a + 1.1, x1 = b + 2.2, x2 = a * 3.3, x3 = b * 4.4;
    let x4 = a - 0.5, x5 = b - 0.75, x6 = x0 + x1, x7 = x2 + x3;
    let y = asmLeaf(x0, x1, 2);
    return Math.sin(x4) + Math.cos(x5) + x6 + x7 + y + iter;
}

for (let i = 0; i < 200; i++) {
    jsCall(i, 1.5, 2.5);
}

for (let k = 0; k < 16; k++) {
    u32[k * 4 + 0] = (0x10000001 + k * 0x01010101) >>> 0;
    u32[k * 4 + 1] = (0x20000002 + k * 0x02020202) >>> 0;
    u32[k * 4 + 2] = (0x99aabb03 + k * 0x03030303) >>> 0;
    u32[k * 4 + 3] = (0xddeeff04 + k * 0x04040404) >>> 0;
}

const inst = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), { env: { jsCall, mem } });

function verifyRun(fn, numVecs, outOffset, iters) {
    for (let t = 0; t < iters; t++) {
        const delta = (t + 1) * 7;
        fn(delta, 4, outOffset);
        const wordBase = outOffset >>> 2;
        for (let k = 0; k < numVecs; k++) {
            for (let lane = 0; lane < 4; lane++) {
                const got = u32[wordBase + k * 4 + lane] >>> 0;
                const exp = ((u32[k * 4 + lane] + delta) | 0) >>> 0;
                if (got !== exp) {
                    throw new Error(`v128 corrupted (vecs=${numVecs}, iter=${t}, vec=${k}, lane=${lane}): got=0x${got.toString(16)} exp=0x${exp.toString(16)}`);
                }
            }
        }
    }
}

verifyRun(inst.exports.run12, 12, 512, 100);
verifyRun(inst.exports.run16, 16, 1024, 100);
print("Passed");
