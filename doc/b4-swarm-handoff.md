# B4 swarm review — handoff and deferred-work register

Status: **complete for the applied batch**; branch clean and pushed.
Date: 2026-11-10. Branch: `modernize/baseline`.
Starting review point: `4779e73bc`. Current HEAD at handoff: `250d25175`.

## 1. What was reviewed

Ten read-only reviewers covered the branch from independent angles:
architecture/layering, memory management, parser/regex, JIT/backend,
runtime/library, build/tooling, test coverage, portability/ABI,
concurrency/async/debugger, and security/robustness. Each finding was
required to carry a `file:line` anchor plus a reproduction, probe output,
or a concrete correctness argument. Findings were deduplicated and
triaged by severity and reachability.

Two findings were disproved by the reviewers themselves:
- the recorded `test/es7/class-private-methods.js` SIGSEGV did not
  reproduce in 320 runs;
- the `AsyncSuspendedResume` exclusion was not caused by the Attach
  message loop (the actual cause is the coroutine reparse, see §4).

## 2. Fixes applied and verified

| Fix | Location |
|---|---|
| Typed-array integer index guarded before `uint32` cast; `a[2^32]` no longer writes element 0 | `lib/Runtime/Library/TypedArray.cpp` |
| Oversized `repeat`/`padStart`/`padEnd` throw catchable `RangeError` instead of aborting in `LargeAlloc` | `lib/Runtime/Library/JavascriptString.cpp` |
| Remaining double→int32/int64 casts guarded; `ToInt32Core`, `TryToInt64`, `toFixed`/`toExponential`/`toPrecision`/`toString(radix)` | `lib/Runtime/Math/JavascriptMath.inl`, `lib/Common/Common/NumberUtilities.inl`, `lib/Runtime/Library/JavascriptNumber.cpp` |
| `toExponential`/`toPrecision` check ordering per spec: coerce first, return NaN/Infinity, then finite digit-range cap | `lib/Runtime/Library/JavascriptNumber.cpp` |
| FDE CIE skip bounded; no trap on malformed input; deregistration reuses the exact registered FDE pointer | `lib/Common/PlatformAgnostic/AssemblyCommon.h`, `lib/Common/Memory/amd64/XDataAllocator.{h,cpp}` |
| Regex literal terminator cached per literal; removes the O(classes × literal length) speculative scan | `lib/Parser/RegexParser.{h,cpp}` |
| Validation-only WASM codegen no longer commits frame/register layout | `lib/WasmReader/WasmByteCodeGenerator.cpp` |
| Regression coverage for fround/exp folds, v-scan trailing text, and typed-array out-of-range indices | `test/es6/ES6MathAPIs.js`, `test/es7/regexp-v-flag.js`, `test/typedarray/typedarray_bugfixes.js` |
| B4 gate doc corrected: the Linux/Mac engine/feature buckets are not identical (+154 engine-defect, +76 feature, host 14 vs 3) | `doc/b4-gate.md` |

## 3. Verification evidence

Binary at verification: `ch` SHA-256 `054addb0fa67a8fb…`,
`libChakraCore.so` SHA-256 `56bf1615865f246e…`.

| Check | Result |
|---|---|
| `zig build` | green |
| `check-smoke` | PASS |
| `check-metadata` | PASS: 237 exports / 1 marker; 669 archive members (90 PAL / 1 Codex / 11 PlatformAgnostic) |
| `check-jit` | PASS |
| `verify-bytecode` | PASS |
| `check-selftest` | PASS |
| `check-regression` (test profile) | interpreted **2841/0**, dynapogo **2305/0**, exact pin match |
| `ES6MathAPIs.js` | 19/19 |
| `regexp-v-flag.js` | 34/34 |
| `typedarray_bugfixes.js` | 3/3 |
| Targeted test262 (Number, TypedArray, TypedArrayConstructors, String repeat/pad) | **4910 passed / 190 failed / 0 new regressions** vs `out/test262-cert-linux-final` |

The 190 targeted test262 failures are the pre-existing certified buckets
(SAB flag-off, TypedArray `length` descriptors, ES2018 digit ranges,
formatter exactness). Full certified test262 was **not** rerun on this
batch; the targeted comparison is the evidence for no regression.

Linux has no `-Dprofile=debug-static`; that failure is a configure-time
"unsupported profile" error, not a test failure. The Mac cross-check
receipt is commit `2a2aee16d`.

## 4. Deferred work (authoritative register)

Each card below is also in `doc/open-cards.md` §8. Start with the ones
marked high.

### D1. Async debugger attach / suspended coroutine reparse (high)
- **Symptoms:** `test/DebuggerCommon/AsyncSuspendedResume.js` hangs;
  `WScript.Detach` with a live suspended async frame SIGSEGVs;
  a generator that completes after an attach reparse returns
  `undefined` instead of `{value,done}`.
- **Root cause:** the debugger reparses a live plain-async `FunctionBody`
  in place, lengthening its bytecode. `GeneratorStateHelper::IsDone`
  compares the suspended frame's old reader offset with the current
  body length and never reports completion; `AsyncSpawnStep` re-chains
  on `InternalPromiseResolve(undefined)` forever.
