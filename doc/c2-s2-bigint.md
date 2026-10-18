//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S2 sub-batch 1: BigInt conversions, stringification, crash safety

Status: sub-batch 1 landed in `f7f396a88` (interpreter-verified; JIT gated at B4).
Sub-batch 2 (`BigInt.asIntN`/`asUintN`, and the spec correction that
`ToBigInt(Number)` throws) landed in `41ada630f`; sub-batch 3
(non-decimal `0x`/`0o`/`0b` BigInt literals) is implemented in the
working tree. Both are interpreter-verified; see "Sub-batch 2" and
"Sub-batch 3" below. Measured on Mac no-JIT (`-nonative`) with
`-ESBigInt`; JIT evidence is not claimed (see B4 below). `ESBigInt`
remains off by default. Sub-batch 1 keeps and completes the interrupted
implementer's slice; the batch as a whole still does not add BigInt
arithmetic, `BigInt64Array`, or mixed numeric comparison (C3 completion,
residual list below).

## What landed

1. **ToBigInt and the constructor** (`JavascriptConversion.cpp`,
   `JavascriptBigInt.{h,cpp}`): BigInt/Boolean/String convert directly;
   Objects convert once via `ToPrimitive(hint Number)` and then re-enter
   `ToBigInt` (no double coercion). `null`, `undefined`, `Symbol`, a
   non-convertible post-ToPrimitive primitive, and `BigInt()` with no
   argument throw `TypeError`; non-integral, `NaN` and `±Infinity` Numbers
   throw `RangeError`; strings outside the `StringToBigInt` grammar throw
   `SyntaxError`. `new BigInt(...)` throws `TypeError` before any argument
   coercion (spec `BigInt` step 1). The 40 pre-slice SIGABRT scenarios
   (constructor/coercion and prototype `toString`/`valueOf` fixtures) are
   now controlled throws or correct results. Sub-batch 2 corrected the
   abstract `ToBigInt` operation: a Number primitive is a `TypeError`
   (spec ToBigInt); the BigInt constructor now performs its own
   ToPrimitive/NumberToBigInt steps, so `BigInt(42)` still succeeds while
   `BigInt.asIntN(2, 42)` throws as required.
2. **Multi-digit NumberToBigInt** (`JavascriptBigInt::CreateFromNumber`):
   the interrupted slice stored one 64-bit digit and rejected `>= 2^64`.
   The conversion now splits an integral finite double into 32-bit chunks
   (lossless: doubles are binary) and feeds them most-significant first
   through `MulThenAdd(2^32, chunk)`. `BigInt(2^64)`, `BigInt(2^80)` and
   `BigInt(Number.MAX_VALUE)` therefore produce the full value; there is no
   stub range limit, only the engine's BigInt/string size limits. `-0`
   canonicalizes to `0n`.
3. **StringToBigInt** (`JavascriptBigInt::CreateFromString`): ECMAScript
   whitespace trim (excluding U+0085, which is not ES WhiteSpace), optional
   decimal sign, unsigned `0x`/`0X`, `0o`/`0O`, `0b`/`0B` prefixes, and
   decimal leading zeros. Empty/blank input is `0n`; a lone sign, a bare
   prefix, a sign in front of a prefix, `1e3`/`1.5`/`10n`, bad radix digits
   and embedded whitespace are syntax errors. Results are canonicalized
   (no high zero limbs, no negative zero).
4. **`typeof`** (`JavascriptBigInt::GetTypeOfString`,
   `StringCacheList.h`, `JavascriptLibrary::InitializeStaticValues`):
   BigInt values report `"bigint"`. The display string uses the plain
   `STRING` cache entry (not `PROPERTY_STRING`), so no static PropertyId
   was added; `bigint` is not in `JnDirectFields.h`.
5. **`BigInt.prototype` stringification** (`JavascriptLibrary.cpp`,
   `JavascriptBuiltInFunctionList.h`, `JavascriptBigInt.cpp`):
   - `toString([radix])`: `ToIntegerOrInfinity` via `ToInteger`, then a
     `RangeError` outside 2..36; default/`undefined` radix is 10; correct
     sign and `0`; digits use `a`..`z` for 10..35. The implementation copies
     the magnitude limbs and repeatedly divmods by a small radix in place
     (`DivModInPlace`, two 32-bit half-limbs per 64-bit limb, no `__int128`).
   - `valueOf()` returns the primitive; both accept a primitive BigInt or a
     `JavascriptBigIntObject` with a non-null `[[BigIntData]]`, and throw
     `TypeError` for everything else (including `BigInt.prototype` itself).
   - `toLocaleString()` returns the decimal string (the same spec
     observable this engine supports without Intl).
   - `Symbol.toStringTag` is an own `"BigInt"` data property on the
     prototype, so `Object.prototype.toString.call(1n)` is
     `"[object BigInt]"`.
   - Methods are installed with
     `AddFunctionToLibraryObjectWithPropertyName` (tracked dynamic property
     records), have `length = 0`, the spec names, and `ErrorOnNew`; the
     prototype's deferred-property capacity hint is 5. The profiler
     registration was extended in `ProfilerRegisterBigInt` (its
     `DEFINE_OBJECT_NAME(BigInt)` is now present) and
     `ProfilerRegisterBuiltIns` calls it when `ESBigInt` is enabled, so the
     NTBUILD profiler path both compiles and is reachable.
