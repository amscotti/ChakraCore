//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8 remainder: ES2025 Float16Array

Status: landed on the lane branch (Mac no-JIT; JIT lowering gated at B4).
This is the separable `Float16Array` constructor / typed-array element-kind
slice named by the S8 triage: `DataView.prototype.getFloat16/setFloat16` and
`Math.f16round` already landed with a portable binary64<->binary16 converter
([c2-s8-float16.md](c2-s8-float16.md)). That converter is reused here; no
second codec was written.

## Scope

1. New typed-array element kind (2-byte elements): `Float16Array` constructor
   with all overloads (length, TypedArray source of any kind, iterable,
   array-like, `ArrayBuffer` + byteOffset + length), `Float16Array.prototype`
   chained to `%TypedArray%.prototype`, `BYTES_PER_ELEMENT` (2) on the
   constructor and the prototype, `length` 3 / `name` `"Float16Array"`,
   `@@toStringTag` `"Float16Array"` through the shared
   `%TypedArray%.prototype` getter.
2. Element load/store conversion: store runs `ToNumber` then
   round-to-nearest-ties-even to binary16 (subnormals, overflow to Infinity,
   NaN canonicalization with sign preserved); load widens exactly.
3. Generic `%TypedArray%` behavior works through kind dispatch with no
   per-method special cases: `set`/`subarray`/`slice`/`map`/`filter`/`reduce`/
   `sort` (numeric default, NaN last), `copyWithin`/`fill`, `indexOf`/`includes`
   (SameValueZero on widened values), `join`/`toLocaleString`,
   `entries`/`keys`/`values`, `at`/`with`/`toSorted`/`toReversed`, `from`/`of`,
   species, resizable-buffer length tracking, detach semantics.
4. Exclusions per spec: `Atomics` rejects `Float16Array` (allowlist of integer
   kinds, unchanged); `DataView` already handles float16 separately.

## Design

Storage is a distinct trivially-copyable struct `Float16Bits { uint16 bits; }`
(`lib/Runtime/Library/Float16.h`), so `typedef TypedArray<Float16Bits>
Float16Array` gets its own instantiation, `TypeIds_Float16Array`,
`DynamicType`, vtables and conversion semantics instead of aliasing
`Uint16Array`. The buffer holds raw patterns:

- **Store** (`ToFloat16Element`, file-local in `TypedArray.cpp`, mirrored by
  `Float16ElementStore` in `JavascriptOperators.cpp` for the JIT-only
  memmove/memset helpers): observable `ToNumber` first (user code may run and
  may detach — the conversion precedes the detached check, matching the other
  kinds), then `Float16::ToBits`. NaN encodes as quiet NaN `0x7E00` with the
  input sign, exactly like the `DataView.setFloat16` path.
- **Load** (`Float16ElementAt`): `Float16::ToDouble` then
  `JavascriptNumber::ToVarWithCheck`, the same result shaping as `Float32Array`
  loads.
- **Sort** (`TypedArrayCompareElementsHelper<Float16Bits>`): widens both
  sides exactly, keeps NaN-to-end and the `-0`/`+0` shared treatment, and
  passes widened Numbers to a user comparefn.
- **Same-size memmove exclusion**: `TypedArrayBase::Set` memmoves between
  same-size kinds except float/clamped mixes; `Float16Array` is excluded on
  both sides so `Uint16Array`/`Int16Array` sources convert element-wise
  (a bitwise copy would misread integer values as binary16 patterns).
  Same-type `Float16Array`->`Float16Array` still memmoves (bit-identical).
- **No virtual-array variant**: the constructor never installs a virtual
  vtable (unlike the 8 classic kinds), mirroring `Int64Array`/`Uint64Array`;
  detach only records, and there is no `Float16VirtualArray`/`MixedArray`
  `ObjectType`.

`TypeIds_Float16Array` is inserted immediately after `TypeIds_Float64Array`
(all later ids shift by one; `TypeIds_Limit`-sized tables adjust). This keeps
the `JsArrayTypeX - TypeIds_X == JsArrayTypeInt8 - TypeIds_Int8Array`
arithmetic in `Jsrt.cpp` intact with `JsArrayTypeFloat16` appended after
`JsArrayTypeFloat64`, so `JsGetTypedArrayInfo` needs no special case.
Optimized-JIT predicates (`<= TypeIds_Float64Array`, `ObjectType` virtual/
mixed tables, merge maps) intentionally exclude the new kind: the JIT
backend is not compiled on the Mac no-JIT verification configuration, and
JIT lowering for the new kind is B4 work.

## Files changed

- `lib/Runtime/Types/TypeId.h`: insert `TypeIds_Float16Array` after
  `TypeIds_Float64Array`; renumber followers. `TypedArraySCAMax` stays
  `Float64Array` (SCA has its own id space).
- `lib/Runtime/Library/Float16.h`: add `Float16Bits` storage struct
  (`== int` / explicit `operator int` support the memset fast path).
- `lib/Runtime/Library/TypedArray.h`: include `Float16.h`,
  `typedef TypedArray<Float16Bits> Float16Array`, comparator declaration,
  explicit-instantiation entry.
