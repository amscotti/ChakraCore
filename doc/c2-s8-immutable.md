//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8: bounded immutable ArrayBuffer proposal surface

Status: **pending** — implementation and focused Mac ARM64 no-JIT evidence are
in the `s8-immutable` lane on base `0dbfd358a`. Review, merge and B4 (Linux/JIT)
remain open. This is the bounded `immutable-arraybuffer` proposal surface named
by the S8 triage, not the import-bytes proposal and not a claim of full ES2026
conformance.

## Scope

1. `get ArrayBuffer.prototype.immutable` (`name` `"get immutable"`, `length` 0,
   getter only, configurable/non-enumerable).
2. `ArrayBuffer.prototype.transferToImmutable([newLength])` (`length` 0) and
   `ArrayBuffer.prototype.sliceToImmutable(start, end)` (`length` 2, both
   optional).
3. `[[ArrayBufferIsImmutable]]` on ordinary `ArrayBuffer` objects only;
   `SharedArrayBuffer` is rejected by the getter and all three entry points.
4. Copy/detach semantics: `transferToImmutable` copies `min(newLength,
   currentLength)`, zero-fills a larger result, detaches the source and returns
   a fixed-length immutable result. It never consults `constructor`/species.
   `sliceToImmutable` never detaches or mutates the source and copies the
   resolved range.
5. Write protections: `resize`, `transfer`, `transferToFixedLength` and the
   debug `ArrayBuffer.detach` reject an immutable receiver. DataView setters,
   TypedArray write-mode validation/mutators, direct indexed stores and
   TypedArray species/create destinations reject immutable backing buffers.
   Reads, views, `subarray`, `slice` (as a read of the source), constructors
   from views, and non-index properties remain available.

All new properties are installed unconditionally (no config flag), use
Append-only `JavascriptBuiltInFunctionList.h` entries, and use tracked dynamic
property ids. No static `PropertyIds`/`JnDirectFields.h` entries and no
bytecode-header change. `tools/build/gate_counts.json` is deliberately
untouched.

## Design and ownership

Ownership record for the new slice:

- **Representation.** `ArrayBuffer` gains one non-pointer
  `Field(bool) isImmutable` initialized to false in every constructor. This is
  the presence bit for `[[ArrayBufferIsImmutable]]`; `IsImmutableBuffer`
  (`ArrayBuffer.h`) returns true only for a non-shared `ArrayBuffer` with the
  bit set. Existing field offsets for byte length, buffer content and
  `isDetached` are unchanged; the new field is appended after `isResizable`.
- **Allocation domain / release authority.** Immutable buffers use the same
  `JavascriptArrayBuffer` allocation as normal fixed buffers:
  `CreateArrayBuffer(length)` → `ArrayBuffer(length, type, malloc)`, one
  `RefCountedBuffer`, the existing external-memory accounting, finalizer and
  `DetachAndGetState` release path. There is no second allocator, arena,
  external root or ownership transfer. `SetImmutable` is called only after the
  destination exists and before the internal copy; the only writes into an
  immutable data block are `AllocateImmutableArrayBuffer`'s copy in
  `transferToImmutable`/`sliceToImmutable`.
- **Transfer.** `TransferBuffer` keeps its existing order: `ToIndex(newLength)`
  (undefined means source length), detached check, immutable check, engine
  allocation cap, WebAssembly detach-key check, destination allocation, copy,
  then source detach/free. Immutable mode never preserves resizability. The
  destination is allocated and copied before the source is detached, so a
  failed `ToIndex`, allocation or WebAssembly check leaves the source attached
  and unchanged.
- **Slice bounds.** `ResolveArrayBufferBounds` implements the proposal's
  `ResolveBounds`: start is coerced first, end second (undefined end means the
  original length), negative values clamp at zero, positive values clamp at the
  original `len`, and `-∞`/`+∞`/NaN/-0 follow `ToIntegerOrInfinity`. After the
  coercion the receiver is rechecked for detachment, then the current length is
  compared with the resolved end: detached is TypeError and a shrink below the
  end is RangeError. The destination is a fresh immutable buffer, never a view.
- **Invalidation / reentrancy.** The source is re-read through the buffer
  object after user coercion; no raw pointer is retained across it. Detach still
  runs the existing parent notification path. `sliceToImmutable` reads the
  source data only after the final detached/length checks and allocates the
  destination before copying. Source resizing cannot invalidate the copy.
- **Views.** `IsImmutableBuffer` is a type-tag check plus the immutable bit;
  SharedArrayBuffer views always report false. TypedArray's
  `ValidateTypedArray(..., writeAccess)` checks the bit before
  detached/out-of-bounds validation. `TypedArrayCreate(..., writeAccess)` applies
  the same access mode after construction, covering `from`/`of`, map/filter/slice
  species results and filter's post-callback construction. `SetItem`,
  `BaseTypedDirectSetItem` and the BigInt variant return failure before value
  conversion for indexed stores; a strict-mode indexed store raises TypeError
  through `OP_SetElementI`. DataView setters call `GetWritableViewIndex`, which
  checks the bit before `ToIndex(byteOffset)` and value conversion. Atomics
  passes write access only for the read-modify-write/store family; `load` and
  `notify` stay read mode.
- **Failure.** A TypeError is raised for non-object/non-ArrayBuffer receivers,
  SharedArrayBuffer, detached sources, immutable write targets and immutable
  species/create destinations. RangeError is used for invalid ToIndex bounds,
  negative `slice` ranges below zero, and a slice end beyond the current source
  length. A failed operation does not partially mutate an immutable block.
- **Thread/context.** Unchanged: all checks run on the script thread and use the
  existing ScriptContext/library; no new async job, cross-thread pointer or GC
  root is introduced. The field is a GC-inert bool.