6. **Primitive conversions and stringification** (`JavascriptConversion.cpp`,
   `JavascriptMath.cpp`): `ToString`/`ToLocaleString` handle
   `TypeIds_BigInt`; `Add_FullHelper` checks String before the BigInt
   operand check, so `String(1n)`, template literals, `"x" + 1n`,
   `1n + "x"` and `"".concat(1n)` work. `ToBoolean` returns `false` for
   `0n` and `true` for every other BigInt (the interrupted slice had
   treated `0n` as true). `Number(BigInt)` is the one explicit
   BigInt-to-Number conversion and is correctly rounded: `ToDouble` finds
   the magnitude's bit length, takes the top 53 bits, and applies
   round-to-nearest, ties-to-even before `ldexp` (including infinity at
   the double boundary). `Number(Object(1n))` and objects whose
   `@@toPrimitive`/`valueOf` returns a BigInt apply `ToPrimitive` first and
   then the same conversion; `ToNumber(BigInt)` in ordinary operator
   contexts still throws. A 305-value byte comparison against Node covers
   values from 1 bit through 1000 decimal digits.
7. **Wrapper objects** (`JavascriptBigInt::ToObject`,
   `JavascriptLibrary::CreateBigIntObject`): `Object(1n)` produces a real
   `JavascriptBigIntObject` with `[[BigIntData]]`, so `valueOf`/`toString`
   and ordinary `ToPrimitive` reach the primitive. `ThisBigIntValue`
   marshals the wrapper's primitive through `CrossSite::MarshalVar`, so
   `BigInt.prototype.valueOf.call(otherRealm.Object(otherRealm.BigInt(0)))`
   returns a target-context `0n`; before the fix the Debug build asserted
   in `InterpreterStackFrame::ValidateRegValue` (test262 cross-realm
   `valueOf` failed only on debug-static). Full wrapper arithmetic and
   abstract equality remain C3 (residual list).
8. **JSON stringification** (`JSONStringifier.cpp`): `JSON.stringify`
   consults `toJSON` for a BigInt primitive as well as for objects,
   unwraps `[[BigIntData]]` from `JavascriptBigIntObject`, and throws
   `TypeError` for a BigInt that survives `toJSON`/the replacer. The six
   `test/built-ins/JSON/stringify/value-bigint*.js` files pass 12/12
   scenarios on both binaries, including the replacer, `toJSON` receiver
   and cross-realm cases.
9. **Crash safety / controlled failure** (`JavascriptConversion.cpp`,
   `JavascriptMath.cpp`, `JavascriptOperators.cpp`):
   - `ToNumber`, `ToInteger`, `ToInt32`, `ToUInt32`, `ToUInt16` and the
     strict `ToInt32Finite` now throw `TypeError` for BigInt instead of
     reaching the "bad type object" assert. This covers `/`, `%`, `**`,
     unary `+`, shifts, bitwise operators and `Math.*` argument coercion.
   - Relational comparison between BigInt and any non-BigInt remains
     fail-closed `TypeError` (no `AssertOrFailFast`).
   - `JavascriptBigInt::Equals(JavascriptBigInt*, Var, ...)` no longer
     asserts on a non-BigInt operand; it returns `false`.
   - The `Add` path rejects BigInt with a non-string primitive via a
     controlled `TypeError`; same-BigInt `+`/`-`/`*`, unary `-`, `~`, `++`,
     `--` continue to work.
   - `Number(1n)` is the one explicit BigInt-to-Number conversion; it uses
     the correctly rounded `JavascriptBigInt::ToDouble` described above.
   - The `ToBoolean` and `JSON.stringify` paths are covered by focused tests
     (`Boolean(0n)`, `Boolean(1n)`, root/property replacer, `toJSON` on a
     primitive and a wrapper, and the unconditional `TypeError`).

## Tests and gates

