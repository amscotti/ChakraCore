//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8: resizable ArrayBuffer core and transfer (first bounded slice)

Status: **landed in `6ceb23d6d`**; current integrated gates **2012/2064** (lane commits `4bb25b145`, `ef361fe1d`,
`488fded74`, `d2c579543`; integration `f1ed9afce`); Mac ARM64 no-JIT.
Focused resizable corpus **424/834**, **236 fail→pass / 0 pass→fail**;
ArrayBuffer **358/442**; DataView resizable **44/52**; Float16 resizable
**4/4**. TypedArray tracking and immutable buffers remain follow-ups.
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
  allocation until detach. Auto-length DataView views track the current logical
  length; TypedArray views remain fixed-length (see Residuals).
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
  finalization, not leaked. A host-externalized (`JsExternalizeArrayBuffer`)
  buffer uses a no-op byte release in its detached state: the engine releases
  only the `RefCountedBuffer` wrapper and never frees host-owned bytes.
  WebAssembly.Memory buffers are rejected before destination allocation/copy
  because their detached-state creation throws.
- **Transfer coercion order.** `ArrayBufferCopyAndDetach` performs
  `ToIndex(newLength)` before the detached and range checks. `TransferBuffer`
  uses the constructor's engine-cap-free ToIndex helper: negative and
  `>2^53-1` values throw RangeError before detachment; fractional values
  truncate; NaN becomes 0; explicit `undefined` keeps the source length
  default. The detached check follows coercion, so an already-detached buffer
  with a coercible non-negative ToIndex value still ends in TypeError without a
  destination allocation, while the engine `MaxArrayBufferLength` check
  happens after the detached check. `transferToFixedLength` reports its own
  function name in detached errors.
- **`resize` precedence.** `resize` uses ToIntegerOrInfinity (fractional
  truncation, NaN -> 0, no `2^53-1` ToIndex cap), matching Node and the pinned
  test262 behavior. Current ECMA-262 writes ToIndex here; the detached check
  runs after coercion and before the `maxByteLength` range check, so detached
  `resize(-1)` is a TypeError while attached `resize(-1)` is a RangeError.
  This is recorded as Node/test262-compatible behavior, not a claim that the
  existing coercion is spec-identical.
