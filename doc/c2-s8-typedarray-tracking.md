//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt in the project root for license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8: TypedArray views over resizable ArrayBuffers

Status: **pending** — implementation and focused evidence are in this lane;
JIT/B4, TTD and review remain open.

This receipt extends the landed resizable `ArrayBuffer` core
([c2-s8-resizable.md](c2-s8-resizable.md)) with the TypedArray length-tracking
half. The resizable/growable buffer representation, transfer family and DataView
auto-length behavior are unchanged.

## Scope

1. A TypedArray constructed from a resizable `ArrayBuffer` without an explicit
   length is **auto-length**: its element length follows the current
   `[[ArrayBufferByteLength]]`.
2. A TypedArray with a non-undefined explicit length is **fixed**: it keeps the
   captured element length while it fits, becomes length-zero while
   out-of-bounds, and recovers when the buffer grows back over its range.
3. `.length`, `.byteLength` and `.byteOffset` getters report the current
   valid view. Element get/set, `in`, enumeration and `GetOwnPropertyDescriptor`
   use the same current range.
4. OOB views behave like detached views for prototype methods that call
   `ValidateTypedArray`; `set` rejects OOB targets/sources before reading the
   source; iterators throw on a fixed view that becomes OOB mid-iteration.
5. `subarray` preserves the auto/fixed character (an auto subarray with an
   omitted/undefined end stays auto), `slice` remains a fixed copy, and
   `set`/`copyWithin`/`fill`/iteration/species see the current range.

Out of scope, as specified: immutable ArrayBuffers, `Float16Array` and
`BigInt64Array`/`BigUint64Array` element types, SharedArrayBuffer, and JIT.

## Representation and ownership

`TypedArrayBase` now records one non-pointer `Field(bool) lengthTracking`.
`length` remains the construction snapshot used by fixed views (and by the
existing `ArrayObject` layout); it is not rewritten by resize. `GetLength`,
`GetByteLength`, `IsOutOfBounds` and `GetEffectiveByteOffset` derive the live
range from `GetArrayBuffer()->GetByteLength()`, the captured byte offset and
`BYTES_PER_ELEMENT`:

- auto: `floor((bufferByteLength - byteOffset) / elementSize)` when the offset
  fits, otherwise zero;
- fixed: the captured length while the full range fits, otherwise zero;
- byte getter: length times element size;
- offset getter: zero while OOB (the stored `[[ByteOffset]]` is retained for
  subarray and recovery), zero while detached.

Ownership record:

- **Resource / creation:** the existing `ArrayBuffer` allocation
  (`ArrayBuffer.cpp`), created by `JavascriptArrayBuffer`/`CreateResizableArrayBuffer`.
  A resizable buffer allocates its full `maxByteLength` capacity once.
- **Physical base / extent / release:** unchanged. The underlying
  `RefCountedBuffer` bytes are released by the existing finalizer/detach path;
  this slice adds no allocation, root or free authority.
- **Logical length / views:** `bufferLength` is the buffer's logical length;
  each view keeps only `byteOffset`, a construction `length` snapshot and
  `lengthTracking`. A view's `buffer` pointer is the base of the stable
  capacity plus `byteOffset`, so grow/shrink never remaps it.
- **Invalidation:** detach clears the parent buffer pointer and `length` through
  the existing `ArrayBuffer::Detach`/`ClearLengthAndBufferOnDetach` path. Resize does
  not notify parents; all consumers read the current buffer length.
- **Reentrancy:** the TypedArray constructor re-reads the byte length after
  `ToIndex(byteOffset)` and after `ToIndex(length)`, so a getter may grow,
  shrink or detach; `set`, `sort`, the iterator, `with`, `copyWithin`, `map`
  and `slice` recheck detached/OOB state or the current length at their
  user-code boundaries. `copyWithin` uses a per-element fallback when coercion
  changes the live length and treats an OOB/zero-length receiver as a no-op.
- **Thread/context:** unchanged script-thread access; no async or cross-thread
  state was added.

DataView keeps its existing separate `lengthTracking` implementation and was
not modified.

## Tests

New `test/es7/resizable-typedarray.js`, registered once in
`test/es7/rlexe.xml`, covers all supported element types (`Uint8Array`,
`Int8Array`, `Uint8ClampedArray`, `Int16Array`, `Uint16Array`, `Int32Array`,
`Uint32Array`, `Float32Array`, `Float64Array`):

- auto grow/shrink/partial-element/boundary/offset-OOB/recovery for all
  getters and element access;
- fixed grow/shrink/OOB/recovery, explicit-undefined versus explicit-zero
  length, and partial trailing elements at construction;