- New `test/es7/bigint-basics.js` (registered in `test/es7/rlexe.xml` with
  `-ESBigInt -args summary -endargs`) covers Number/String/Boolean/object
  conversions (including `-0`, whitespace, signs, radix prefixes, invalid
  strings, `null`/`undefined`/Symbol/no-arg), `new BigInt`, `typeof`,
  `ToBoolean(0n)`/`ToBoolean(1n)`, correctly rounded `Number(BigInt)` and
  `Number(Object(1n))`, `toString`/`valueOf` descriptors and radix behavior, `String`/template/
  concatenation, `Object(1n)`, `JSON.stringify` rejection/replacer/`toJSON`,
  and the crash matrix. Passes on both the test and debug-static binaries.
- `test/BigInt/global_object.js` was updated from the stub-era
  `BigInt.prototype == "[object Object]"` expectation to the spec
  `Object.prototype.toString.call(BigInt.prototype) == "[object BigInt]"`;
  the old loose comparison now throws `TypeError` because the prototype has
  new own `valueOf`/`toString` and no `[[BigIntData]]` (Node 26.5.0
  agrees).
- `mise exec -- zig build check --summary line`: 1975 passed / 0 failed,
  bytecode up to date, 572/572 steps.
- `mise exec -- zig build check-regression -Dprofile=debug-static
  --summary line`: 2027 passed / 0 failed, 545/545 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls pass, including the debug 2027/0 pin; partial
  `--expected-passed`/`--expected-failed` overrides were exercised
  separately.
- `zig build` test and `-Dprofile=debug-static` both build.
- One first debug gate run after the portability build reported 2026/1;
  the failing detail line was not retained. Two consecutive full reruns
  were 2027/0, and the focused BigInt/es7 plus scoped test262 suites were
  green. This is recorded as an unexplained transient, not counted as
  evidence.
- `JSON/stringify/value-bigint*.js` (6 files): 12/12 scenarios on both the
  test and debug-static binaries.

## Portability review

The core BigInt representation uses `digit_t = uintptr_t`, which is 32-bit
on x86/ARM Windows and 64-bit on the main builds. The new
`NumberToBigInt` chunking, `ToString` capacity/divmod, and `ToDouble` bit
extraction therefore derive the limb width from `sizeof(digit_t)` rather
than hard-coding 64-bit limbs:

- `CreateFromNumber` splits a double into half-limb chunks (32-bit chunks
  on 64-bit limbs, 16-bit chunks on 32-bit limbs) and scales by the
  matching power of two; the chunk array is sized for the 1024-bit double
  exponent in either configuration.
- `DivModInPlace` divides two variable-width half-limbs per limb, with the
  divisor at most 36 and every intermediate in `uint64`.
- `ToString` reserves one character per bit using `sizeof(digit_t) * 8`.
- `ToDouble` collects the top 53 bits across one or more limbs and uses a
  limb-size-derived power-of-two base for the exact small-value path.

No 32-bit engine binary was available on this machine, so the 32-bit
runtime is not claimed; the algorithms were additionally checked with a
standalone `digit_t = uint32_t` model against Node's values. The Windows
profiler path is compile-reviewed only (NTBUILD/`ENABLE_SCRIPT_PROFILING`
is not built here): `DEFINE_OBJECT_NAME(BigInt)` supplies
`pwszObjectName`, and `ProfilerRegisterBuiltIns` conditionally registers
the construct. The Mac ARM64 gates exercise the 64-bit-limb path.

## Corpus evidence (scoped: `test/built-ins/BigInt/**/*.js`)

154 scenarios in 77 files, `-nonative -Test262 -CollectGarbage -ESBigInt`:

| Run | Passed | Failed | SIGABRT scenarios |
| --- | --- | --- | --- |
| Pre-slice (`out/test262-audit-bigint`, same files) | 32 | 122 | 40 |
| Interrupted-slice pre-completion (`/tmp/s2-bigint`) | 68 | 86 | 0 |
| This sub-batch, test binary (`/tmp/s2-bigint-postport`) | **94** | **60** | **0** |
| This sub-batch, debug-static (`/tmp/s2-bigint-debug-postport`) | **94** | **60** | **0** |

Flips: 62 fail→pass / 0 pass→fail vs the pre-slice artifact; 26 fail→pass
/ 0 pass→fail vs `/tmp/s2-bigint` (constructor and crash fixes were already
in the interrupted slice). The final debug-static rerun is also 94/60 after
the cross-realm marshal fix (it was 92/60 before that fix, with two SIGTRAP
scenarios in `prototype/valueOf/cross-realm.js`). The post-portability
rerun has 0 pass→fail against the pre-portability artifact (94 common
passes). Every remaining failure is classified:

