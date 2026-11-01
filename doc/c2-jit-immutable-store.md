//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-JIT: immutable-store fast-path conformance fix (jit-closure-1 holes 1+2)

Status: **in progress** — lane `jit-immutable-store`, base `90f65f75f`.
Baseline evidence below was measured on the clean base build
(`out/jit/bin/ch`, `-Dprofile=jit`, sources at `90f65f75f`).

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

## Post-fix results

(pending)

## Residuals

(pending)