## Tests

New `test/es7/immutable-arraybuffer.js`, registered once in
`test/es7/rlexe.xml` with `-CollectGarbage -args summary -endargs`, covers
descriptors/names/lengths/non-constructibility, getter receiver checks,
ordinary/detached/immutable slot results, copy/detach/zero-fill/prefix copy,
resizable sources, species bypass, ToIndex order and errors, slice bounds grow/
shrink/detach cases, slice species rejection, ArrayBuffer slice of an immutable
source, DataView read/write behavior and argument order, TypedArray mutators,
strict/sloppy/Reflect indexed writes, descriptors and `defineProperty`,
subarray/slice/set read paths, TypedArray/`from`/`of` species results, and a
conditional Atomics block. The file passes `test/es7` on both profiles:
**87 passed / 0 failed** in `test` and **87 passed / 0 failed** in
`debug-static`. The added entry is expected to move the gate pins by one:
**2021/2073 → 2022/2074** (`test`/`debug-static`); the pins were not edited.

`test/typedarray/arraybufferType.baseline` now records the three new
`ArrayBuffer.prototype` names in insertion order (`sliceToImmutable`,
`transferToImmutable`, `immutable`); the focused `test/typedarray` folder is
**59 passed / 0 failed** (`test`) and **60 passed / 0 failed** (`debug-static`).

Pinned test262 focused selection (61 files, 122 strict/default scenarios from
`features: [immutable-arraybuffer]` under ArrayBuffer/TypedArray/DataView/
Atomics, excluding the separate `import-bytes` proposal):

| selection | before | after | fail→pass | pass→fail |
|---|---|---|---|---|
| default host flags (`-nonative -Test262 -CollectGarbage`) | 4 / 118 | 84 / 38 | +80 | 0 |
| flag-on (`... -ESSharedArrayBuffer -ESBigInt`) | 4 / 118 | 118 / 122 | +114 | 0 |

Adjacent ArrayBuffer/DataView/TypedArray/TypedArrayConstructors/Atomics trees
(3,360 files, 6,674 scenarios) with the same flag-on host:

| before | after | fail→pass | pass→fail |
|---|---|---|---|
| 5,978 / 696 | 6,088 / 586 | +110 | 0 |

Artifacts (scratch, ignored): `out/test262-s8-immutable-{before,after,
before-flags,after-flags,final-default,final-flags,adj-before,adj-after,
adj-final}`. The two focused flag-on failures are `return-immutable.js`
(host `$DETACHBUFFER` double-detach, see Residuals) and
`Atomics/notify/immutable-buffer-returns-0.js` (BigInt64Array unsupported by
this engine's Atomics surface, see Residuals). The adjacent run has no
pass→fail.

Direct lane smoke checks also verified: strict indexed assignment to an
immutable view throws TypeError, sloppy assignment is a silent no-op,
`Reflect.set` returns false, the index descriptor is non-writable/non-
configurable, and `Object.defineProperty(ta, "0", { value: 1 })` throws
TypeError.

## Residuals and B4

- **Host/JsRT immutability is advisory.** The engine prevents JavaScript writes
  through DataView/TypedArray/Atomics and blocks the JS transfer/resize/detach
  entry points. JsRT still exposes the storage pointer
  (`JsGetArrayBufferStorage`) and `JsDetachArrayBuffer` does not reject an
  immutable buffer, and host APIs that write through a raw storage pointer are
  outside the ECMAScript checks. No OS read-only page protection is claimed.
- **TTD/serialization and debugger snapshots** do not carry the new bool; a
  snapshot replay of an immutable buffer would produce a mutable buffer. This
  slice does not claim TTD fidelity.
- **JIT/B4.** All evidence is Mac ARM64 `-nonative`. The interpreter/helper
  checks are covered; inlined JIT typed-array store fast paths and JIT
  write-barrier behavior were not audited or executed. B4 owns Linux/JIT
  evidence and any JIT fast-path invalidation work.
- **`return-immutable.js` host harness limitation.** The pinned test calls
  `$DETACHBUFFER` on a source that `transferToImmutable` already detached. The
  Chakra test262 host implements `$262.detachArrayBuffer` with
  `WScript.SerializeObject`/`Deserialize`, whose second transfer/
  `JsGetArrayBufferStorage` throws on a detached buffer. The engine behavior is
  correct (the getter returns false on an ordinary detached buffer, verified
  directly); the two failing scenarios are the host helper's non-idempotent
  detach, not a JS semantic failure.
- **BigInt64 Atomics.** `Atomics.notify` on an immutable `Int32Array` returns
  zero and all supported integer RMW/store operations reject immutable
  buffers. `Atomics.notify` with `BigInt64Array` still fails because the
  pre-existing Atomics surface rejects BigInt64Array (the S2/S6 BigInt Atomics
  gap); this slice does not add BigInt wait/notify support.
- **DefineOwnProperty nuance.** Immutable indexed `defineProperty` currently
  returns failure through the typed-array store check; the proposal's
  apply-current-descriptor rule for a no-op `{ value: same }` is not
  implemented. No pinned immutable test exercises it.
- **Uint8Array base64/hex.** `setFromBase64`/`setFromHex` are not implemented
  in this engine. Their two pinned immutable tests currently pass only
  vacuously (calling an absent method throws TypeError and the tests' side-effect
  assertions hold); this slice does not claim those APIs.
- **No zero-copy optimization.** `transferToImmutable`/`sliceToImmutable`
  always copy. The proposal explicitly permits copying.
- Cross-realm species/ArrayBuffer-object behavior was not added to the focused
  lane; Chakra's existing `proto-from-ctor-realm` failures are unchanged in the
  adjacent run. No new cross-realm claim is made.