| Class | Scenarios | Detail |
| --- | --- | --- |
| `BigInt.asIntN` missing | 28 | 14 files: `typeof` 10, missing-method calls 10, `length`/`name` 4, `not-a-constructor` 2, and `arithmetic.js` 2 blocked first by non-decimal BigInt literal syntax |
| `BigInt.asUintN` missing | 28 | Same 14-file shape |
| Wrapper arithmetic/equality | 2 | `wrapper-object-ordinary-toprimitive.js` stops at `Object(1n) == 2n` (abstract equality does not ToPrimitive the wrapper) and would next need wrapper `*`; `Object(1n)` itself, `valueOf`, `toString`, template/string concat and `1 + Object(1n)` now behave |
| Mixed relational comparison | 2 | `toString/a-z.js`: its loop condition `i < radix` compares BigInt with Number; the deliberately fail-closed relational `TypeError` blocks the file. `toString` itself passes every radix assertion |

No other failure class remains. The arithmetic failures are not counted as
a separate class because those files are currently gated by the non-decimal
literal parser gap; once `asIntN` exists, they still need `0x`/`0b`/`0o`
BigInt literal parsing.

## Crash matrix

Each post probe ran in its own process. "Pre" for constructor/conversion
rows is the measured pre-slice artifact (40 in-set SIGABRT scenarios);
"pre" for operator rows is pre-slice HEAD source behavior (the Debug
`AssertMsg` paths) because the pre-slice binary was not retained. `rc` is
the process exit code, `134` is SIGABRT.

| Probe | Pre | Post |
| --- | --- | --- |
| `BigInt(1)` | SIGABRT rc 134 (40 crash scenarios) | `1n`, rc 0 |
| `BigInt()` | SIGABRT rc 134 | `TypeError`, rc 0 |
| `new BigInt(1)` | SIGABRT rc 134 | `TypeError`, rc 0 |
| `BigInt("10")`, `BigInt(true)`, `BigInt({valueOf})` | SIGABRT rc 134 | correct value, rc 0 |
| `1n + 1` | `TypeError` | `TypeError`, rc 0 |
| `1n * 1` | `TypeError` | `TypeError`, rc 0 |
| `1n / 1` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `1n % 1` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `1n ** 2n` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `1n < 1` | `AssertOrFailFast` SIGABRT | `TypeError`, rc 0 |
| `1n == 1` | `false` | `false`, rc 0 |
| `-1n` | `-1n` | `-1n`, rc 0 |
| `+1n` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `1n++` | `2n` | `2n`, rc 0 |
| `1n & 1` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `BigInt(1) >>> 1` | SIGABRT (constructor) | `TypeError`, rc 0 |
| `1n << 1n`, `1n >>> 1` | debug assert / `Internal error` | `TypeError`, rc 0 |
| `String(1n)` | `Internal error`/wrong `[object Object]` | `"1"`, rc 0 |
| `1n + "x"`, `"x" + 1n` | `TypeError` | `"1x"`, `"x1"`, rc 0 |
| `typeof 10n` | `"unknown"` | `"bigint"` |
| `10n.toString()` | `"[object Object]"` | `"10"` |
| `Number(1n)` | debug assert / `Internal error` | `1`, rc 0 |
| `Boolean(0n)` | `true` (stub slice) | `false`, rc 0 |
| `JSON.stringify(1n)` | `undefined` (silent drop) | `TypeError`, rc 0 |
| cross-realm wrapper `valueOf` | debug SIGTRAP assert | `0n`, rc 0 |

A separate 86-expression matrix (add/mul/div/mod/pow, relational, unary
plus, shifts, bitwise, symbol/string operands, `BigInt(1)` on the left,
`Boolean`, `Number`/`Number(Object(1n))`, JSON, `Object(1n)`, cross-realm
wrapper valueOf, property keys and typed-array coercion) returned rc 0 for every process
on both the test and debug-static binaries; unsupported operations threw
only `TypeError`. No probe produced SIGABRT/rc 134.

Node v26.5.0 oracle: conversions, the StringToBigInt grammar, `typeof`,
radix output, `valueOf`, templates/concat, `ToBoolean`, JSON `toJSON`/
replacer ordering, and `Number(BigInt)` were compared. The `Number(BigInt)`
double bits match Node on all 305 generated values (1 to 1000 decimal
digits), and radix 2/8/16/36 strings match on 156 further generated values;
the remaining known differences are the C3 gaps recorded below
(mixed relational/abstract equality, wrapper arithmetic, Map/Set key
identity) and `toLocaleString` locale data (this engine is decimal-only
without Intl). A pre-existing, non-BigInt divergence was also observed:
the engine's Number-to-string formatter is not correctly rounded for some
wide doubles (e.g. the same `35527847360849915904` double prints as
`35527847360849915000`), so comparisons in this receipt use numeric
equality or bytes, not `String(number)`.