- `lib/Runtime/Library/TypedArray.cpp`: `VarIsImpl`, `DirectSetItem` /
  `DirectSetItemNoSet` / `DirectGetItem` / `DirectGetItemNoDetachCheck`
  specializations, `DummyVirtualFunctionToHinderLinkerICF`,
  `INSTANTIATE_BUILT_IN_ENTRYPOINTS`, comparator definition, `ToFloat16Element`
  / `Float16ElementAt`, `NOTSUPPORTEDSTUB(Float16Array, ...)` for all nine
  Atomics entry points, `GetDefaultConstructor`, `@@toStringTag`
  (`"Float16Array"` dynamic string like `BigInt64Array`),
  `CreateSameType`, `FindMinOrMax` (widened loop), memmove exclusion.
- `lib/Runtime/Library/JavascriptLibraryBase.h`: `Float16ArrayConstructor` /
  `Float16ArrayPrototype` fields + constructor getter.
- `lib/Runtime/Library/JavascriptLibrary.h`: `float16ArrayType` field,
  `GetTypedArrayType<Float16Bits>` specialization, `STANDARD_INIT(Float16Array)`,
  `Library/Float16.h` include.
- `lib/Runtime/Library/JavascriptLibrary.cpp`: prototype chained to
  `%TypedArray%.prototype`, type init, constructor installed on the global
  under the dynamic name `"Float16Array"` (no static `PropertyIds` entry,
  like `BigInt64Array`), `INIT_TYPEDARRAY_CONSTRUCTOR/PROTOTYPE` expansions,
  vtable recording, `IsTypedArrayConstructor`.
- `lib/Runtime/Library/JavascriptBuiltInFunctionList.h`:
  `BUILTIN_TEMPLATE(Float16Array, NewInstance, ...)` (defines the static
  `FunctionInfo`; no `Set` entry, same as the other float kinds).
- `lib/Runtime/Library/ArrayBuffer.cpp`: detach case (no virtual variant).
- `lib/Runtime/Language/JavascriptOperators.cpp`: `GetElementIIntIndex` and
  `OP_SetElementI` cases (+ `isTypedArray`), `OP_Memcopy` same-type bitwise
  case, `OP_Memset` conversion case via file-local `Float16ElementStore`.
- `lib/Common/BackendApi.h`: `VtableFloat16Array`.
- `lib/Runtime/Language/ValueTypes.h` / `ValueType.cpp`: non-optimized
  `OBJECT_TYPE(Float16Array)` + `TypeIdToBits` mapping (no virtual/mixed
  entries, like the BigInt kinds).
- `lib/Runtime/Library/JavascriptArray.cpp`: explicit
  `TypedArraySort<Float16Bits>` instantiation.
- `lib/Jsrt/ChakraCommon.h`: append `JsArrayTypeFloat16` (existing values
  untouched).
- `lib/Jsrt/Jsrt.cpp`: `JsCreateTypedArray`, `C_ASSERT`, element-size,
  `GetObjectArrayData` + `TypedArrayTypeTraits<Float16Bits>`,
  `JsSetIndexedPropertiesToExternalData` cases.
- `lib/Jsrt/JsrtDebugUtils.cpp`: object-model switch + type-name string.
- `lib/Runtime/Debug/TTSnapObjects.cpp`: snapshot-replay creation.
- `lib/SCACore/SCATypes.h` (`SCA_Float16Array = 71`, `IsSCATypedArray`),
  `SCASerialization.h` (`TypedArrayTrace<Float16Bits>`),
  `SCASerialization.cpp` (`IsTypedArray` + writer case),
  `SCADeserialization.cpp` (reader case). Raw patterns serialize as-is;
  old binaries reject the new id through the existing unknown-type path.
- `lib/Runtime/Base/ScriptContext.cpp`: diagnostic type-count line.
- `test/es7/float16array.js` (new, registered in `test/es7/rlexe.xml`):
  17 tests; `tools/build/gate_counts.json` test/debug pins 2012/2064 ->
  2013/2065.
- 66 `test/DebuggerCommon|Debugger/*.dbg.baseline` files: the new global
  appears in debugger snapshots of the global object; promoted
  machine-regenerated `.rebase` output after verifying every hunk only adds
  `Float16Array` entries (CRLF preserved per `.gitattributes`).

`AtomicsObject.cpp` is unchanged by design: `ValidateSharedIntegerTypedArray`
allow-lists the six integer kinds, so `Float16Array` throws `TypeError`.

## Tests

New `test/es7/float16array.js` (UnitTestFramework, `-args summary -endargs`):

- constructor identity/descriptors (`length` 3, `name`, `BYTES_PER_ELEMENT`
  non-writable/non-enumerable/non-configurable on both objects, prototype
  chain, `@@species`, no-`new` throws), `@@toStringTag`;
- length/TypedArray/iterable/array-like/buffer overloads, offset/length
  `RangeError`s, detached-buffer `TypeError`;
