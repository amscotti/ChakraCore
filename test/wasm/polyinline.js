//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.Flag("-WasmMultiValue");
WScript.LoadScriptFile("../WasmSpec/testsuite/harness/wasm-constants.js");
WScript.LoadScriptFile("../WasmSpec/testsuite/harness/wasm-module-builder.js");

const buf = WebAssembly.wabt.convertWast2Wasm(`
(module
  (func (export "min") (param f64 f64) (result f64)
    (f64.min (get_local 0) (get_local 1))
  )

  (func (export "max") (param f64 f64) (result f64)
    (f64.max (get_local 0) (get_local 1))
  )
)`);
const corrupt = buf.slice(0);
const view = new Uint8Array(corrupt);
view[corrupt.byteLength - 1] = 6;
let rejected = false;
try
{
    new WebAssembly.Module(corrupt);
}
catch (e)
{
    rejected = e instanceof WebAssembly.CompileError;
}
if (!rejected)
{
    print("Fail: corrupted module was not rejected");
}
else
{
    var mod = new WebAssembly.Module(buf);
    var {min, max} = new WebAssembly.Instance(mod).exports;

    function foo(fn) {
      fn();
    }

    try {foo(min);} catch (e) {}
    try {foo(max);} catch (e) {}
    try {foo(min);} catch (e) {}

    function assert(cond, msg) {
      if (!cond) {
        throw new Error("Fail: " + msg);
      }
    }

    function assertTrapsRuntimeError(fn, label) {
      let trapped = false;
      try {
        fn();
      } catch (e) {
        trapped = e instanceof WebAssembly.RuntimeError;
      }
      assert(trapped, label + " should trap with WebAssembly.RuntimeError");
    }

    // 1. Live-compiled & executed (!IsValidating()) unreachable control-flow
    //    and label-0 / exitLabel collision checks for block, loop, and if.
    (function testUnreachableControlFlow() {
      const builder = new WasmModuleBuilder();
      const sig_i_i = builder.addType(kSig_i_i);

      builder.addFunction("unreachBlockVoid", kSig_v_v)
        .addBody([
          kExprUnreachable,
          kExprBlock, kWasmStmt,
            kExprNop,
          kExprEnd,
        ])
        .exportFunc();

      builder.addFunction("unreachBlockTyped", kSig_i_v)
        .addBody([
          kExprUnreachable,
          kExprBlock, kWasmI32,
            kExprI32Const, 7,
            kExprBr, 0,
          kExprEnd,
        ])
        .exportFunc();

      builder.addFunction("unreachLoopVoid", kSig_v_v)
        .addBody([
          kExprUnreachable,
          kExprLoop, kWasmStmt,
            kExprI32Const, 0,
            kExprBrIf, 0,
          kExprEnd,
        ])
        .exportFunc();

      builder.addFunction("unreachLoopTyped", kSig_i_v)
        .addBody([
          kExprUnreachable,
          kExprLoop, kWasmI32,
            kExprI32Const, 5,
            kExprI32Const, 0,
            kExprBrIf, 0,
          kExprEnd,
        ])
        .exportFunc();

      builder.addFunction("unreachIfTyped", kSig_i_v)
        .addBody([
          kExprUnreachable,
          kExprIf, kWasmI32,
            kExprI32Const, 11,
          kExprElse,
            kExprI32Const, 22,
          kExprEnd,
        ])
        .exportFunc();

      builder.addFunction("retBeforeUnreachBlock", sig_i_i)
        .addBody([
          kExprGetLocal, 0,
          kExprIf, kWasmStmt,
            kExprI32Const, 42,
            kExprReturn,
          kExprEnd,
          kExprUnreachable,
          kExprBlock, kWasmI32,
            kExprI32Const, 99,
            kExprBr, 0,
          kExprEnd,
          kExprDrop,
          kExprI32Const, 77,
        ])
        .exportFunc();

      builder.addFunction("retBeforeUnreachLoop", sig_i_i)
        .addBody([
          kExprGetLocal, 0,
          kExprIf, kWasmStmt,
            kExprI32Const, 43,
            kExprReturn,
          kExprEnd,
          kExprUnreachable,
          kExprLoop, kWasmI32,
            kExprI32Const, 99,
            kExprI32Const, 0,
            kExprBrIf, 0,
          kExprEnd,
          kExprDrop,
          kExprI32Const, 78,
        ])
        .exportFunc();

      builder.addFunction("retBeforeUnreachIf", sig_i_i)
        .addBody([
          kExprGetLocal, 0,
          kExprIf, kWasmStmt,
            kExprI32Const, 44,
            kExprReturn,
          kExprEnd,
          kExprUnreachable,
          kExprIf, kWasmI32,
            kExprI32Const, 101,
          kExprElse,
            kExprI32Const, 102,
          kExprEnd,
          kExprDrop,
          kExprI32Const, 79,
        ])
        .exportFunc();

      const inst = builder.instantiate();
      const ex = inst.exports;

      assertTrapsRuntimeError(() => ex.unreachBlockVoid(), "unreachBlockVoid");
      assertTrapsRuntimeError(() => ex.unreachBlockTyped(), "unreachBlockTyped");
      assertTrapsRuntimeError(() => ex.unreachLoopVoid(), "unreachLoopVoid");
      assertTrapsRuntimeError(() => ex.unreachLoopTyped(), "unreachLoopTyped");
      assertTrapsRuntimeError(() => ex.unreachIfTyped(), "unreachIfTyped");

      assertTrapsRuntimeError(() => ex.retBeforeUnreachBlock(0), "retBeforeUnreachBlock(0)");
      assertTrapsRuntimeError(() => ex.retBeforeUnreachLoop(0), "retBeforeUnreachLoop(0)");
      assertTrapsRuntimeError(() => ex.retBeforeUnreachIf(0), "retBeforeUnreachIf(0)");

      for (let i = 0; i < 20; ++i) {
        assert(ex.retBeforeUnreachBlock(1) === 42, "retBeforeUnreachBlock(1)");
        assert(ex.retBeforeUnreachLoop(1) === 43, "retBeforeUnreachLoop(1)");
        assert(ex.retBeforeUnreachIf(1) === 44, "retBeforeUnreachIf(1)");
      }
    })();

    // 2. Cross-realm multi-value WASM dispatch (CrossSite::DefaultThunk/ProfileThunk),
    //    ShouldJitFunction multi-value gate, and retBuf stack probe.
    (function testCrossRealmAndStackProbeMultiValue() {
      const builder = new WasmModuleBuilder();
      const sigPair = builder.addType({ params: [kWasmI32, kWasmI32], results: [kWasmI32, kWasmI32] });
      const sigFPair = builder.addType({ params: [kWasmF64, kWasmF64], results: [kWasmF64, kWasmF64] });
      const sigQuad = builder.addType({ params: [kWasmI32], results: [kWasmI32, kWasmI32, kWasmI32, kWasmI32] });

      builder.addFunction("pair", sigPair)
        .addBody([
          kExprGetLocal, 0,
          kExprI32Const, 1,
          kExprI32Add,
          kExprGetLocal, 1,
          kExprI32Const, 2,
          kExprI32Add,
        ])
        .exportFunc();

      builder.addFunction("fpair", sigFPair)
        .addBody([
          kExprGetLocal, 0,
          kExprGetLocal, 1,
          kExprF64Add,
          kExprGetLocal, 1,
          kExprGetLocal, 1,
          kExprF64Add,
        ])
        .exportFunc();

      builder.addFunction("quad", sigQuad)
        .addBody([
          kExprGetLocal, 0,
          kExprGetLocal, 0,
          kExprI32Const, 1,
          kExprI32Add,
          kExprGetLocal, 0,
          kExprI32Const, 2,
          kExprI32Add,
          kExprGetLocal, 0,
          kExprI32Const, 3,
          kExprI32Add,
        ])
        .exportFunc();

      // 4096 i32 results => retBuf = 32768 bytes (16x MinStackDefault).
      const manyCount = 4096;
      const manySig = builder.addType({ params: [], results: new Array(manyCount).fill(kWasmI32) });
      const manyBody = [];
      for (let k = 0; k < manyCount; ++k) {
        manyBody.push(kExprI32Const);
        // Signed LEB128 for k in [0, 4095]
        let val = k;
        while (true) {
          let b = val & 0x7f;
          val >>= 7;
          if ((val === 0 && (b & 0x40) === 0) || (val === -1 && (b & 0x40) !== 0)) {
            manyBody.push(b);
            break;
          }
          manyBody.push(b | 0x80);
        }
      }
      builder.addFunction("many", manySig).addBody(manyBody).exportFunc();

      const mvBytes = builder.toBuffer();
      const localInst = new WebAssembly.Instance(new WebAssembly.Module(mvBytes));
      const { pair, fpair, quad, many } = localInst.exports;

      function checkPair(res, expected0, expected1, label) {
        assert(Array.isArray(res) && res.length === 2, label + " should return length-2 Array");
        assert(res[0] === expected0 && res[1] === expected1, label + " mismatch: " + res);
      }

      function checkQuad(res, n, label) {
        assert(Array.isArray(res) && res.length === 4, label + " should return length-4 Array");
        assert(res[0] === n && res[1] === n + 1 && res[2] === n + 2 && res[3] === n + 3, label + " mismatch: " + res);
      }

      // Hot loop in originating realm (exercises ShouldJitFunction gate & -forceNative).
      for (let i = 0; i < 40; ++i) {
        checkPair(pair(i, i + 10), i + 1, i + 12, "local pair");
        checkPair(fpair(i, 2.5), i + 2.5, 5.0, "local fpair");
        checkQuad(quad(i), i, "local quad");
      }

      const otherRealm = WScript.LoadScript(`
        function callCrossPair(fn, a, b) {
          return fn(a, b);
        }
        function callCrossQuadHot(fn, count) {
          let last = null;
          for (let i = 0; i < count; ++i) {
            last = fn(i);
          }
          return last;
        }
        function makeCrossExports(bytes) {
          return new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports;
        }
      `, "samethread");

      // Direction 1: call main-realm exports from otherRealm.
      for (let i = 0; i < 20; ++i) {
        checkPair(otherRealm.callCrossPair(pair, i, 10), i + 1, 12, "main->other pair");
        checkPair(otherRealm.callCrossPair(fpair, i, 2.5), i + 2.5, 5.0, "main->other fpair");
      }
      checkQuad(otherRealm.callCrossQuadHot(quad, 30), 29, "main->other quad hot");

      // Direction 2: instantiate in otherRealm, call exported functions from main realm.
      const remoteExports = otherRealm.makeCrossExports(mvBytes);
      for (let i = 0; i < 40; ++i) {
        checkPair(remoteExports.pair(i, 20), i + 1, 22, "other->main pair");
        checkPair(remoteExports.fpair(i, 1.5), i + 1.5, 3.0, "other->main fpair");
        checkQuad(remoteExports.quad(i), i, "other->main quad");
      }

      // Verify many-result function works when sufficient stack is available.
      const manyRes = many();
      assert(Array.isArray(manyRes) && manyRes.length === manyCount, "many length");
      assert(manyRes[0] === 0 && manyRes[1234] === 1234 && manyRes[manyCount - 1] === manyCount - 1, "many values");

      // Verify retBuf stack probe rejects before _alloca when remaining stack < 32KB.
      let unwinds = 0;
      function recurseAndCallMany() {
        try {
          return recurseAndCallMany();
        } catch (e) {
          if (++unwinds === 6) {
            try {
              many();
              return "no-throw";
            } catch (e2) {
              return "caught:" + (e2 && e2.message);
            }
          }
          throw e;
        }
      }
      const probeResult = recurseAndCallMany();
      assert(typeof probeResult === "string" && probeResult.startsWith("caught:"),
        "Expected stack overflow from many() near stack limit, got: " + probeResult);
    })();

    // 3. WasmModuleBuilder.addBody() immediate-aware decoding for bytes 0x02, 0x03, 0x04, 0x0b.
    (function testAddBodyImmediates() {
      const immediateBytes = [0x02, 0x03, 0x04, 0x0b];
      for (const imm of immediateBytes) {
        // i32.const <imm> without and with explicit trailing kExprEnd
        for (const withEnd of [false, true]) {
          const b = new WasmModuleBuilder();
          const body = withEnd ? [kExprI32Const, imm, kExprEnd] : [kExprI32Const, imm];
          b.addFunction("f", kSig_i_v).addBody(body).exportFunc();
          const res = b.instantiate().exports.f();
          assert(res === imm, "i32.const imm=" + imm + " withEnd=" + withEnd + " got " + res);
        }
      }

      // Multi-byte LEB128 i32.const whose bytes include 0x82 and 0x0b -> (0x0b << 7) | 2 = 1410
      for (const withEnd of [false, true]) {
        const b = new WasmModuleBuilder();
        const body = withEnd ? [kExprI32Const, 0x82, 0x0b, kExprEnd] : [kExprI32Const, 0x82, 0x0b];
        b.addFunction("f", kSig_i_v).addBody(body).exportFunc();
        const res = b.instantiate().exports.f();
        assert(res === 1410, "multi-byte i32.const withEnd=" + withEnd + " got " + res);
      }

      // f32.const with raw bytes [0x02, 0x03, 0x04, 0x0b]
      const f32Expected = new Float32Array(new Uint8Array([0x02, 0x03, 0x04, 0x0b]).buffer)[0];
      for (const withEnd of [false, true]) {
        const b = new WasmModuleBuilder();
        const body = withEnd
          ? [kExprF32Const, 0x02, 0x03, 0x04, 0x0b, kExprEnd]
          : [kExprF32Const, 0x02, 0x03, 0x04, 0x0b];
        b.addFunction("f", kSig_f_v).addBody(body).exportFunc();
        const res = b.instantiate().exports.f();
        assert(res === f32Expected, "f32.const withEnd=" + withEnd + " got " + res);
      }

      // local.get with local index 0x02, 0x03, 0x04, 0x0b (12 i32 params)
      const sig12 = makeSig(new Array(12).fill(kWasmI32), [kWasmI32]);
      const args12 = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120];
      for (const idx of immediateBytes) {
        for (const withEnd of [false, true]) {
          const b = new WasmModuleBuilder();
          const body = withEnd ? [kExprGetLocal, idx, kExprEnd] : [kExprGetLocal, idx];
          b.addFunction("f", sig12).addBody(body).exportFunc();
          const res = b.instantiate().exports.f(...args12);
          assert(res === args12[idx], "local.get idx=" + idx + " withEnd=" + withEnd + " got " + res);
        }
      }

      // Block containing immediates 0x04 and 0x0b without/with trailing kExprEnd
      for (const withEnd of [false, true]) {
        const b = new WasmModuleBuilder();
        const body = withEnd
          ? [kExprBlock, kWasmI32, kExprI32Const, 0x04, kExprDrop, kExprI32Const, 0x0b, kExprEnd, kExprEnd]
          : [kExprBlock, kWasmI32, kExprI32Const, 0x04, kExprDrop, kExprI32Const, 0x0b, kExprEnd];
        b.addFunction("f", kSig_i_v).addBody(body).exportFunc();
        const res = b.instantiate().exports.f();
        assert(res === 0x0b, "block imm withEnd=" + withEnd + " got " + res);
      }

      // Negative/truncated bodies must not throw inside addBody(), and must fail module compile
      for (const badBody of [[kExprCallIndirect, 1], [0xf0, 0x00]]) {
        const b = new WasmModuleBuilder();
        b.addFunction("bad", kSig_v_v).addBody(badBody).exportFunc();
        let threwCompileError = false;
        try {
          new WebAssembly.Module(b.toBuffer());
        } catch (e) {
          threwCompileError = e instanceof WebAssembly.CompileError;
        }
        assert(threwCompileError, "badBody " + badBody + " should fail with WebAssembly.CompileError");
      }
    })();

    print("Pass");
}