## Residual C3 work

- `BigInt.asIntN` / `BigInt.asUintN`: landed in sub-batch 2; the four
  `arithmetic.js` scenarios now pass after sub-batch 3's non-decimal
  literal support.
- Full BigInt arithmetic: `/`, `%`, `**`, shifts, bitwise, same-type
  `BigInt64Array`/`BigUint64Array`; mixed BigInt/Number relational and
  abstract equality (the last currently fails closed, except `==` returns
  `false` instead of ToPrimitive/compare).
- Wrapper objects in arithmetic and abstract equality (`Object(1n) * 1n`,
  `Object(1n) == 2n`); wrapper primitive extraction itself is done.
- `Map`/`Set` key identity for BigInt values: `new Map([[1n, 2n]]).get(1n)`
  is `undefined` because the complex dictionary uses pointer identity for
  BigInt instead of SameValueZero. This is a collections hash/equality
  slice, not a conversion fix.
- BigInt literal property names (`({1n: 1})`, `class C { 0x10n() {} }`)
  remain a pre-existing parser gap; computed keys and property values are
  covered.
- Decimal `ToString` of million-bit BigInts remains quadratic in the
  existing digit-array converter (the 1M-hex parse is now ~4.3 s, but
  stringifying that value is still a separate performance debt).
- `BigInt.prototype.toLocaleString` is decimal-only (no Intl locale data);
  no Intl behavior is claimed.
- `ToBigInt` covers the user-reachable Number/String/Boolean/object paths;
  `TypeIds_Int64Number`/`TypeIds_UInt64Number` are not added because the
  current JS surface does not produce them (host-only latent case).
- Pre-existing, not changed here: `Number(new Number(2))` unwraps
  `[[NumberData]]` before observing an overridden `valueOf`/`@@toPrimitive`
  (Node returns the overridden primitive). The new BigInt path only touches
  non-NumberObject objects; this is recorded as inherited debt.
- JIT lowering/verification is untouched.

## Ownership record (BigInt wrapper allocation)

This batch adds two retained-allocation paths: the `Object(1n)` wrapper and
the `JavascriptBigInt` values/digit arrays created by the new conversion
factories. Both follow the existing script-context Recycler contract:

```text
Resource / creation call / source location:
  JavascriptBigIntObject via JavascriptLibrary::CreateBigIntObject
  (JavascriptLibrary.cpp) -> RecyclerNew(scriptContext recycler);
  JavascriptBigInt via CreateZero/CreateZeroWithLength and
  CreateFromNumber/CreateFromString (JavascriptBigInt.cpp).
Domain and allocator instance:
  The owning ScriptContext's Recycler; wrapper, BigInt and digit arrays are
  all Recycler objects. There is no foreign allocation, finalizer or
  external buffer. `CreateFromNumber` validation happens before any
  allocation; `CreateFromString` may allocate a partial BigInt before
  discovering an invalid digit, returns nullptr, and leaves that unreferenced
  object to the Recycler. `MulThenAdd` may `Resize`, allocating a replacement
  digit array and copying; the old array becomes unreachable Recycler
  garbage. If an allocation throws OOM, no result is published.
Logical length / borrowed views:
  A BigInt owns `m_length` limbs in `m_digits` (capacity `m_maxLength`);
  Resize copies to a new array and does not retain a borrowed view. The
  wrapper has no separate buffer and stores a traced Field(JavascriptBigInt*)
  value; the digit array remains owned by that BigInt object.
Owner; transfer/publication point; remaining aliases:
  The wrapper owns the reference to the primitive for its lifetime.
  JavascriptBigInt::ToObject and the conversion factories publish the object
  only as the return value; callers may hold independent references to the
  immutable BigInt value.
Borrow invalidation:
  GC may move/collect both objects; the Field is traced and no raw pointer
  is retained by the wrapper or by callers. Cross-context marshal creates a
  replacement BigInt via CloneToScriptContext rather than sharing it.
Potential user-code or reentrancy points:
  ToObject is allocation-only. ThisBigIntValue reads the wrapper's field and
  does not call user code; ToPrimitive on the wrapper may call user-defined
  valueOf/toString before the intrinsic is reached.
Thread/context:
  Created and used on the owning ScriptContext thread; cross-context access
  goes through CrossSite::MarshalVar.
Release on success, partial failure, exception, OOM:
  Recycler GC; no explicit release. If RecyclerNew/Resize fails, the
  engine's OOM/exception path runs before a result is published; a partial
  BigInt from a rejected string is unreferenced and collected.
Memory accounting / bound:
  One wrapper per ToObject call and one BigInt+digit array per successful
  conversion; no retained capacity beyond the traced primitive and the
  BigInt's own limbs. No pooling or external accounting is added.
Tests / uncertainty:
  test/es7/bigint-basics.js Object(1n)/valueOf/toString; test262
  BigInt/prototype/valueOf/cross-realm.js (debug-static) and
  JSON/stringify/value-bigint-cross-realm.js. No GC-stress or
  concurrent-GC test was run for the new wrapper; that remains unverified
  until the B4/GC gate.
```

