# P2 FullJIT gate receipt — wasm/arm64 value-path fixes

Date: 2026-11-12. Branch: `modernize/baseline`. Binary: `out/jit/bin/libChakraCore.dylib`
(built from this commit + widening-guard rebuild verified identical results).

## Gate result (fresh, `/tmp/p2jit_fresh.log`)

`python3 tools/build/check_wasm_p1.py --jit test/runtests.py "$PWD/out/jit/bin/ch"`:

- **P1 JIT-pass: 250/381** (failed 4, missing 0, unmatched 0, duplicates 0)
- interp-pinned pass: 24; excluded-absent: 103; runtests Total 315/19
- The only 4 failures are the stack-exhaustion family handed to W6:
  `WasmSpec[33]` call.wast, `WasmSpec[35]` call_indirect.wast,
  `WasmSpec[63]` fac.wast, `WasmSpec[129]` skip-stack-guard-page.wast.
  JIT spins then SIGSEGVs on native overflow where interp throws
  "Out of stack space". GenerateStackProbe emission verified present;
  probe-vs-unwinder interaction untriaged (W6).

First P2 run (pre-fix) was 241/381 with 13 fast fails; 9 of 13 fixed here,
remaining 4 = single workstream above.

## Fixes in this commit

1. **LDRS 64-bit sign-extend** (`lib/Backend/arm64/EncoderMD.cpp`,
   `GenerateEncoding` ~L999): i64.load8_s/16_s emitted 32-bit Wt
   LDRSB/LDRSH (zeroing upper 32 bits); now emits 64-bit Xt forms.
   Evidence: `0x...FFFFFFEF` sext mismatches resolved; `load*_equals_zext32=0`.
2. **Delay-thunk f64 first-call** (`lib/Runtime/Language/InterpreterStackFrame.cpp`
   ~L1832, Double case): `Box()` clobbers d0 before the return-value restore;
   added fmov restore of d0 after Box. Evidence: repeat-call f64 NaN-payload
   probes steady-state correct.
3. **f32.const sNaN quieting** (`lib/Backend/arm64/LowerMD.{h,cpp}`,
   float `LoadFloatValue` overload ~L7766): float args previously bound the
   hidden double overload (name hiding) and round-tripped float->double->float,
   quieting sNaN (`0x7FA00000` -> `0x7FE00000`). New overload pools exact
   4 bytes for Float32 dst; Float64 dst delegates to the double overload
   (widening preserved). Evidence: `const-bits=1`, float_memory 90/90,
   f32+f64 2501/2501, float suites all green.
4. **P2 checker** (`tools/build/check_wasm_p1.py`): `--jit` (dynapogo
   FullJIT `-forceNative -off:simpleJit -bgJitDelay:0`) mode; interp-pinned
   (`-nonative` flag) and `exclude_dynapogo` entries graded separately
   (pinned_passed/excluded), never counted as JIT pass; 31 self-tests
   (`--selftest`: `ALL NEGATIVE CONTROLS BEHAVE`).

## Suite evidence (final binary, FullJIT)

f32 2501/2501, f64 2501/2501, i32 360/360, i64 360/360, address 243/243,
endianness 69/69, float_exprs 900/900, float_misc 441/441, memory 71/71;
misc.js PASS, memory.js PASS; fastarray variants PASS;
GC-stress checksum=1531722802 both tiers (unchanged).

## Open (not in this commit)

- W6 stack-exhaustion unwinder work (4 tests above).
- W7 function multi-return (reader throws; I_Call wires result 0 only).
- W8 SIMD/NEON, W9 threads/atomics (§9.2 default-on decision).