- **Why deferred:** `ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD` is 0 on
  Linux x64 and is coupled to `GLOBAL_ENABLE_WRITE_BARRIER`; the guard's
  frame count is decremented from the generator finalizer. Enabling the
  macro without a working frame-lifetime path would pin bytecode
  permanently.
- **Direction:** either make the reparse out-of-place for plain async
  bodies on all targets (the guard already has
  `NewDebugReparseBodyForSuspendedAsync`), or add a frame-lifetime
  mechanism that does not depend on the finalized-allocation arm.
  Re-enable `AsyncSuspendedResume.js` in the gate and add detach +
  generator-completion probes.

### D2. ES2018 100-digit Number formatting (medium)
- **Gap:** `toFixed`/`toPrecision`/`toExponential` accept only 20/21
  digits; ES2018 allows 100.
- **Why deferred:** the formatter computes at most 20 significant digits
  (`kcchMaxSig` in `NumberUtilities_strtod.cpp`). Raising the cap without
  an exact-conversion path emits wrong low digits.
- **Direction:** implement an exact double→decimal path (Ryu or equivalent)
  for the 21..100 range, or use a correctly-rounded `to_chars`/printf
  fallback behind a feature check. Then raise the caps and update the
  certified test262 buckets.

### D3. `Number.prototype.toString(radix)` low digits (medium-low)
- **Gap:** large integer-valued doubles produce wrong low-order digits
  for non-power-of-two radices; 195/439 fuzz cases mismatch exact base-36.
  The 180 test262 `Number/toString` tests are blind here.
- **Direction:** replace the double-division digit loop with an exact
  big-integer conversion; add a differential test against Python/Node
  for large magnitudes.

### D4. `WebAssembly.Module` vs `WebAssembly.validate` (medium)
- **Gap:** deferred bodies are not validated at construction; invalid
  bytes can construct a Module that only throws on first call.
- **Why deferred:** eager `ValidateFunction` mutated the shared
  `WasmReaderInfo`/function info and rejected valid import/instance
  modules (`WasmSpec/jsapi`). It was reverted.
- **Direction:** add a side-effect-free validator (separate reader state,
  no `CommitToFunctionInfo`), or compile deferred bodies at construction
  and accept the startup cost. Gate it against both `WasmSpec/jsapi` and
  invalid-body constructor tests.

### D5. ARM/ARM64 CIE-vs-FDE registration (medium)
- **Gap:** `ChakraEhFrameFdeStart` is x86-64 gated; `arm/` and `arm64/`
  `XDataAllocator` still pass the CIE start to `__register_frame`.
  Non-Apple ARM64 emits Windows `.xdata`, so "ARM64 support" currently
  means macOS ARM64 only.
- **Direction:** select by buffer format at each call site, not by
  architecture; add a cross-target build/run check.

### D6. `_LIBCPP_VERSION` unwinder-contract proxy (medium)
- **Gap:** the FDE-vs-CIE choice is inferred from a stdlib header macro,
  not the linked unwinder. A libstdc++-headers + LLVM-libunwind build
  would register nothing.
- **Direction:** introduce a build-contract define for the unwinder
  contract; verify with a libgcc control build.

### D7. Gate/tooling gaps (low–medium)
- FullJit without a dynamic profile is ungated (reviewer probe 287/0,
  35 s); add it as a third regression variant.
- `check-jit` proves an `nm` symbol plus identical stdout, not that the
  JIT executed; add a JIT-only observable/counter.
- Deep legacy regex class nesting hard-fails at ~36k through the
  `VValidateClass` stack probe; Node accepts 40k.
- `gen_tables_linux.py`/`tables_linux.zig` drift: the generated table was
  hand-edited for WeakRef/FinalizationRegistry and the generator's
  reference predates them.

## 5. Suggested order for the next agent

1. Run the **full certified test262** on `250d25175` and record the
   before/after key-join. This is the missing global evidence.
2. D1, because it is the only crash/hang family still reachable from the
   debugger surface; land it with the excluded test re-enabled.
3. D2 and D3 together (both need an exact decimal/big-integer path).
4. D4 with a side-effect-free validator; keep the reverted experiment out.
5. D5/D6 before any Linux-ARM64 or alternate-unwinder work.
6. D7 as gate hardening.

## 6. Operating constraints (carry forward)

- Serialize every build, test262 run, and regression suite through
  `/tmp/opencode/cc-gate-lock.sh`; reclaim a stale lock only after
  verifying no live gate process.
- Use Zig 0.17.0 via mise; never a system Zig.
- Test262 only via `python3 tools/test262/run.py` with an absolute
  `--binary`; corpus pin is `2e0a5676…`.
- Regression pins live in `tools/build/check_regression_linux.py`
  (`interpreted 2841/0`, `dynapogo 2305/0`).
- Do not pin or update a count to hide a failure; investigate first.
- Every finding/fix needs a `file:line` anchor and a reproduction or
  differential proof; style preferences are not bugs.
- Keep engine code/tests/comments free of plan-phase references.