## B4 note

All evidence is Mac ARM64 no-JIT (`-nonative`, interpreter). The slice adds
no bytecode (the checked-in headers verify up to date) and no static
PropertyIds; it adds `FunctionInfo` entries for the new prototype methods.
Native x64 JIT execution, `Typeof`/`ToString`/`Add` lowering, and the
`BigInt.prototype` dynamic-property inline caches still require the B4
native gate before any JIT claim. `ESBigInt` stays off by default until
the C3 completion and a B4 run.

## Sub-batch 2: BigInt.asIntN / BigInt.asUintN

Status: landed in `41ada630f` on top of `725be0ccb`. Interpreter-verified
on Mac ARM64 no-JIT with `-ESBigInt`;
JIT is not claimed (same B4 gate as sub-batch 1).

### Scope

- `BigInt.asIntN(bits, bigint)` and `BigInt.asUintN(bits, bigint)`, their
  static property descriptors, `length = 2`, names, and non-constructibility.
- `ToIndex(bits)` with the spec error classes and left-to-right coercion
  order (bits before bigint).
- The `ToBigInt` correction required by the asIntN/asUintN fixtures:
  abstract `ToBigInt(Number)` throws `TypeError`; the BigInt constructor
  performs ToPrimitive(hint Number) and NumberToBigInt itself.
- Only the low `bits` bits of the magnitude are materialized. Positive
  in-range results and in-range signed results return the immutable input;
  `asUintN` negative results allocate exactly `ceil(bits/limbBits)` limbs
  and fill the two's-complement pattern. `asIntN` sign-extends by testing
  bit `bits-1` and, when set, converting the pattern to a negative
  sign-magnitude value with `2**bits - pattern`.
- Excluded and left C3: BigInt64Array, BigInt arithmetic beyond existing
  same-type `+`/`-`/`*`, non-decimal BigInt literals, mixed
  comparison/equality, wrapper arithmetic. No new static PropertyIds were
  added: both names are tracked dynamic property records, like the
  prototype methods; bytecode headers remain unchanged (verified).

### Implementation notes

- `JavascriptBigInt.cpp` adds `ToIndex`, `GetMagnitudeBitLength`,
  `GetMagnitudeBit`, `HasSingleMagnitudeBit`, `TruncateMagnitude`,
  `TwosComplement`, `AsIntN`/`AsUintN`, and the two entry points. Limb
  width is derived from `sizeof(digit_t)`, so 32-bit digit builds mask the
  partial top limb correctly.
- `asUintN(1000, -1n)` allocates 16 limbs on this build (1000 bits, top
  limb masked to 40 bits) and returns the exact 2^1000 - 1 decimal string.
  `asIntN(1000, -1n)` returns `-1n` without allocation; `asIntN(1000,
  2^999)` returns `-2^999`.
- Oversized results that a negative `asUintN` would have to materialize
  are rejected with `RangeError` at **>= 2^24 limbs** (128 MiB on 64-bit
  limbs, 64 MiB on 32-bit limbs); the exact boundary is rejected before
  allocation, so it cannot reach `ToString` and abort. This matches the class V8 uses for
  "Maximum BigInt size exceeded"; positive/in-range signed inputs above
  that bit count still return the unchanged value. `bits` itself is
  capped at `ToIndex`'s 2^53 - 1.
- `JavascriptLibrary.cpp` registers both static methods on the BigInt
  constructor (deferred capacity 5) with
  `AddFunctionToLibraryObjectWithPropertyName`; `ProfilerRegisterBigInt`
  registers the same dynamic names. `JavascriptBuiltInFunctionList.h`
  adds the `ErrorOnNew` FunctionInfo entries.

### Evidence

- `test/es7/bigint-asintn.js` (new, `test/es7/rlexe.xml` with `-ESBigInt
  -args summary -endargs`) passes on both the test and debug-static
  binaries. It covers bits 0/1/8/32/64/65/128/1000, signs, values above
  and below the bits, partial top limbs, exact 1000-bit `toString`
  results, ToIndex coercion/errors/order, BigInt/Symbol bits, all
  bigint-parameter error classes, `new`/`Reflect.construct` rejection,
  descriptors, and asUintN/asIntN round-trips.
