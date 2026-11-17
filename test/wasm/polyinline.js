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

    // Item #9: Mixed-type multi-value validation, stack/tuple order, and control flow.
    (function testMixedTypeMultiValue() {
      function f32Bytes(v) {
        const buf = new ArrayBuffer(4);
        new Float32Array(buf)[0] = v;
        return Array.from(new Uint8Array(buf));
      }
      function f64Bytes(v) {
        const buf = new ArrayBuffer(8);
        new Float64Array(buf)[0] = v;
        return Array.from(new Uint8Array(buf));
      }
      function assertTuple(actual, expected, msg) {
        assert(Array.isArray(actual), msg + ": expected array, got " + typeof actual);
        assert(actual.length === expected.length, msg + ": length " + actual.length + " != " + expected.length);
        for (let i = 0; i < expected.length; i++) {
          assert(Object.is(actual[i], expected[i]), msg + "[" + i + "]: got " + actual[i] + " expected " + expected[i]);
        }
      }

      // 1. Mixed (i32, f64) implicit return & explicit return; swapped stack order rejected.
      {
        const b = new WasmModuleBuilder();
        const sigI32F64 = makeSig([kWasmI32], [kWasmI32, kWasmF64]);
        b.addFunction("retPair", sigI32F64)
          .addBody([
            kExprGetLocal, 0,
            kExprIf, kWasmStmt,
              kExprI32Const, 0x79, // -7 signed LEB128
              kExprF64Const, ...f64Bytes(3.25),
              kExprReturn,
            kExprEnd,
            kExprI32Const, 42,
            kExprF64Const, ...f64Bytes(-1.5)
          ])
          .exportFunc();
        const exp = b.instantiate().exports;
        assertTuple(exp.retPair(1), [-7, 3.25], "mixed explicit return");
        assertTuple(exp.retPair(0), [42, -1.5], "mixed implicit return");

        // Swapped stack order (f64, i32) for (result i32 f64) must fail validation.
        const bad = new WasmModuleBuilder();
        bad.addFunction("badOrder", makeSig([], [kWasmI32, kWasmF64]))
          .addBody([kExprF64Const, ...f64Bytes(1.0), kExprI32Const, 1]);
        assert(!WebAssembly.validate(bad.toBuffer()), "swapped (f64, i32) must not validate as (i32, f64)");
      }

      // 2. 4-type mixed tuple (f32, i32, i64, f64) across block param/result & consumer ops.
      {
        const b = new WasmModuleBuilder();
        const sigFourBlock = b.addType(makeSig([kWasmF32, kWasmI32, kWasmI64, kWasmF64], [kWasmF32, kWasmI32, kWasmF64]));
        const sigOut = makeSig([], [kWasmF32, kWasmI32, kWasmF64]);
        b.addFunction("fourTypes", sigOut)
          .addLocals({ f64_count: 1 })
          .addBody([
            kExprF32Const, ...f32Bytes(1.5),
            kExprI32Const, 42,
            kExprI64Const, 5,
            kExprF64Const, ...f64Bytes(9.25),
            kExprBlock, sigFourBlock,
              // stack inside block: [f32(1.5), i32(42), i64(5), f64(9.25)]
              kExprSetLocal, 0,       // save f64
              kExprI32ConvertI64,     // wrap i64 -> i32(5)
              kExprI32Add,            // 42 + 5 = 47
              kExprGetLocal, 0,       // restore f64(9.25)
            kExprEnd
          ])
          .exportFunc();
        const res = b.instantiate().exports.fourTypes();
        assertTuple(res, [1.5, 47, 9.25], "4-type mixed block");
      }

      // 3. Mixed (i32, f64) block + br_if + br_table order preservation.
      {
        const b = new WasmModuleBuilder();
        const sigBlock = b.addType(makeSig([], [kWasmI32, kWasmF64]));
        b.addFunction("brIfMixed", makeSig([kWasmI32], [kWasmI32, kWasmF64]))
          .addBody([
            kExprBlock, sigBlock,
              kExprI32Const, 11,
              kExprF64Const, ...f64Bytes(2.5),
              kExprGetLocal, 0,
              kExprBrIf, 0,
              kExprDrop,
              kExprDrop,
              kExprI32Const, 33,
              kExprF64Const, ...f64Bytes(7.5),
            kExprEnd
          ])
          .exportFunc();

        b.addFunction("brTableMixed", makeSig([kWasmI32], [kWasmI32, kWasmF64]))
          .addBody([
            kExprBlock, sigBlock,
              kExprBlock, sigBlock,
                kExprI32Const, 20,
                kExprF64Const, ...f64Bytes(4.25),
                kExprGetLocal, 0,
                kExprBrTable, 1, 0, 1, // 1 target (label 0), default label 1
              kExprEnd,
              kExprReturn,
            kExprEnd
          ])
          .exportFunc();

        const exp = b.instantiate().exports;
        assertTuple(exp.brIfMixed(1), [11, 2.5], "br_if taken mixed");
        assertTuple(exp.brIfMixed(0), [33, 7.5], "br_if untaken mixed");
        assertTuple(exp.brTableMixed(0), [20, 4.25], "br_table target 0 mixed");
        assertTuple(exp.brTableMixed(1), [20, 4.25], "br_table default mixed");
      }

      // 4. Mixed-type loop (param i32 f64) (result i32 f64) with back-edge br_if 0.
      {
        const b = new WasmModuleBuilder();
        const sigLoop = b.addType(makeSig([kWasmI32, kWasmF64], [kWasmI32, kWasmF64]));
        b.addFunction("loopMixed", makeSig([kWasmI32], [kWasmI32, kWasmF64]))
          .addLocals({ i32_count: 1, f64_count: 1 })
          .addBody([
            kExprGetLocal, 0,
            kExprF64Const, ...f64Bytes(0.0),
            kExprLoop, sigLoop,
              // locals: 0=init, 1=curI, 2=accF
              kExprSetLocal, 2,
              kExprTeeLocal, 1,
              kExprI32Const, 1,
              kExprI32Sub,                  // nextI = curI - 1
              kExprGetLocal, 2,
              kExprGetLocal, 1,
              kExprF64SConvertI32,
              kExprF64Add,                  // nextF = accF + double(curI)
              kExprGetLocal, 1,
              kExprI32Const, 1,
              kExprI32GtS,                  // continue while curI > 1
              kExprBrIf, 0,
            kExprEnd
          ])
          .exportFunc();
        const exp = b.instantiate().exports;
        assertTuple(exp.loopMixed(5), [0, 15.0], "loop (param i32 f64) n=5");
        assertTuple(exp.loopMixed(1), [0, 1.0], "loop (param i32 f64) n=1");
      }

      // 5. Mixed-type if (param i32 f64) (result i32 f64) with else and without else.
      {
        const b = new WasmModuleBuilder();
        const sigIf = b.addType(makeSig([kWasmI32, kWasmF64], [kWasmI32, kWasmF64]));
        b.addFunction("ifWithElse", makeSig([kWasmI32, kWasmF64, kWasmI32], [kWasmI32, kWasmF64]))
          .addLocals({ f64_count: 1 })
          .addBody([
            kExprGetLocal, 0,
            kExprGetLocal, 1,
            kExprGetLocal, 2,
            kExprIf, sigIf,
              kExprSetLocal, 3,
              kExprI32Const, 10,
              kExprI32Add,
              kExprGetLocal, 3,
              kExprF64Const, ...f64Bytes(2.0),
              kExprF64Mul,
            kExprElse,
              kExprSetLocal, 3,
              kExprI32Const, 20,
              kExprI32Add,
              kExprGetLocal, 3,
              kExprF64Const, ...f64Bytes(1.5),
              kExprF64Add,
            kExprEnd
          ])
          .exportFunc();

        b.addFunction("ifNoElse", makeSig([kWasmI32, kWasmF64, kWasmI32], [kWasmI32, kWasmF64]))
          .addLocals({ f64_count: 1 })
          .addBody([
            kExprGetLocal, 0,
            kExprGetLocal, 1,
            kExprGetLocal, 2,
            kExprIf, sigIf,
              kExprSetLocal, 3,
              kExprI32Const, 1,
              kExprI32Add,
              kExprGetLocal, 3,
              kExprF64Const, ...f64Bytes(0.5),
              kExprF64Add,
            kExprEnd
          ])
          .exportFunc();

        const exp = b.instantiate().exports;
        assertTuple(exp.ifWithElse(5, 3.5, 1), [15, 7.0], "if-else true");
        assertTuple(exp.ifWithElse(5, 3.5, 0), [25, 5.0], "if-else false");
        assertTuple(exp.ifNoElse(5, 3.5, 1), [6, 4.0], "if-no-else true");
        assertTuple(exp.ifNoElse(5, 3.5, 0), [5, 3.5], "if-no-else false");
      }

      // 6. Unreachable polymorphic stack + mixed nested param block + partial br_if.
      {
        const b = new WasmModuleBuilder();
        const sigBlock = b.addType(makeSig([kWasmI32, kWasmF64], [kWasmI32, kWasmF64]));
        const sigRet = b.addType(makeSig([], [kWasmI32, kWasmF64]));
        b.addFunction("unreachNested", sigRet)
          .addBody([
            kExprUnreachable,
            kExprBlock, sigBlock,
              kExprDrop,
              kExprI32Const, 7,
              kExprI32Add,
              kExprF64Const, ...f64Bytes(2.5),
            kExprEnd,
            kExprDrop,
            kExprDrop,
            kExprF64Const, ...f64Bytes(1.0),
            kExprI32Const, 1,
            kExprBrIf, 0
          ])
          .exportFunc();
        assert(WebAssembly.validate(b.toBuffer()), "unreachable nested mixed param block + partial br_if must validate");

        // Negative: unreachable + wrong top-of-stack type (f32 instead of f64) must fail validation.
        const bad = new WasmModuleBuilder();
        bad.addFunction("badUnreach", makeSig([], [kWasmI32, kWasmF64]))
          .addBody([kExprUnreachable, kExprF32Const, ...f32Bytes(1.0)]);
        assert(!WebAssembly.validate(bad.toBuffer()), "unreachable with wrong f32 top-of-stack for (i32, f64) must fail");
      }
    })();

    print("Pass");
}