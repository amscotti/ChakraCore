//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-JIT: immutable-store fast-path conformance fix (jit-closure-1 holes 1+2)

Status: **fixed** — lane `jit-immutable-store`, receipt
[doc/c2-jit-immutable-store.md](c2-jit-immutable-store.md). Guard design,
perf numbers, verification tables and residuals are recorded there.

## Root cause

Two store paths reach TypedArray buffer memory without the
`[[ArrayBufferIsImmutable]]` check that every helper implements
(`BaseTypedDirectSetItem`/`BaseTypedBigIntDirectSetItem`,
lib/Runtime/Library/TypedArray.h):

1. **JIT inline indexed store (hole 1, runtime-proven).**
   `StElemI_A`/`StElemI_A_Strict` on a site whose base profiles as a typed
   array lowers through `Lowerer::GenerateFastStElemI` →
   `GenerateFastElemICommon` → `GenerateFastElemIIntIndexCommon`
   (lib/Backend/Lower.cpp). The emitted guards are base-type
   (`GenerateArrayTest`), non-negative int index, and an unsigned upper
   bound against the view's `length` field (`Float64Array::GetOffsetOfLength`
   → the effective length, zeroed on detach by
   `TypedArrayBase::ClearLengthAndBufferOnDetach`). The element address is
   the view's raw `TypedArrayBase::buffer` BYTE*
   (`Float64Array::GetOffsetOfBuffer()` at the element-addressing block)
   plus scaled index — written directly, with no immutability test and no
   way for the type check to see the buffer's immutability (same class,
   same vtable, real length).
2. **MemSet/MemCopy loop fusions (hole 2, source-level).**
   `GlobOpt::CollectMemsetStElementI`/`CollectMemcopyStElementI` fuse runs
   of typed-array stores into `OpCode::Memset`/`Memcopy`, whose runtime
   helpers `Js::JavascriptOperators::OP_Memset`/`OP_Memcopy` dispatch to
   `TypedArray::DirectSetItemAtRange` (both overloads,
   lib/Runtime/Library/TypedArray.h:412/:462), which checked detached only.
   Returning FALSE there already bails the JIT'd loop out
   (`BailOutOnMemOpError`, Lower.cpp `LowerMemOp`) and replays the
   per-element stores — the exact contract the cross-site object case uses.

## Baseline (pre-fix) evidence, base `90f65f75f`

Repro shape (audit's, per class, in one IIFE per class so every store site
is monomorphic and profile-fed): four sites — sloppy/strict single
`ta[1]=v` and sloppy/strict 8-element loop — warmed 20 000 iterations on a
mutable view, then fed an immutable-backed view of the same class 1 000
times. Interpreter semantics: sloppy silent no-op (bytes unchanged), strict
TypeError 1000/1000.

| class \ flags | default tier-up | `-mic:1 -bgjit-` | `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-` | `-forceNative -off:simpleJit -bgJitDelay:0` | `-nonative` |
|---|---|---|---|---|---|
| Int8..Float64 (all nine specialized) | **WROTE + 0/1000 strict** | **WROTE + 0/1000** | **WROTE + 0/1000** | clean | clean |
| BigInt64 / BigUint64 / Float16 | clean | clean | clean | clean | clean |

(Single-class spot check also reproduced the audit's verbatim Float64
repro: `view[1] === 9.25`, `0/1000` TypeErrors, default and `-mic:1`.)

Adversarial battery on the same binary (builtins set/fill/copyWithin/sort/
reverse, species, DataView, Atomics, RAB shrink/transfer mid-loop,
valueOf-detach reentrancy, detach-realloc): 3 failures, all the hole
itself firing through warmed store sites (RAB→transferToImmutable fixed
result written; detach-realloc bytes changed); every helper-mediated shape
(set/fill/sort/DataView/Atomics/species/reentrancy) clean, matching the
audit's review.

Hot mutable-store microbench (Float64Array(1024), all in-bounds, JIT
profile): medians of 7 runs `-mic:1 -bgjit-` (5 runs default tier-up):

| shape | `-mic:1 -bgjit-` | default tier-up |
|---|---|---|
| store1 ×30M (one store/call) | 111 ms | 110 ms |
| store8 ×7.5M (8 stores/call, 60M stores) | 33 ms | 33 ms |
| loop8 ×7.5M (induction loop, 8 stores/iter) | 240 ms | 235 ms |

## Guard design (object-layout evidence)