- `mise exec -- zig build check --summary line`: 1979 passed / 0 failed,
  bytecode up to date, 572/572 steps.
- `mise exec -- zig build check -Dprofile=debug-static --summary line`:
  2031 passed / 0 failed, bytecode up to date, 549/549 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls pass with the 1979/2031 pins.
- Scoped corpus `test/built-ins/BigInt/**/*.js` with `-nonative
  -Test262 -CollectGarbage -ESBigInt`, same 154 scenarios:
  test binary (`out/test262-s2bigint-asintn-final3`) 146/154 and
  debug-static (`out/test262-s2bigint-asintn-debug-final3`) 146/154
  (baseline `/tmp/s2-bigint-final` 94/154).
  52 fail→pass (26 asIntN + 26 asUintN; 52 of the 56 asIntN/asUintN
  scenarios), 0 pass→fail on both. The 8
  remaining failures are exactly the four residue classes below; the
  requested 150/154 target is unreachable without the excluded
  non-decimal literal parser work (4 arithmetic scenarios).
- Node v26.5.0 oracle: 400 generated values (0-1100 bits, both signs,
  bits 0-1024 including partial-limb and 999/1000/1001 boundaries) match
  exactly for both functions; the error matrix (negative, fractional,
  +/-Infinity, 2^53, 2^53-1, BigInt/Symbol bits, Number/Symbol/
  unparseable strings for the bigint parameter, oversized negative
  asUintN) matches exception classes.
- `git diff --check` clean; no generated or stray files.

### Residuals (C3)

- `asIntN/arithmetic.js` and `asUintN/arithmetic.js` (4 scenarios) pass
  the method's numeric behavior only after the parser accepts
  `0x`/`0b`/`0o` BigInt literals; the file fails at its first such
  literal. Non-decimal BigInt literal parsing remains C3.
- `wrapper-object-ordinary-toprimitive.js` (2): wrapper arithmetic and
  abstract equality remain C3.
- `prototype/toString/a-z.js` (2): its `i < radix` loop still hits the
  fail-closed mixed BigInt/Number relational comparison (C3).
- All earlier residuals remain: BigInt64Array/BigUint64Array, full
  BigInt arithmetic/bitwise/shift, Map/Set BigInt key identity, wrapper
  arithmetic/equality, non-decimal literals, and JIT/B4 coverage.

### Ownership record

The new code adds no owned buffers beyond the existing Recycler contract:
`TruncateMagnitude` and `TwosComplement` allocate a `JavascriptBigInt`
plus a zeroed digit array in the owning ScriptContext's Recycler and
publish them only as the return value. `CreateZeroWithLength` bounds the
array to `ceil(bits/limbBits)` limbs; no foreign allocation or retained
borrow exists. The input BigInt is immutable and may be returned directly
when it is already in range; no caller mutates a returned value.
Reentrancy is confined to `ToIndex`/`ToBigInt`, which run user
`@@toPrimitive`/`valueOf`/`toString` before any result allocation, in
spec order. OOM/exception in the result allocation occurs before
publication; the partial object is Recycler garbage. GC may move all
objects; no raw pointer is retained across a user-code call.

## Sub-batch 3: non-decimal BigInt literals

Status: implemented in the working tree on top of `dbe3bd379`;
interpreter-verified on Mac ARM64 no-JIT with `-ESBigInt`. JIT is not
claimed (same B4 gate as sub-batches 1 and 2).

### Scope

- `0x…n`/`0X…n`, `0o…n`/`0O…n` and `0b…n`/`0B…n` in sloppy and strict
  code, including numeric separators between digits, negative literals,
  zero, multi-limb magnitudes, property values/computed keys, call
  arguments and `eval`/`new Function` source.
- The same scanner error behavior as decimal BigInt and Node: bare or
  invalid prefixes/digits, leading/trailing/doubled separators, a decimal
  point, exponent-like forms that are not valid in the base, an uppercase
  `N` suffix, and an identifier or digit immediately after the literal
  are `SyntaxError`s. `e`/`E` in a hex literal are digits, so
  `0x1e2n` is the integer `482n` (Node 26.5.0 agrees), not an exponent.
  Leading-zero decimal forms (`00n`, `08n`, `09n`, `081n`, `08_1n`,
  `0_0n`) are rejected; only `0n` is legal.
- Only literal parsing and construction changed. The decimal grouping path
  is retained, with a collateral fix that skips validated `_` separators
  (`1_2n` now correctly yields `12n`); no arithmetic, typed-array, mixed
  comparison/equality, wrapper or Map/Set behavior was added.

### Implementation notes

