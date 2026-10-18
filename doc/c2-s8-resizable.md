//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8: resizable ArrayBuffer core and transfer (first bounded slice)

Status: `<pending>` — implemented on the `s8-resizable` lane (Mac ARM64 no-JIT).
This receipt covers the resizable/growable `ArrayBuffer` object and transfer
family only. TypedArray length-tracking element semantics, `Float16Array`,
`BigInt64Array`/`BigUint64Array`, immutable ArrayBuffers and JIT-specific paths
remain out of scope (see Residuals).

## Scope

1. `new ArrayBuffer(length [, options])` with `options.maxByteLength`.
2. `ArrayBuffer.prototype.maxByteLength` and `.resizable`.
3. `ArrayBuffer.prototype.resize(newLength)`.
4. `ArrayBuffer.prototype.transfer([newLength])` and
   `.transferToFixedLength([newLength])`.
5. `ArrayBuffer.prototype.detached` (the `arraybuffer-transfer` detached
   state) reports true after detach/transfer for fixed and resizable buffers.
6. Existing `.byteLength`, `.slice`, DataView/TypedArray access and
   fixed-length buffers keep their current behavior.

All new properties are installed unconditionally. Functions use dynamic
property names (`AddFunctionToLibraryObjectWithPropertyName`, tracked
`maxByteLength`/`resizable` property ids), no new static `PropertyIds` and no
bytecode-header change. The `JavascriptBuiltInFunctionList.h` entries are
appended so existing built-in ids do not shift.

## Design and ownership

Resource / creation call / source location:
`JavascriptArrayBuffer::Create(length, maxByteLength, type)` →
`ArrayBuffer(length, maxByteLength, type, malloc)` (`ArrayBuffer.cpp`) →
`Recycler::RequestExternalMemoryAllocation` / `malloc` / `ZeroMemory`; one
`RefCountedBuffer` owns the byte pointer.

- **Allocation domain / release authority.** The bytes remain an external
  recycler allocation released by `JavascriptArrayBuffer::Finalize` through
  `free` (or the existing virtual-memory path for fixed, non-resizable asm.js
  buffers). `RefCountedBuffer` keeps its existing refcount protocol.
- **Capacity is separate from logical length.** `allocatedByteLength` is the
  physical extent (for resizable buffers, `maxByteLength`; for fixed buffers,
  the length). `maxByteLength` is the immutable resizable limit. `bufferLength`
  is the current `[[ArrayBufferByteLength]]` and the only value resize changes.
- **No remap on resize.** A resizable buffer allocates its full
  `maxByteLength` capacity once. `resize` only changes `bufferLength`, zeroes
  newly exposed bytes on grow and never reallocates, moves or remaps. Existing
  DataView/TypedArray views therefore keep a stable pointer into the same
  allocation until detach (they remain fixed-length; see Residuals).
- **Accounting.** External-memory charges use `allocatedByteLength` for
  allocation, finalization and detach; shrinks do not release capacity. The
  delayed-free helper receives the allocation extent so its conservative
  stack scan covers the whole capacity. `transfer` creates the destination
  first, copies `min(newLength, currentLength)`, and only then detaches and
  frees the source.
- **Detach.** `Detach()` still reports the external-memory release, clears
  `bufferContent`, sets `bufferLength = 0`, and notifies every registered
  DataView/TypedArray parent. `resizable` remains true (the slot is unchanged)
  while `byteLength`/`maxByteLength` read 0. `transfer`/`transferToFixedLength`
  detach through `DetachAndGetState(false)` + `CleanUp()` so the source's
  `RefCountedBuffer` and bytes are released through the same free function as
  finalization, not leaked.