- integer-indexed get/set, `in`, descriptors and detached buffers;
- TypeError rejection of OOB fixed views across prototype methods and
  iterators; iterators that grow/shrink mid-iteration;
- `set` target/source validation, `subarray` auto propagation and species,
  `slice` fixed copies and post-species OOB revalidation, `fill`, `copyWithin`
  after target/start/end coercion resizes, `with` after ToNumber reentrancy,
  and constructor reentrancy after offset coercion;
- `map` after species-constructor shrink/detach over the captured callback
  range, distinct `%TypedArray%.prototype.toLocaleString` versus generic
  `Array.prototype.toLocaleString.call`, and DataView auto-length tracking.

## Evidence

Mac ARM64, no-JIT test profile, shared gate lock, local `.zig-cache`/`out`;
test262 revision `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 18 controls.
The earlier before run in this lane used Node `v26.5.0`; to remove that runner
difference the base binary was rerun with Node `v22.22.3` (same as this lane's
after run) for every count below.

- `/tmp/opencode/cc-gate-lock.sh zig build --summary line`:
  **567/567 steps succeeded**.
- New es7 test, base binary (`out/base-bin/ch`, SHA-256 `81de4407…`):
  **2/18 passed, 16 failed**. Same source with this lane
  (`zig-out/bin/ch`, launcher `0122769e…`, dylib `d499575d…`):
  **18/18 passed**.
- Adjacent `test/es7/resizable-arraybuffer.js` **12/12** and
  `test/es7/float16.js` **10/10** still pass.
- Focused test262 corpus
  `test/built-ins/TypedArray/**/*resizable*` +
  `test/built-ins/TypedArray/**/*out-of-bounds*`:
  **22/340 before → 132/340 after**, **110 fail→pass / 0 pass→fail**.
  Result keys match one-for-one.
- Added `test/built-ins/TypedArrayConstructors/**/*resizable*` and
  `.../**/*out-of-bounds*`: combined corpus **378 scenarios / 191 files**,
  **32/378 → 150/378**, **118 fail→pass / 0 pass→fail**; the constructors
  slice alone is **10/38 → 18/38**, **8/0**.
- The 208 residual scenarios in the 340 slice are BigInt-gated: 178 hit
  `resizableArrayBufferUtils`'s direct `BigInt64Array` reference and 30 contain
  BigInt literal syntax rejected without `-ESBigInt`. These are not all
  "non-BigInt behavior passed": 178 abort before their later assertions, so
  each file's remaining behavior was not executed.
- The 20 constructors-slice residuals are BigInt-gated (10 scenarios) plus
  pre-existing integer-indexed `Delete`/`Reflect.set` and
  `resized-out-of-bounds-to-in-bounds-index` semantics (10 scenarios), all
  failing on the base binary too; no pass→fail.

## Residuals

- **BigInt corpus.** `BigInt64Array`/`BigUint64Array` are not implemented in
  this build; the focused helper and literal parse failures above are outside
  this slice. Enabling them is separate S2/S8 work.
- **Special element types.** `Float16Array` and the immutable-ArrayBuffer
  proposal remain unimplemented and outside this slice.
- **JIT/B4.** This is Mac ARM64 no-JIT evidence only. The JIT still consults the
  cached TypedArray length offset (`Lower.cpp` `GetOffsetOfLength`), which is
  the construction snapshot for auto views. Before any JIT claim, B4 must either
  deopt/inline the live length calculation for `lengthTracking` views and
  `IsOutOfBounds`, then run the focused test and this es7 test under
  SimpleJIT/FullJIT. No bytecode, PropertyId or JIT helper was changed here.
- **TTD/JsRT.** Snapshots and JsRT reconstruction do not serialize
  `lengthTracking`; reconstructed views are fixed. `TypedArrayBase::GetBuffer`
  reports the current byte length, but no length-tracking flag is exposed to
  embedders.
- **Sort callback resize.** `sort` with a comparison callback still writes the
  captured range after the callback. It is memory-safe because resizable
  capacity is retained, but a callback that resizes the buffer can diverge from
  per-element no-op semantics. The focused corpus does not exercise that path.
- **Integer-indexed Set/Delete residual.** The added constructors corpus exposes
  pre-existing behavior: OOB `Delete`/`Reflect.set` results and
  `resized-out-of-bounds-to-in-bounds-index` (a value coercion that grows the
  buffer before the set validity check). These fail identically on the base
  binary and are not changed by this slice.
- **Slow numeric property path.** The element fast path returns undefined for
  OOB indices; the generic numeric `GetPropertyQuery` path still reports
  not-found (allowing prototype lookup) for an undefined element. The focused
  OOB tests use the fast path and pass; auditing every generic path is follow-up.
