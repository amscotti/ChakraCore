# C2-S2 residual: BigInt collection keys, literal property names, DataView accessors

Status: **landed in `10154e3f5`** (feature `b45d224d3`; base `1c983ef9c`); current integrated gates **2012/2064**. All measurements are Mac ARM64 no-JIT with
`-ESBigInt`; `ESBigInt` remains off by default and no JIT claim is made
(see B4).

This slice completes the remaining bounded BigInt surface listed in
[c2-s2-bigint](c2-s2-bigint.md),
[c2-s2-bigint-arithmetic](c2-s2-bigint-arithmetic.md) and
[c2-s2-bigint64array](c2-s2-bigint64array.md): SameValueZero identity for
BigInt keys in `Map`/`Set`, BigInt literal property names, and
`DataView` 64-bit BigInt accessors. It does not add Atomics BigInt
operations, general BigInt arithmetic, or immutable-buffer support.

## What changed

1. **Map/Set BigInt keys.** `SameValueZero` already compared BigInts by
   value, but the collections' dictionary hashed them by pointer and the
   simple-map fast path treated every BigInt as pointer-comparable. A
   BigInt key therefore occupied a distinct slot for every allocation
   (`new Map([[1n,1],[1n,2]]).size` was 2).
   - `JavascriptBigInt::GetHashCode` hashes the canonical limb sequence
     (FNV-1a, sign mixed in) without allocating. Equal canonical values
     produce the same hash.
   - `SameValueComparerCommon<Var, zero>::GetHashCode` dispatches
     `TypeIds_BigInt` to that value hash.
   - `TryCanonicalizeAsSimpleVar` returns `nullptr` for BigInt, so a map
     or set containing a BigInt is promoted to the complex dictionary
     (the same path already used for strings and non-canonical numbers).
   Object, number, string, symbol, NaN and `-0` identity are unchanged;
   `WeakMap`/`WeakSet` remain object-only and reject BigInt as before.
2. **BigInt literal property names.** `PropertyName: NumericLiteral`
   uses `ToString(NumericValue)`, so `{ 1n: ... }` must define the key
   `"1"` (and `0x10n`/`0b101n`/`0o17n` their decimal values).
   `Parser::PidFromBigIntLiteral` parses the literal with the existing
   `JavascriptBigInt` factory and converts it with `ToString`, then
   interns the decimal key through the parser hash table. It is used for
   object literals, `get`/`set` accessors, class methods/accessors,
   class fields and destructuring patterns. `NextTokenIsPropertyNameStart`
   now recognizes `tkBigIntCon` so `get 2n() {}` works. The
   syntax-only/deferred parse returns the raw token and performs no
   runtime allocation; the AST-building parse does the conversion.
   `ParseClassPropertyName` now also returns the decimal pid through
   `pidHint` for BigInt, integer and float literal names, so class
   method/accessor naming uses `"1"`, `"get 2"`, `"set 4"`, and the
   static analogue rather than an empty name. The same one-line omission
   was fixed for ordinary numeric literal class names.
   Computed keys (`{[1n]: ...}`, `class C { [1n]() {} }`) already used
   `ToPropertyKey` and continue to work.
3. **DataView BigInt accessors.** New own prototype functions on
   `DataView.prototype`, installed only when `IsESBigIntEnabled()`:
   `getBigInt64`/`getBigUint64` (`length = 1`) and
   `setBigInt64`/`setBigUint64` (`length = 2`), with spec names,
   non-enumerable/writable/configurable descriptors, `ErrorOnNew`, and
   no `.prototype`.
   - Reads perform `ToIndex(byteOffset)`, `ToBoolean(littleEndian)`, then
     the detached/out-of-bounds checks. The 8 bytes are assembled
     byte-wise, so endianness and alignment are independent of the host.
     `CreateFromInt64` / `CreateFromUInt64` return exact BigInts for every
     bit pattern (`INT64_MIN`, `INT64_MAX`, `UINT64_MAX`, values above
     2^53) without a double round trip.
   - Stores perform `ToIndex(byteOffset)` before `ToBigInt(value)`, then
     `ToBoolean(littleEndian)`, then the detached/range checks, and keep
     the low 64 two's-complement bits via `ToUInt64Low`. A missing value
     is `undefined` and throws from `ToBigInt`; invalid offsets throw
     `RangeError` before any value conversion; an abrupt conversion
     propagates before the bounds check. Detached and shrunken
     fixed-length views throw `TypeError` as the rest of `DataView`
     does; auto-length views follow resize/recovery.
   - The four `FunctionInfo` entries are appended to
     `JavascriptBuiltInFunctionList.h` so existing built-in enum values
     and checked-in bytecode stay stable. No new static `PropertyId` was
     added; names are tracked dynamic property records.
4. **DataView constructor length.** `DataView.length` is now `1`
   (`buffer [ , byteOffset [ , byteLength ] ]`), matching the default
   largest-named-argument rule and test262/Node. This pre-existing
   registration error was a one-constant correction.

## Ownership note

The new paths add no owned buffer or finalizer:

- BigInt values produced by `DataView` reads and by the parser's literal
  key conversion are ordinary `JavascriptBigInt` objects plus digit
  arrays in the owning `ScriptContext` recycler. They are published only
  as return values or interned property names; the parser's temporary is
  unreachable after the key is interned and is reclaimed by the recycler.
- Map/Set hash computation reads the immutable BigInt limbs and does not
  retain a pointer. Dictionary promotion traces keys and values through
  the existing `ComplexVarDataMap` contract.