- **User-code / reentrancy points.** Constructor `length` is `ToIndex`, then
  the `maxByteLength` property is read and `ToIndex`'d, then `length >
  maxByteLength` is checked. For model/constructor-newTarget paths the
  `newTarget.prototype` read happens before data allocation, matching spec order.
  `resize` and `transfer` coerce through `ToIntegerOrInfinity`; the buffer is
  rechecked for detachment after the coercion before any buffer pointer is
  touched. No storage pointer is retained across user code.
- **Failure.** Fixed-length buffers and non-ArrayBuffers throw TypeError before
  choosing a size. Negative, non-integer (after truncation) or over-limit values
  throw RangeError. Allocation failure keeps the existing OOM/RangeError path.
  A failed transfer leaves the source attached. A failed resize leaves the old
  length intact.
- **Thread/context.** Unchanged: ArrayBuffer access stays on the script
  thread/context; this slice adds no asynchronous or cross-thread ownership.

## Tests

New `test/es7/resizable-arraybuffer.js` (registered in `test/es7/rlexe.xml`)
covers:

- construction with no options, `maxByteLength` absent/undefined/non-object,
  resizable limits including 0, `maxByteLength < length`, excessive limits,
  options getter/coercion order, RangeError-before-prototype-read ordering and
  data-allocation-after-prototype-read ordering;
- `maxByteLength`/`resizable`/`detached` values, descriptors, brand checks and
  detached behavior on fixed and resizable buffers;
- `resize` grow/shrink/same/zero, fractional truncation, data preservation and
  zero-fill of newly exposed bytes, out-of-range/negative/fixed/detached errors,
  detach during coercion;
- `transfer` and `transferToFixedLength` default/smaller/larger/zero lengths,
  data copy and zero-fill, resizable preservation (transfer) versus fixed
  result (transferToFixedLength), source detachment, coercion and error order;
- fixed-length ArrayBuffer, `slice`, DataView and TypedArray access remain
  unchanged; DataView operations on a shrunken resizable buffer throw TypeError.

## Evidence

Mac ARM64, no-JIT test profile, shared gate lock, local `.zig-cache`/`out`.
The before run used the clean base-commit (`10d44c972`) test-profile artifact
copied to `out/base-bin` (ArrayBuffer/DataView/TypedArray-before sources); the
after run used this lane's `zig-out/bin/ch`.

- `zig build --summary line`: **566/566 steps succeeded**.
- `./zig-out/bin/ch -args summary -endargs test/es7/resizable-arraybuffer.js`:
  **8/8 tests passed**. `test/es7/float16.js` also still passes after the
  DataView out-of-bounds addition.
- Focused test262 run (same patterns, same checkout, 834 scenarios / 417
  files): `test/built-ins/ArrayBuffer/**/*.js`,
  `test/built-ins/DataView/**/*resizable*`,
  `test/built-ins/TypedArray/**/*resizable*`,
  `test/built-ins/TypedArray/**/*out-of-bounds*`.
  **188/834 before → 416/834 after**, **228 fail→pass / 0 pass→fail**.
- ArrayBuffer corpus only: **166/442 → 358/442**, **192 fail→pass,
  0 pass→fail**. Every `maxByteLength` (20), `resizable` (18), `detached`
  (18), `resize` (38), `transfer` (42) and `transferToFixedLength` (42)
  scenario that the base binary could parse now passes; the 84 residue
  scenarios are immutable-arraybuffer (54), SharedArrayBuffer-disabled (18)
  and BigInt-disabled parse cases (8), the inherited `Reflect.construct`
  prototype-order test (2) and cross-realm prototype handling (2; see
  Residuals).
- DataView resizable subset: **0/52 → 36/52**, **36 fail→pass,
  0 pass→fail**; the four known Float16 `resizable-buffer` scenarios
  (get/setFloat16, default + strict) are **4/4 passing**.
- TypedArray resizable/out-of-bounds subset: **22/340 → 22/340**, 0 flips;
  these are the deferred length-tracking cases.
- Feature-tag counts in the same run: `resizable-arraybuffer` 14/516 →
  174/516; `arraybuffer-transfer` 0/118 → 102/118; `immutable-arraybuffer`
  remains 4/58; `Float16Array` 0/4 → 4/4.

## Residuals

- **TypedArray length tracking (out of this slice).** Views created from a
  resizable buffer keep the element length captured at construction. Growing
  the buffer does not extend them; shrinking does not clamp them to zero. They
  can still address the retained capacity (memory-safe because capacity is not
  remapped), but spec `out-of-bounds` get/set/iteration behavior and
  `TypedArray.prototype` length/byteLength tracking are not implemented.
  `test/built-ins/TypedArray/resizable-buffer-length-tracking-1.js` and the
  resizable TypedArray out-of-bounds family remain expected failures.
- **DataView length tracking.** DataView `byteLength` remains fixed at
  construction. DataView element methods now reject operations when the view
  is out of bounds for the current buffer (including the Float16
  `resizable-buffer` scenarios); an omitted-length DataView does not grow or
  shrink its reported `byteLength`.
- **Immutable ArrayBuffers.** `transferToImmutable`, `sliceToImmutable`,
  `ArrayBuffer.prototype.immutable` and the `immutable-arraybuffer` proposal are
  not implemented.
- **Other element types.** `Float16Array` and `BigInt64Array`/`BigUint64Array`
  constructors and element semantics remain later S8/S2 batches.
- **JsRT detach-state round trip.** Detached-state serialization continues to
  reconstruct a fixed-length buffer from the current logical length; it does
  not preserve the resizable/maxByteLength slots. The JS `transfer` family does
  not use that path.
- **Reflect.construct prototype ordering (inherited engine behavior).**
  `Reflect.construct` pre-creates the result object and reads
  `newTarget.prototype` in `JavascriptReflect::EntryConstruct` before invoking
  the constructor. Constructor checks therefore cannot make
  `options-maxbytelength-compared-before-object-creation` observe its
  RangeError before that getter; the constructor-side max check and deferred
  engine-limit check are still ordered against `OrdinaryCreateFromConstructor`
  for normal/subclass construction.
- **JIT/B4.** This receipt is Mac ARM64 no-JIT only. No bytecode or JIT helper
  was added; any JIT exposure (e.g. inline `byteLength` reads) is later B4 work.
