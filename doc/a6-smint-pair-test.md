# A6 — ARM64 `GenerateSmIntPairTest` never passes (fixed)

## Defect

`LowererMD::GenerateSmIntPairTest` (`lib/Backend/arm64/LowerMD.cpp`, was line
6616) is the shared pair-tag check for every var-var integer fast path on
ARM64: `GenerateFastAdd`, `GenerateFastSub`, `GenerateFastMul`,
`GenerateFastShiftRight`, and the `GenerateFastCmXxTaggedInt` comparison site
(line 5645). (`GenerateFastAnd/Or/Xor/ShiftLeft` are helper-only stubs on
ARM64, matching AMD64, so they never reach the pair test.)

The old lowering sequence was:

```
s1 = MOV src1
s1 = UBFX s1, VarTagShift - 16, 64 - (VarTag_Shift - 16)   // s1 = src1[63:32]
s2 = MOV src2
s1 = BFXIL s2, VarTagShift, 64 - VarTag_Shift              // insert s2[15:0] into s1[63:48]
s1 = EOR s1, AtomTag_Pair   (32-bit view: low 32 of s1)
CBNZ s1, $fail
```

## Bit derivation

64-bit tagged int Var layout (`INT32VAR`, `VarTag_Shift = 48`,
`lib/Runtime/RuntimeCommon.h:164-171`; constructors
`TaggedInt::ToVarUnchecked`, `lib/Runtime/Language/TaggedInt.inl`):

```
Var = (uint32)value | (0x1 << 48)
```

so for every genuine int `bits[63:32] == 0x00010000` (bit 48 set, bits
47:32 zero — the value lives in bits 31:0).

Step through the old sequence for an int pair:

1. `UBFX s1, src1, #32, #32` → `s1 = src1[63:32] = 0x00010000`.
2. `BFXIL s1, s2, #48, #16` inserts `s2[15:0]` into `s1[63:48]` — the
   **high** half of the 64-bit register.
3. The sequence then views `s1` as `TyInt32` (low 32 bits) and EORs vs
   `AtomTag_Pair = 0x00010001`. The low 32 bits are still just
   `src1[63:32]`; the inserted `s2` bits sit in bits 63:48 and are discarded.
4. `EOR32(0x00010000, 0x00010001) = 0x00000001 ≠ 0` → `CBNZ` always taken →
   **always fails**, for every genuine int pair.

The reference implementation is the x64 mirror
(`LowererMD::GenerateSmIntPairTest`, `lib/Backend/LowerMDShared.cpp:3822`,
`INT32VAR` branch):

```
s1 = SHR src1, VarTag_Shift     // 0x0001
s2 = SHR src2, 32               // 0x00010000
s1 = OR s1, s2                  // 0x00010001 == AtomTag_Pair
CMP s1, AtomTag_Pair / JNE $fail
```

which is exactly `TaggedInt::IsPair`
(`lib/Runtime/Language/TaggedInt.inl:57-63`):
`tags = (uint32)((uint64)aLeft >> 32 | (uint64)aRight >> 48)`.

## Fix

Restore identical semantics with ARM64 shifts (`lib/Backend/arm64/LowerMD.cpp`
`GenerateSmIntPairTest`):

```
s1 = MOV src1
s1 = LSR s1, VarTag_Shift       // src1[63:48] = 0x0001
s2 = MOV src2
s2 = LSR s2, 32                 // src2[63:32] = 0x00010000
s1 = ORR s1, s2                 // 0x00010001 == AtomTag_Pair
s1 = EOR s1, AtomTag_Pair       // 0 for int pairs (was CMP on x64; EOR+CBNZ kept)
CBNZ s1, $fail
```

For int pairs the EOR yields 0 and `CBNZ` falls through to the fast path;
for any non-int pair at least one tag half differs and the branch takes the
helper, exactly as on x64. One function, so all callers are fixed at once.
No new instruction classes are introduced (LSR/ORR/EOR/CBNZ immediates are
already used and encoded on this backend).

## Severity: perf restoration + latent-hazard removal, NOT a correctness fix

The defect could not produce a wrong result in any reachable configuration:

The pair test only gates entry to the int fast path; failure falls through
to the (correct) helper call. A never-passing test means "always slow",
never "sometimes wrong". The reverse hazard (a non-int pair wrongly
*passing*) is not present in either the old or the new sequence, and the
surrounding guards (GlobOpt type specialization, the float fast path,
`IsNotInt` / `IsLikelyString` early-outs) intercept non-int shapes first.
Proven empirically: 7 probe shapes (`/tmp/a6_evil*.js`, float64 aliasing
the pair tag `0x00010001` in its high word through Add/Mul/Sub under
forced JIT) all stayed correct/identical; JIT engagement confirmed by
timing (11 ms JIT vs 554 ms interpreted on the same probe).

## Regression test

`test/Basics/SmIntPairOps.js` (registered in `test/Basics/rlexe.xml`) hammers
the var-var Add/Sub/Mul/And/Or/Xor/Shl/Shr/ShrU paths plus tagged-int
comparisons in hot loops so JIT tiers compile them, asserts exact results
(including closed-form loop checksums), and pins the fail branch with the
`0x00010001`-high-word float plus float/string mixed-type cases. It passes
with the pair test disabled (helper fallback) and enabled (fast path) — it
guards the restored path, and the fail-branch cases would catch a future
mis-fix that wrongly passes non-int pairs.

## Validation

Worktree at `a1e1f6b1b` (on `modernize/baseline`), ARM64 macOS host.
Full JIT rebuild `mise x -- zig build -Dprofile=jit` → `BUILD_EXIT:0`
(`out/jit/bin/ch` + `libChakraCore.dylib`, 2026-11-14 21:29).

- Parent probes `/tmp/a6_evil2.js`–`/tmp/a6_evil8.js` (run in place, forced
  native JIT `-forceNative -off:simpleJit -bgJitDelay:0`): all correct and
  identical — `add`/`mul`/`sub` of the `0x00010001`-high-word float match
  the un-JITed want-value, every `buggy=` line `false`, exit 0.
- New `test/Basics/SmIntPairOps.js`: `PASS` exit 0 under forced native JIT
  and under `-nonative` (interpreter), i.e. passes with the pair test
  enabled and disabled, as designed.
- Own differential `/tmp/a6_diff.js` (var-var int add/sub/mul/shl/shr,
  float add, int comparisons incl. coercion, plus an int32-overflow mul
  exercising the `BVS` bailout): `DIFF-PASS` on all three tiers —
  interpreter (418 ms), forced native JIT (51 ms, ~8x, JIT engaged),
  simple JIT `-mic:1 -bgjit-`.
- Repo gate `mise x -- zig build -Dprofile=jit check` (serialized via
  `/tmp/opencode/cc-gate-lock.sh`) → `CHECK_EXIT:0`, zero `fail` lines:
  `check_smoke` PASS, `check_jit` PASS on both flag sets, bytecode
  `verify_bytecode_pure --jit` green ("Bytecode is up to date").
- `test/Basics/rlexe.xml` re-parsed as XML after the registration edit.

Nothing committed, per instructions.
