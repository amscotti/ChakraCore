# C2-S2 sub-batch: BigInt64Array and BigUint64Array elements

Status: **pending review** (implemented on `s2-bigint64`, base `7a2199595`);
Mac ARM64 no-JIT evidence only. `ESBigInt` is still off by default and is
required to expose these constructors; BigInt arithmetic and S8 typed-array
tracking are otherwise unchanged.

This slice adds the typed-array element family that was left out of
[c2-s2-bigint](c2-s2-bigint.md) / [c2-s2-bigint-arithmetic](c2-s2-bigint-arithmetic.md)
and [c2-s8-typedarray-tracking](c2-s8-typedarray-tracking.md). Map/Set BigInt
key identity and BigInt literal property names remain out of scope.

## What landed

1. **Constructors and prototypes.** `BigInt64Array` and `BigUint64Array`
   are real constructors with `length = 3`, `name`, `BYTES_PER_ELEMENT = 8`,
   `%TypedArray%` inheritance, `prototype.constructor`, `@@species` from the
   shared constructor initializer, subclassing, `Reflect.construct`, `.from`
   and `.of`. They are created only when `IsESBigIntEnabled()` is true and are
   not present in the default configuration.
2. **Storage.** Elements reuse the existing internal `Int64Array` /
   `Uint64Array` slots (`TypeIds_Int64Array = 53`,
   `TypeIds_Uint64Array = 54`, 8-byte element size, little-endian byte
   storage). `TypedArray<int64>` / `TypedArray<uint64>` hold
   two's-complement bit patterns; the type ids are already inside the
   `TypedArrayBase` range that ArrayBuffer detach, species, subarray, slicing
   and resizable-view tracking understand.
3. **Conversion.** Reads allocate a Recycler `JavascriptBigInt` directly from
   the 64-bit element (`CreateFromInt64` / `CreateFromUInt64`), never through
   a double, so `INT64_MIN` and values above 2^53 are exact. Stores call
   `JavascriptConversion::ToBigInt` once (observable coercion and its throws),
   take the low 64 two's-complement bits (`ToUInt64Low`) and store them.
   Number, `null`, `undefined` and Symbol stores throw `TypeError`; invalid
   BigInt strings throw `SyntaxError`. `BigInt.asIntN(64, x)` and
   `BigInt.asUintN(64, x)` are the reference semantics for signed/unsigned
   truncation.
4. **Methods.** `set`, `subarray`, `slice`, `map`, `filter`, `sort`
   (numeric default order and a BigInt-valued user comparator),
   `toSorted`, `toReversed`, `with`, `fill` (converted once for the typed
   entry point), `copyWithin`, `reverse`, `forEach`/`every`/`some`/
   `find*`/`reduce*`, `join`, `toLocaleString`, `at`, `includes`,
   `indexOf`, `lastIndexOf`, iteration (`values`/`keys`/`entries`) and
   descriptors/ownKeys all operate through the existing direct element
   accessors. `SameValueCommon` now compares BigInt primitives by value, so
   `includes` works on a value read back from the array (this also makes
   `Object.is(1n, 1n)` true; Map/Set identity is unchanged).
5. **Resizable views.** Auto-length views (`new BigInt64Array(rab)`) follow
   grow/shrink and partial trailing elements; explicit-length views become
   length zero while OOB, report byteOffset zero, and recover length/offset
   when the buffer grows back. Bytes removed by a shrink read as zero on
   recovery, matching the existing typed-array behavior.

## Storage, ownership and reentrancy

```text
Resource / creation: existing ArrayBuffer byte storage and ArrayBufferParent
  fields; element reads create Recycler JavascriptBigInt results.
Domain/allocator: the owning ScriptContext Recycler for BigInt values; the
  ArrayBuffer's RefCountedBuffer/finalizer path for element bytes. No new
  native allocation, root, finalizer or release authority is added.
Physical base/extent: buffer points at arrayBuffer->GetBuffer() + byteOffset;
  the view keeps only byteOffset, captured length and lengthTracking. Grow
  and shrink do not remap the stable capacity.
Logical length/views: GetLength/IsOutOfBounds derive the live range from the
  current buffer byte length and 8-byte element size; subarray keeps the
  auto/fixed character, slice is a fixed copy.
Invalidation: existing ArrayBuffer::Detach ->
  ClearLengthAndBufferOnDetach clears length/buffer for these type ids;
  fixed OOB views keep their stored offset for recovery.
Reentrancy: ToBigInt can run user code; stores perform it before the
  detached/OOB check. `with` and typed `fill` convert once before their
  copies; sort's comparator receives fresh BigInts and may detach, with the
  existing post-callback revalidation. No new user-code point was added to
  reads.
Thread/context: unchanged script-thread access, no async or cross-thread
  state.
Tests: test/es7/bigint64array.js (constructors, boundaries, methods,
  species/subclassing, resizable views, descriptors, no-truncation probes).
```

## Tests

New `test/es7/bigint64array.js`, registered once in `test/es7/rlexe.xml`
with `-ESBigInt -CollectGarbage -args summary -endargs`, covers:

- constructor shape, descriptors, `%TypedArray%` and prototype chains,
  no-argument/length/ArrayBuffer/offset construction, unaligned/excessive
  offsets, `Reflect.construct` with a custom prototype, subclass instances;