- store conversion spot checks (exact values, ties-to-even at 2049/2051 and
  in the subnormal range, overflow/underflow ties, signed NaN), load/store
  exhaustive 65,536-pattern round trip against an independent arithmetic
  decoder, agreement with `DataView.setFloat16`/`getFloat16`;
- 2-byte cross-kind conversion (`Uint16Array`/`Int16Array` sources and
  `set()` convert element-wise, verified against Node bit patterns);
- `set`/`subarray`/`slice`, species subclassing, iteration/search
  (`includes` finds NaN, `indexOf` does not), `map`/`filter`/`reduce`,
  default and comparefn `sort` (comparefn receives Numbers),
  `fill`/`copyWithin`/`reverse`/`at`/`with`/`toSorted`/`toReversed`,
  `from`/`of`, detach/resizable interplay, index-exotic semantics.
  Index-descriptor `configurable` is asserted by consistency with
  `Float32Array` (false engine-wide) rather than the newer spec text, with
  a comment; constructor own-key order likewise matches `Float32Array`.

Focused runs: `ch test/es7/float16array.js` 17/17 pass; also pass under
`-forcedeferparse` and `-nodeferparse`.

Every conversion expectation was mirrored against Node v26.5.0 first via a
differential harness (bits printed per element): constructor overloads,
descriptors, methods, edge cases all byte-identical except two engine-wide
conventions shared with `Float32Array` (constructor own-key order,
index-descriptor `configurable:false`) and the sort-comparefn call sequence
(implementation-defined; sorted results identical).

## Gates actually observed

- `mise x -- zig build check`: **2013 passed / 0 failed** (includes the new
  file; 66 debugger baselines promoted as described above).
- `mise x -- zig build check-regression -Dprofile=debug-static`: **2065
  passed / 0 failed**.
- `python3 tools/build/check_regression.py --self-test`: negative controls
  behave (part of the gate tooling).
- `git diff --check` clean; bytecode `verify_bytecode_pure` reports
  "source headers unchanged; regen exit 0" — no bytecode regeneration was
  required (no new opcodes, no JS builtin changes).
- Focused test262 corpora, same runner flags, new binary vs the pre-wave
  binary `/Users/ascotti/Documents/code/ChakraCore/zig-out/bin/ch`:
  - `test/built-ins/TypedArrayConstructors/**`: 681/765 both, 0 fail-to-pass,
    0 pass-to-fail. The harness `typedArrayConstructors` list now includes
    `Float16Array` (10 ctors, verified in-engine), so previously-passing
    files now also assert the Float16Array rows.
  - `test/built-ins/TypedArray/**`: 1676/1214 both, 0/0 flips.
  - `test/built-ins/DataView/**`: 882/240 both, 0/0 flips.
  - There is no dedicated `test/built-ins/Float16Array/**` directory in the
    pinned snapshot, so "mostly-fail before" has no per-file baseline; the
    three subset runs above are the applicable before/after evidence.
  - Atomics `non-int32-typedarray-throws` files reference `Float16Array`
    behind a `typeof` guard but require `SharedArrayBuffer`, which this
    configuration does not provide (S6 out of scope); they fail identically
    before/after. The allowlist rejection itself is verified by code
    inspection.

## Residuals / limitations

- **JIT/B4**: no new opcodes and the backend is not compiled on Mac no-JIT,
  but JIT lowering for the new kind (Lowerer type checks, `GlobOpt`
  value-type specialization, backend `InliningDecider` entry) was not added;
  B4 must add and verify it before any JIT-configuration claim. The new
  `OP_Memcopy`/`OP_Memset` cases and `FindMinOrMax` fast path compile but
  are only reachable from JIT'd code, so they are unexercised here.
- **SAB (S6)**: `Atomics` rejection of `Float16Array` cannot be executed
  without `SharedArrayBuffer`; SAB-backed `set` conversions and the three
  `non-int32-typedarray-throws` scenarios remain failing for lack of SAB.
- **Index-descriptor `configurable`**: `false` engine-wide for all typed
  arrays (spec says `true`); pre-existing, shared with `Float32Array`.
- **Constructor own-key order** (`BYTES_PER_ELEMENT` before `prototype`)
  differs from V8 engine-wide; pre-existing convention, kept consistent.
- **`-0` vs `+0` default-sort order** follows the engine's existing
  float-kind treatment (shared with `.sort()`; see `doc/c2-s10.md`).
- **SCA forward compat**: data written with `SCA_Float16Array` (71) is
  rejected by older binaries through the existing unknown-type path.

## Ownership / allocation

No allocation, ownership, retained-pointer, GC-tracing or memory-accounting
changes. Element storage lives in the existing `ArrayBuffer` backing store;
`Float16Array` instances are Recycler-allocated through the same
`TypedArray::Create` path as the other kinds, detached/resize notifications
flow through the unchanged `ArrayBuffer` parent mechanism, and the new code
performs pure arithmetic plus the established `ToNumber` conversion (whose
reentrancy contract — convert first, then detached/length checks — matches
the other float kinds). Structured-clone support copies raw bytes through
the existing `WriteTypedArray`/`ReadTypedArray` templates. No new roots,
finalizers, host callbacks or async jobs.