- `IsImmutableBuffer(ArrayBufferBase*)` (ArrayBuffer.h) is a virtual
  `IsArrayBuffer()` filter plus `ArrayBuffer::IsImmutable()`, which read
  `Field(bool) isImmutable` — a field of `ArrayBuffer` (after
  `isResizable`), NOT of the shared `ArrayBufferBase` header.
  `SharedArrayBuffer` subclasses `ArrayBufferBase` directly and is a
  SMALLER object (it only adds `sharedContents`), so a JIT load at
  `offsetof(ArrayBuffer, isImmutable)` through a view's `ArrayBufferBase*`
  would be a type-confused, potentially out-of-allocation read for
  SAB-backed views. A typeId-guarded load chain (obj → type → typeId →
  field) would be type-safe but costs three extra dependent loads on every
  hot mutable store.
- Fix: the field moves into the shared `ArrayBufferBase` header next to
  `isDetached`/`externalized`/`infoBits`, initialized false in the single
  `ArrayBufferBase` ctor, set true only by `ArrayBuffer::SetImmutable()`
  (transferToImmutable/sliceToImmutable destinations, before any view
  exists). SharedArrayBuffer and every mutable buffer keep it false
  forever, so "byte at `ArrayBufferBase::GetIsImmutableOffset()` is
  nonzero" is exactly `IsImmutableBuffer()` — one load, no type check.
  This mirrors the existing `GetIsDetachedOffset()` JIT pattern
  (IRBuilderAsmJs.cpp:1123 loads it as TyInt8).
- The JIT guard lives in `GenerateFastElemIIntIndexCommon`'s typed-array
  branch, stores only (reads of immutable views are legal and stay on the
  fast path):
  ```asm
  MOV  arrayBuffer, [array + ArrayBufferParent::GetOffsetOfArrayBuffer()]
  CMPB [arrayBuffer + ArrayBufferBase::GetIsImmutableOffset()], 0
  JNE  $cantUseArray            ; helper: OP_SetElementI exact semantics
  ```
  The branch target is the same label `GenerateArrayTest` uses, so an
  immutable view executes the store through the SetElementI helper chain:
  sloppy silent no-op, strict TypeError — identical to the interpreter.
  Loads (`GenerateFastElemI`) are untouched; string/symbol indices never
  take the typed-array fast path (`GenerateFastElemICommon` returns
  nullptr).
- Hole 2 closes by adding the same `IsImmutableBuffer(GetArrayBuffer())`
  early-out to both `DirectSetItemAtRange` overloads (destination only for
  the memcopy overload — reading an immutable source is legal), returning
  FALSE so the caller bails out and replays the per-element stores.

## Memory-management statement (doc/memory-management.md)

This is a pure guard/branch change: **no new allocation, ownership,
retained pointer, GC-visible state, callback, thread or context**. The one
representation change moves an existing GC-inert `Field(bool)` from
`ArrayBuffer` to its base `ArrayBufferBase` (offsets are engine-internal,
recomputed by `offsetof`; TTD/serialization never carried the bit — the S8
residual is unchanged). Immutable buffers remain ordinary recycler
`JavascriptArrayBuffer` allocations under the S8 ownership record.

## Post-fix results (same probes, fixed `out/jit` build)

Repro matrix — 12 classes × sloppy/strict × single/loop × five flag sets,
all runs byte-identical to the interpreter:

| class \ flags | default tier-up | `-mic:1 -bgjit-` | interpreted-variant | forceNative | `-nonative` |
|---|---|---|---|---|---|
| Int8..Float64 (nine specialized) | clean | clean | clean | clean | clean |
| BigInt64 / BigUint64 / Float16 | clean | clean | clean | clean | clean |

"Sloppy no-op + strict TypeError 1000/1000, bytes unchanged" in every
mode; pre-fix the three profile-fed modes wrote all nine specialized
classes and skipped the TypeErrors (0/1000), matching the audit.

Adversarial battery (builtins set/fill/copyWithin/sort/reverse, generic
fill no-op range, species reads, DataView, Atomics with
`-ESSharedArrayBuffer`, RAB shrink/transfer mid-loop, valueOf-detach
reentrancy, detach-realloc): **BATTERY CLEAN** in default, `-mic:1
-bgjit-`, interpreted-variant, forceNative, `-nonative` and the SAB flag
run. (Two battery expectations were corrected during the run and are
recorded here so they are not rediscovered: `transferToImmutable` of a
shrunk RAB legitimately copies the earlier in-bounds mutable write, and
`map` on an immutable SOURCE is a legal read producing a fresh mutable
result — only immutable species/create destinations reject.)

Checked-in regression `test/es7/immutable-store-jit.js` (registered in
`test/es7/rlexe.xml`, no flags, no baseline): prints `pass` and exits 0
in all five modes above, plus dynapogo (suite default variant).

Hot mutable-store microbench (same binary family, medians; before = clean
base build, after = fixed build):

