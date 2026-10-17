//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S2 sub-batch 1: BigInt conversions, stringification, crash safety

Status: `<pending>` (uncommitted; replace with the landed commit hash).
Measured on Mac no-JIT (`-nonative`) with `-ESBigInt`; JIT evidence is not
claimed (see B4 below). `ESBigInt` remains off by default. This sub-batch
keeps and completes the interrupted implementer's slice; it does not add
BigInt arithmetic, `BigInt.asIntN`/`asUintN`, `BigInt64Array`, or mixed
numeric comparison (C3 completion, residual list below).

## What landed

1. **ToBigInt and the constructor** (`JavascriptConversion.cpp`,
   `JavascriptBigInt.{h,cpp}`): BigInt/Boolean/Number/String convert directly;
   Objects convert once via `ToPrimitive(hint Number)` and then re-enter
   `ToBigInt` (no double coercion). `null`, `undefined`, `Symbol`, a
   non-convertible post-ToPrimitive primitive, and `BigInt()` with no
   argument throw `TypeError`; non-integral, `NaN` and `±Infinity` Numbers
   throw `RangeError`; strings outside the `StringToBigInt` grammar throw
   `SyntaxError`. `new BigInt(...)` throws `TypeError` before any argument
   coercion (spec `BigInt` step 1). The 40 pre-slice SIGABRT scenarios
   (constructor/coercion and prototype `toString`/`valueOf` fixtures) are
   now controlled throws or correct results.
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

- `BigInt.asIntN` / `BigInt.asUintN` (56 scoped scenarios; includes the
  non-decimal BigInt literal parser gap for `arithmetic.js`).
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
- BigInt literal grammar for `0x`/`0b`/`0o` forms (and the `n` suffix on
  them) remains C3 parser work.
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