- `INT64_MIN`/`INT64_MAX`/`UINT64_MAX`, `2^63`, `2^64`, negative wrap,
  string/boolean/object/`@@toPrimitive` conversion, Number/null/undefined/
  Symbol `TypeError`, malformed-string `SyntaxError`, and exact low-bit
  agreement with `BigInt.asIntN`/`asUintN` for large values;
- `set` (array, BigInt typed arrays, overlap), `subarray`, `slice`, `map`,
  `filter`, `@@species`, `from`/`of`, signed/unsigned `sort`, custom sort
  callbacks receiving BigInts, `fill` conversion-once, `with`,
  `copyWithin`, `reverse`, iteration, search, reduce/find and
  `toLocaleString`;
- auto/fixed resizable-buffer grow, shrink, OOB, byteOffset and recovery;
- index descriptors, `Object.keys`/`Reflect.ownKeys`, `in`, delete,
  `defineProperty` with BigInt versus Number, OOB no-op stores and large
  value truncation.

## Evidence

Mac ARM64, no-JIT test profile (`-nonative` for test262), shared gate lock,
local `.zig-cache` / `out`; test262 checkout
`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, 18 controls passed for every run.
Before and after binaries were measured with the same host flags as S2:
`-nonative -Test262 -CollectGarbage -ESBigInt`.

- `zig build --cache-dir .zig-cache --summary line`: **567/567** steps.
- `zig build -Dprofile=debug-static --cache-dir .zig-cache --summary line`:
  **545/545** steps.
- Local `test/es7/bigint64array.js`: **pass** on both `zig-out/bin/ch` and
  `out/debug-static/bin/ch`; `test/es7/bigint-{basics,asintn,literals,mixed,arithmetic}.js`,
  `test/es7/resizable-typedarray.js`, `test/es7/resizable-arraybuffer.js`,
  `test/es6/ES6TypedArrayExtensions.js` (registered ES6/SAB flags) and
  `test/es6/ES6ArrayAPI.js` also pass; `test/BigInt/*.js` all pass.
- `TypedArrayConstructors/BigInt64Array/**` + `BigUint64Array/**` +
  `TypedArray/**/BigInt/**` + `TypedArray/**/*BigInt*`: **1016 scenarios /
  512 files**, before **14 passed / 1002 failed**, after **958 passed /
  58 failed** (`out/test262-s2b-before`, `out/test262-s2b-after2`):
  **944 fail→pass / 0 pass→fail**, identical result keys.
  - `TypedArrayConstructors/BigInt64Array`: **0/24 → 24/24**;
    `BigUint64Array`: **0/24 → 24/24**.
  - The extra `ctors-bigint/**` corpus (113 files) is **0/226 → 166/226**,
    **166 fail→pass / 0 pass→fail**; with `-ESSharedArrayBuffer` it is
    **214/226**, leaving only the 12 cross-realm prototype scenarios
    (`proto-from-ctor-realm*.js`, including the SAB variant), which fail
    identically for non-BigInt constructors on both binaries.
- Supplementary same-binary run with `-ESSharedArrayBuffer`: the main
  corpus is **964/1016**, so the six SAB scenarios moved to pass and the
  remaining 52 are the two inherited classes below.
- SHA-256: test launcher `d6f46c5af6dbe36fd97d01cbb04217eec2ec9aeb88a81c34c287ae6cc8a3a3dd`,
  engine `8b609b2e2b490fa2cef42f53c32b27cd43108f01eb28527511a3586fdc5d3269`,
  debug-static launcher `88dcd225dbf4869885e7c420d125b05babf449486785695090cb0f4b3f177823`.
- `git diff --check` clean.

## Residuals and classification

- **Detached-buffer callback semantics (34 main-corpus scenarios).** Methods
  such as `every`/`forEach`/`find*`/`reduce*`/`some`/`filter` call
  `DirectGetItem` after a callback detaches the buffer and surface the
  engine's `TypeError`; the spec-visible result is an undefined element.
  The equivalent non-BigInt tests (`every/callbackfn-detachbuffer.js`) fail
  on **both** the base and after binaries with the same message, so this is
  inherited typed-array behavior, not a BigInt regression.
- **Per-element `toLocaleString` overrides (18 scenarios).** The distinct
  `%TypedArray%.prototype.toLocaleString` entry point validates the receiver
  but formats primitives directly; it does not invoke an overridden
  `BigInt.prototype.toLocaleString`. The same gap is recorded as an S8
  residual and the non-BigInt analog fails identically before/after.
- **SharedArrayBuffer configuration (6 main + 50 ctors scenarios).** S2's
  host flags do not enable `ESSharedArrayBuffer`; enabling it makes these
  pass. The BigInt element storage itself works over SAB.
- **Cross-realm constructor prototype (12 ctors scenarios).** Custom
  `newTarget` prototypes are not preserved from another realm for any typed
  array; both binaries fail the non-BigInt analog. No pass→fail is
  introduced.
- **Atomics BigInt operations** (`Atomics.add/sub/...` on BigInt64Array)
  are not part of this slice; non-BigInt Atomics paths are unchanged.
- **JIT/B4.** No JIT lowering, inline cache, bailout or native frame change
  is included or claimed. Mac no-JIT only; B4 remains blocked on the native
  Linux behavior gate. `ESBigInt` remains off by default, so default-profile
  behavior does not expose the constructors.
- **S8/JIT note.** The JIT's cached typed-array length path still excludes
  these type ids (`TryGetLengthForOptimizedTypedArray`); generic interpreter
  paths are used until B4 work covers both resizable tracking and BigInt
  element types.
