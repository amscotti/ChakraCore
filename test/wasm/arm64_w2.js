//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// WebAssembly on macOS ARM64, W2 (doc/wasm-arm64-plan.md): frontend and JS
// API bring-up with execution gated. Asserts validate/compile/instantiate,
// memory/table/global readback, and a controlled error on export call.
// ARM64-only behavior until W3 lands the interpreter (see rlexe entry).

const buf = readbuffer("arm64_w2.wasm");

console.log("validate=" + WebAssembly.validate(buf));
const mod = new WebAssembly.Module(buf);
console.log("exports=" + WebAssembly.Module.exports(mod).map(e => e.name + ":" + e.kind).join(","));
console.log("imports=" + WebAssembly.Module.imports(mod).map(e => e.module + "." + e.name + ":" + e.kind).join(","));

const inst = new WebAssembly.Instance(mod, { env: { inc: function(x) { return x + 1; } } });
console.log("instance exports=" + Object.keys(inst.exports).join(","));
console.log("g=" + inst.exports.g);

const mem = inst.exports.mem;
console.log("mem is Memory=" + (mem instanceof WebAssembly.Memory));
console.log("mem bytes=" + mem.buffer.byteLength);
const memBytes = new Uint8Array(mem.buffer);
console.log("data=" + memBytes[0] + "," + memBytes[1] + "," + memBytes[2] + "," + memBytes[3]);
console.log("mem grow=" + mem.grow(1));
console.log("mem bytes after grow=" + mem.buffer.byteLength);

const tbl = inst.exports.t;
console.log("t is Table=" + (tbl instanceof WebAssembly.Table));
console.log("t length=" + tbl.length);
console.log("t get0=" + tbl.get(0));
tbl.set(0, inst.exports.f);
console.log("t get0===f=" + (tbl.get(0) === inst.exports.f));
console.log("t grow=" + tbl.grow(1));
console.log("t length after grow=" + tbl.length);

const apiMem = new WebAssembly.Memory({ initial: 1, maximum: 2 });
console.log("api mem bytes=" + apiMem.buffer.byteLength);
console.log("api mem grow=" + apiMem.grow(1));
console.log("api mem bytes after grow=" + apiMem.buffer.byteLength);
const apiTbl = new WebAssembly.Table({ element: "anyfunc", initial: 1, maximum: 2 });
console.log("api t length=" + apiTbl.length);
apiTbl.set(0, inst.exports.f);
console.log("api t get0===f=" + (apiTbl.get(0) === inst.exports.f));
console.log("api t grow=" + apiTbl.grow(1));
console.log("api t length after grow=" + apiTbl.length);

try {
    const r = inst.exports.f();
    console.log("call returned " + r);
} catch (e) {
    console.log("call threw name=" + e.name + " message=" + e.message + " isRuntimeError=" + (e instanceof WebAssembly.RuntimeError));
}