| shape | before (`-mic:1 -bgjit-`) | after | before (default) | after (default) |
|---|---|---|---|---|
| store1 ×30M (one store/call) | 111 ms | 110 ms | 110 ms | 110 ms |
| store8 ×7.5M (8 stores/call, 60M stores) | 33 ms | 43 ms | 33 ms | 43 ms |
| loop8 ×7.5M (induction loop, 8 stores/iter) | 240 ms | 236 ms | 235 ms | 235 ms |

Guard cost: **zero** within noise on call-bound and loop-bound shapes
(the audit's repro shapes); **+0.17 ns/store (~0.5 cycle, +30%)** only on
the store-saturated unrolled shape where eight guarded sites execute per
call body — the price of the second load (buffer object header) plus
test/branch per store site. A cheaper alternative — caching the immutable
bit on `TypedArrayBase` — was considered and rejected: the raw
`TypedArrayBase::buffer` pointer is assigned outside the primary ctor
(`TypedArray.h`'s `TypedArray(DynamicType*)` marshaling shape, and
`ClearLengthAndBufferOnDetach`), so a view-cached bit would duplicate
buffer state with a staleness class across those sites; the direct
single-byte read of the ArrayBufferBase header has no such coherence
obligation (see the memory statement below).

Gates:

| gate | result |
|---|---|
| `zig build -Dprofile=jit check` | green (check_jit/check_smoke PASS) |
| `zig build check` (test profile, no-JIT) | **2036/0** (was 2035/0; +1 = the new rlexe entry; suite 43.6 s) |
| `zig build -Dprofile=debug-static check` | **2088/0** (was 2087/0; suite 181.6 s; the new test costs ~5.6 s on the debug binary) |
| `zig build check-build-tools` | 62/62 OK (gate_counts consumers) |
| test262 `s8-resizable` (incl. the immutable/sliceToImmutable/transferToImmutable corpora) × {nojit, interp, fulljit} | 804/30 of 834 in every mode, **0 diffs vs nojit** |
| test262 `s8-float16` × {nojit, interp, fulljit} | 4966/492 of 5458 in every mode, **0 diffs vs nojit** |

## Residuals

- The immutable-bit byte lives in the `ArrayBufferBase` header; a mutable
  store now touches the buffer object's cache line per store site. The
  measured cost is quantified above; if a future hot kernel regresses,
  the view-cached-bit design sketch (and its coherence hazards) is
  recorded here as the starting point.
- x64/Windows backends share `Lower.cpp` and get the same guard by
  construction, but only ARM64 macOS was executed (B4 owns Linux and the
  Windows proof).
- The S8 residuals (TTD snapshots drop the immutable bit, no OS page
  protection, `return-immutable.js` host harness) are unchanged.


### Review outcome (dual ACCEPT, observations resolved)

Both reviewers ACCEPTed with zero `[bug]` findings. The general review
verified the layout move end-to-end (SAB type-confusion rationale
confirmed; `SetImmutable` has exactly two call sites, both on fresh
allocations before views exist; old views of a `transferToImmutable`
source see a DETACHED buffer, so the immutable bit never transitions on
a view-visible buffer — no 0→1 race), the guard funnel/label semantics
(`labelCantUseArray == labelHelper` for typed arrays → exact
`OP_SetElementI` replay), hole-2 upfront checks (zero partial writes),
and every quantitative claim. The adversarial review hunted layout
consumers (SCA serialization copies bytes only; TTD snapshots bytes
only; no offsetof/static_assert consumers), proved SAB-backed views
type-safe through warmed sites, verified freeze-race/reentrancy/hole-2
exactly-once behavior across modes, confirmed fusion is load-bearing
via a 7x timing discriminator, and ran perf beyond store8 (invisible on
load-dominated/non-TA workloads).

Folded notes: (1) pre-existing, residual — the store helper checks
immutability BEFORE ToNumeric conversion of the value (spec converts
first; tier-identical, out of this lane); (2) pre-existing, residual —
sloppy stores to DETACHED typed arrays throw TypeError in this engine
where spec says no-op (tier-identical; do not conflate with
sloppy-immutable no-op); (3) the adversarial review's four ad-hoc
dynapogo failures in `matchall.js`/`s6-growable-nonshared.js` were
RESOLVED as invocation artifacts: both tests require their per-entry
rlexe flags (e.g. `-ES6RegExSymbols`), reproduce identically on the
no-JIT binary without them, and the authoritative es7 dynapogo leg on
the fixed binary is **99/0**. Operational gotcha recorded: runtests.py
needs an ABSOLUTE `--binary` path (it chdirs per test); a relative path
fails every test at spawn.