- **User-code / reentrancy points.** Constructor `length` is `ToIndex`, then
  the `maxByteLength` property is read and `ToIndex`'d, then `length >
  maxByteLength` is checked. For model/constructor-newTarget paths the
  `newTarget.prototype` read happens before data allocation, matching spec order.
  `resize` uses its ToIntegerOrInfinity behavior and `transfer` uses ToIndex;
  the buffer is rechecked for detachment after coercion before any buffer
  pointer is touched. `slice` re-reads the source length after start/end/species/constructor
  user code and copies only `min(newLen, currentLength - first)`; it does not
  touch destination bytes beyond that copy (normal destinations are already
  zeroed, species-returned destinations keep their prefilled bytes). DataView
  re-reads the buffer length after `ToIndex(byteLength)` and again after the
  final `OrdinaryCreateFromConstructor` prototype user code, refusing to return
  an out-of-bounds view. The DataView retains the buffer object, not a raw
  storage pointer; resizable capacity is stable and detach notifies all
  parents, so retaining that reference across user code is safe.
- **Failure.** Fixed-length buffers and non-ArrayBuffers throw TypeError before
  choosing a size. Negative, non-integer (after truncation) or over-limit values
  throw RangeError. Allocation failure keeps the existing OOM/RangeError path.
  A failed transfer leaves the source attached and its bytes intact. A failed
  resize leaves the old length intact.
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
  result (transferToFixedLength), source detachment, coercion and error order,
  coercion-before-detached order (throwing and coercible `newLength`, no
  argument, negative/large values), failed-transfer data integrity, max-0
  buffers, source views plus GC, and reentrancy;
- `slice` on a resizable source when start/end/species user code shrinks the
  source: current-length re-read, bounded copy, and a prefilled species
  destination whose bytes beyond the copy range must remain intact;
- DataView constructor revalidation after `ToIndex(byteLength)` and after the
  final `OrdinaryCreateFromConstructor` prototype read (detach/shrink on the
  second read), auto-length `byteLength` tracking across
  grow/shrink/boundary/OOB, fixed-length OOB getter/access TypeErrors, and
  detached getter TypeErrors;
- fixed-length ArrayBuffer, `slice`, DataView and TypedArray access remain
  unchanged; TypedArray views remain fixed-length (deferred tracking).

## Evidence

Mac ARM64, no-JIT test profile, shared gate lock, local `.zig-cache`/`out`.
The before run used the clean base-commit (`10d44c972`) test-profile artifact
copied to `out/base-bin` (ArrayBuffer/DataView/TypedArray-before sources); the
after run used this lane's `zig-out/bin/ch`.

- `zig build --summary line`: **566/566 steps succeeded**.
- `./zig-out/bin/ch -CollectGarbage test/es7/resizable-arraybuffer.js`:
  **12/12 tests passed** (verbose). `test/es7/float16.js` also still passes.
- Focused test262 run (same patterns, same checkout, 834 scenarios / 417
  files): `test/built-ins/ArrayBuffer/**/*.js`,
  `test/built-ins/DataView/**/*resizable*`,
  `test/built-ins/TypedArray/**/*resizable*`,
  `test/built-ins/TypedArray/**/*out-of-bounds*`.
  **188/834 before → 424/834 after**, **236 fail→pass / 0 pass→fail**.
  Against the second-round run (424/834) the final fixes are **0 flips**
  (0 fail→pass, 0 pass→fail): the new transfer/slice/DataView cases are covered
  by the focused JS test and Node differential, not by additional test262
  scenarios in this corpus. The eight DataView getter scenarios added in the
  second round remain green.
- ArrayBuffer corpus only: **166/442 → 358/442**, **192 fail→pass,
  0 pass→fail**. The 84 residue scenarios are immutable-arraybuffer (54),
  SharedArrayBuffer-disabled (18), BigInt-disabled parse cases (8), the
  inherited `Reflect.construct` prototype-order test (2) and cross-realm
  prototype handling (2; see Residuals).
- DataView resizable subset: **0/52 → 44/52**, **44 fail→pass,
  0 pass→fail**. The remaining eight are BigInt DataView files that cannot
  parse without the BigInt flag. The four known Float16 `resizable-buffer`
  scenarios (get/setFloat16, default + strict) are **4/4 passing**.
- TypedArray resizable/out-of-bounds subset: **22/340 → 22/340**, 0 flips;
  these are the deferred length-tracking cases.
- Feature-tag counts in the same run: `resizable-arraybuffer` 14/516 →
  182/516; `arraybuffer-transfer` 0/118 → 102/118; `immutable-arraybuffer`
  remains 4/58; `Float16Array` 0/4 → 4/4.
- Node v26.5.0 differential for the new order cases: detached
  `transfer`/`transferToFixedLength` with `-1` and `2^53` → RangeError,
  detached with `2^32` → TypeError, attached with `-1`/`2^53` → RangeError,
  `NaN` → zero length, `2.9` → 2; prefilled-species slice after a source
  shrink leaves bytes beyond the copied range intact. Chakra rejects an
  attached `2^32` request with RangeError because of its `MaxArrayBufferLength`
  engine cap; Node accepts a 4 GiB fixed buffer. That is an engine-limit
  difference, not a Node-matching claim. The focused JS test encodes the same
  Chakra results.
- Evidence hashes: test launcher `b079eaf1…b148`; test dylib
  `2a302112…c69989` (a launcher hash alone does not identify the engine).

## Residuals

- **TypedArray length tracking (out of this slice).** Views created from a
  resizable buffer keep the element length captured at construction. Growing
  the buffer does not extend them; shrinking does not clamp them to zero. They
  can still address the retained capacity (memory-safe because capacity is not
  remapped), but spec `out-of-bounds` get/set/iteration behavior and
  `TypedArray.prototype` length/byteLength tracking are not implemented.
  `test/built-ins/TypedArray/resizable-buffer-length-tracking-1.js` and the
  resizable TypedArray out-of-bounds family remain expected failures.
- **DataView tracking boundary.** An omitted-length DataView now tracks the
  current buffer byte length for `byteLength`, element access and OOB checks.
  An explicit-length DataView keeps its captured length and becomes fully OOB
  (TypeError on getters/access) if the buffer shrinks past its range. There is
  no separate DataView length-tracking flag exposed to JIT/SCA; JsRT
  `JsCreateDataView` and SCA reconstruction create fixed-length views.
- **Generic DataView setter argument order.** `setInt8`/`setUint8`/multi-byte
  setters still convert `value` before `ToIndex(byteOffset)`; the spec order is
  ToIndex -> ToNumber(value) -> detached/range. Float16 setters already use the
  correct order. Fixing every generated setter was left as a separate residual
  to avoid broadening this patch.
- **Pre-existing JsRT detach ownership leak.** `JsDetachArrayBuffer` calls
  `ArrayBuffer::Detach()` directly, which clears `bufferContent` without
  releasing its `RefCountedBuffer`; it can also discard non-externalized bytes
  without an engine free. This predates the resizable slice and is not changed
  here. Engine `transfer`/`transferToFixedLength` use the detached-state +
  `CleanUp()` path instead. The externalized transfer audit is clean: no-op
  release leaves host-owned bytes alone; the host must own/free them through
  `JsGetArrayBufferFreeFunction` and the detached-state round trip caveat above.
- **Immutable ArrayBuffers.** `transferToImmutable`, `sliceToImmutable`,
  `ArrayBuffer.prototype.immutable` and the `immutable-arraybuffer` proposal are
  not implemented.
- **Other element types.** `Float16Array` and `BigInt64Array`/`BigUint64Array`
  constructors and element semantics remain later S8/S2 batches.
- **JsRT detach-state round trip.** Detached-state serialization continues to
  reconstruct a fixed-length buffer from the current logical length; it does
  not preserve the resizable/maxByteLength slots. The JS `transfer` family does
  not use that path. A host-externalized buffer's bytes are not freed by engine
  detach/transfer (`NoOpFree`), but the host must have obtained the free
  function via `JsGetArrayBufferFreeFunction` and remains responsible for
  releasing the storage; if the detached state is claimed/reconstructed as a
  normal owning buffer, that reconstructed owner would free it.
- **Reflect.construct prototype ordering (inherited engine behavior).**
  `Reflect.construct` pre-creates the result object and reads
  `newTarget.prototype` in `JavascriptReflect::EntryConstruct` before invoking
  the constructor. Constructor checks therefore cannot make
  `options-maxbytelength-compared-before-object-creation` observe its
  RangeError before that getter; the constructor-side max check and deferred
  engine-limit check are still ordered against `OrdinaryCreateFromConstructor`
  for normal/subclass construction. The same inherited pre-read affects
  `DataView/byteOffset-validated-against-initial-buffer-length.js`, which
  expects RangeError before the prototype read; it is outside this focused
  corpus and also fails on the base.
- **Eager capacity.** A resizable buffer allocates the whole `maxByteLength`
  up front, even when `byteLength` is much smaller. This is spec-permitted and
  keeps view pointers stable, but makes `new ArrayBuffer(0, {maxByteLength:
  large})` as expensive as the maximum size; the existing recycler OOM/retry
  path applies.
- **WebAssembly.Memory transfer.** WebAssembly ArrayBuffers reject
  `transfer`/`transferToFixedLength` with `WASMERR_CantDetach` before any
  destination is allocated; their backing memory is not transferable here.
- **JIT/B4.** This receipt is Mac ARM64 no-JIT only. No bytecode or JIT helper
  was added; any JIT exposure (e.g. inline `byteLength` reads or DataView
  auto-length tracking) is later B4 work.