- `DataView` stores only bytes in the existing `ArrayBufferParent` view
  range; bounds are revalidated after `ToBigInt` user code, so no raw
  `buffer` pointer is used across that call. `ToBigInt` is the only new
  user-code point in the setters; reads and hash computation call no
  user code.
- No thread, cross-context, async or GC tracer change is introduced.

## Evidence

Pinned test262 `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`; shared gate
lock; local `.zig-cache`/`out`; before binary is the unchanged base
`/Users/ascotti/Documents/code/ChakraCore/zig-out/bin/ch`
(`8dd8d6b0…`, source HEAD `1c983ef9c`). After binaries:
`zig-out/bin/ch` `eb31ad36…` with `libChakraCore.dylib`
`91a92751…`; `out/debug-static/bin/ch` `6b6eff3f…`.
Both test262 runs passed all 18 controls.

Before/after focused corpus (144 scenarios / 72 files, host flags
`-nonative -Test262 -CollectGarbage -ESBigInt`):

| Selection | Before | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| Map/Set BigInt + literal property name + DataView BigInt | 38 | **140** | **102** | **0** |

The focused selection is `built-ins/Map/bigint-number-same-value.js`,
`built-ins/Set/bigint-number-same-value.js`,
`language/expressions/object/literal-property-name-bigint.js`, and
`built-ins/DataView/prototype/{getBigInt64,getBigUint64,setBigInt64,setBigUint64}/**/*.js`.
The 102 flips are 100 DataView scenarios and 2 literal-property-name
scenarios. The two upstream Map/Set files only assert Number-vs-BigInt
distinctness and already passed 4/4; identical-BigInt identity is covered
by the local test and the direct probe below.

Broad control selection (Map, Set, DataView and `language/expressions/
object|class`; 12,570 scenarios / 6,377 files) was **9,641/2,929**
before and **9,745/2,825** after: **104 fail→pass, 0 pass→fail**, with
identical result keys. The 104 are the same 102 focused flips plus the
two `built-ins/DataView/length.js` scenarios enabled by the constructor
length correction. Debug-static focused rerun: **140/4**, 102 fail→pass /
0 pass→fail against the base artifact.

Direct identity probe:
`new Map([[1n,1],[1n,2]]).size`, `get(1n)`, `new Set([1n,1n,2n]).size`,
and a Number/BigInt mixed map. Before:
`2 undefined 3 2 1 undefined`; after: `1 2 2 2 1 2`.

Local test `test/es7/bigint-collections-dataview.js`, registered once in
`test/es7/rlexe.xml` with `-ESBigInt -CollectGarbage -args summary
-endargs`, covers Map/Set identity and deletion, Number/BigInt separation,
object/string/symbol/NaN identity and weak collections, literal keys for
object methods/accessors/class members/class fields/destructuring and
computed keys, class method/accessor names for BigInt and numeric literal
keys (instance and static), `DataView.length`, accessor
descriptors/names/lengths/non-constructibility,
signed/unsigned boundaries, both endiannesses, custom offsets, ToIndex and
ToBigInt errors, conversion/range/detached ordering, and resizable views.
It passes on the test and debug-static binaries.

Builds (shared gate lock):

- `zig build --cache-dir .zig-cache --summary line`: **567/567**.
- `zig build -Dprofile=debug-static --cache-dir .zig-cache --summary line`:
  **545/545**.
- Existing local suites re-run green: `test/es7/bigint-{basics,asintn,
  literals,mixed,arithmetic}.js`, `test/es7/bigint64array.js`,
  `test/es6/{map,set,weakmap,weakset}_{basic,functionality}.js` (with
  `-ES6ObjectLiterals`), `test/es7/class-{fields-public,private-fields,
  private-methods}.js`, and `test/typedarray/dataview2.js`.
- Default-flag probe: all four DataView methods are `undefined` without
  `-ESBigInt` and `function` with it.
- `git diff --check` clean.

## Residual / risks

- **Immutable ArrayBuffer (4 focused failures).** The only remaining
  focused failures are `setBigInt64/immutable-buffer.js` and
  `setBigUint64/immutable-buffer.js` (default + strict); its first
  statement calls the unsupported `transferToImmutable`, so it fails
  before the accessor. The engine has no immutable-buffer support in this
  slice; the new stores use the same mutable/immutable contract as the
  other DataView setters. Not counted as a pass→fail.
- **Feature flag.** `ESBigInt` remains off by default. The DataView
  accessors are conditionally installed; literal property names are
  already gated by the scanner's `ESBigInt` check. Default-profile
  behavior is unchanged.
- **Parser allocation.** AST-building parse now creates one temporary
  `JavascriptBigInt` and decimal string per BigInt literal property name.
  Huge literals inherit the existing quadratic decimal `ToString`
  residual; this is parse-time, not a new retained allocation or a new
  unbounded result-size class. Deferred/syntax-only parse avoids it.
- **Class static fields.** BigInt-named instance class fields work.
  Static public fields (including ordinary numeric/identifier static
  fields) are a pre-existing parser gap, unrelated to this slice.
- **Pre-existing collection/typed-array residuals** listed in
  `c2-s2-bigint64array.md` remain: per-element `toLocaleString`, some
  detached-callback typed-array methods, and descriptor `configurable`
  reporting. Atomics BigInt operations remain out of scope.
- **B4.** No JIT lowering, inline cache, bailout, native frame or
  cross-thread claim is made. Mac no-JIT only; Windows/profiler link and
  native x64 behavior are not exercised here. `ESBigInt` default
  enablement still waits on B4.