- `Scanner::FScanNumber` (`lib/Parser/Scan.cpp`) keeps the existing
  `DblFromHex`/`DblFromOctal`/`DblFromBinary` digit scan (and its
  numeric-separator placement rules). When `ESBigInt` is enabled, at
  least one digit was consumed (`pchT > p + 2`, so a bare `0x`/`0o`/`0b`
  is not accepted), and the next character is the lowercase `n` suffix,
  the scanner consumes the suffix, sets
  `LikelyNumberType::BigInt`, and emits `tkBigIntCon` with the full
  literal text (prefix + digits/separators + `n`). The shared
  `LIdCheck` still runs after the suffix, so an identifier start or
  decimal digit immediately after the literal remains an error.
- `JavascriptBigInt::InitFromCharDigits`
  (`lib/Runtime/Library/JavascriptBigInt.cpp`) detects `0x`/`0X`,
  `0o`/`0O` and `0b`/`0B` after the optional negative flag, strips the
  final `n`, and parses the prefixed digits with radix 16/8/2 through the
  existing `MulThenAdd`. Numeric separators are skipped; the scanner has
  already validated their placement. Non-decimal digits are folded in the
  largest radix-power chunk that fits a limb (15 hex / 21 octal / 63 binary
  digits on 64-bit limbs; 7/10/31 on 32-bit), keeping a 1M-hex literal
  parse around 4.3 s instead of the prior per-digit path. Decimal literals
  retain the base-1e9 grouping path. The radix loop works for both 32-bit
  and 64-bit `digit_t`; the sign still comes from the parser's
  negative flag and all-zero magnitudes canonicalize to `0n`. Invalid
  digits are only an internal invariant here: debug `Assert`s record a
  scanner bug, and release skips the character rather than aborting the
  host.
- No new static PropertyIds, object layouts, allocations or ownership
  domains: the literal still becomes one `JavascriptBigInt` and digit
  array in the owning ScriptContext's Recycler, as in sub-batch 1.

### Evidence

- New `test/es7/bigint-literals.js` (registered in `test/es7/rlexe.xml`
  with `-ESBigInt -args summary -endargs`) passes on both the test and
  debug-static binaries. It covers both prefix cases, zero, separators,
  multi-limb values, negative literals/negative zero, property/key/
  argument/return positions, `String`/`toString`/template output,
  equality and `asIntN`/`asUintN`, sloppy/strict functions and class
  methods, `eval`, `new Function`, and the invalid-form matrix.
- Node v26.5.0 oracle: 4,966 exhaustive short literal forms (all
  `0x`/`0o`/`0b` bodies up to three characters over digits, prefix
  letters, `n`/`N`, separators, dots and signs) plus 4,000 randomized
  bodies up to 12 characters matched Node byte-for-byte in acceptance,
  thrown error class and `String(value)` result; an independent review
  sweep of 27,639 short non-decimal forms found zero mismatches. The
  hand-written invalid matrix, including the leading-zero forms, matches
  Node's `SyntaxError` class.
- `mise exec -- zig build check --summary line`: 1980 passed / 0 failed,
  bytecode up to date, 572/572 steps.
- `mise exec -- zig build check -Dprofile=debug-static --summary line`:
  2032 passed / 0 failed, bytecode up to date, 549/549 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls pass with the 1980/2032 pins.
- Scoped corpus `test/built-ins/BigInt/**/*.js` with `-nonative
  -Test262 -CollectGarbage -ESBigInt`, same 154 scenarios: test binary
  (`/tmp/s2-bigint-literals-final5`) 150/154 and debug-static
  (`/tmp/s2-bigint-literals-debug-final5`) 150/154. Against the
  sub-batch 2 artifacts (`out/test262-s2bigint-asintn-final3` and
  `...-debug-final3`, 146/154), this is exactly the four
  `asIntN`/`asUintN` `arithmetic.js` scenarios (default + strict each):
  4 fail→pass, 0 pass→fail on both; against the 94/154 sub-batch 1
  baseline the aggregate is 56 fail→pass, 0 pass→fail. The requested
  150/154 target is reached; every remaining failure is classified in
  the residual list.
- `git diff --check` clean; no generated or stray files.

### Residuals (C3)

- Full BigInt arithmetic/bitwise/shift beyond the existing same-type
  `+`/`-`/`*`.
- `BigInt64Array`/`BigUint64Array`.
- Mixed BigInt/Number relational and abstract equality (the
  `toString/a-z.js` loop, 2 scenarios, still fails closed).
- Wrapper arithmetic/equality (`wrapper-object-ordinary-toprimitive.js`,
  2 scenarios).
- `Map`/`Set` BigInt key identity.
- JIT/native execution and B4 coverage; `ESBigInt` remains off by
  default.
