# C2-S2 sub-batch 5: BigInt arithmetic, shifts and bitwise operators

Status: `<pending>` (draft commit on `s2-bigint-arith`; interpreter-verified
on Mac ARM64 no-JIT with `-ESBigInt`; JIT is not claimed — see B4 note).
This slice completes the separable arithmetic/bitwise/shift part of S2 and
leaves typed arrays, collection key identity and literal property names to
later slices.

## Scope

- `+`, `-`, `*`, `/`, `%`, `**` on two BigInt primitives, including
  truncating division, remainder sign, non-negative exponents and the
  exponentiation special cases (`0n ** 0n`, `0n`, `±1n`, negative
  exponent, oversized result).
- `&`, `|`, `^`, `<<`, `>>` on two BigInts, including negative operands,
  negative shift counts and the infinite two's-complement representation.
  `>>>` remains a `TypeError` for BigInt operands, thrown after both
  operands have been converted.
- Unary `-`, `~`, wrapper objects and `@@toPrimitive` results for every
  operator in this set; mixed BigInt/Number (or Symbol/Boolean/String)
  operands throw `TypeError` with the spec's left-to-right `ToNumeric`
  order.
- Preserves the existing comparison/equality, `asIntN`/`asUintN`,
  increment/decrement and stringification behavior.

Not in scope: `BigInt64Array`/`BigUint64Array`, Map/Set BigInt key
identity, BigInt literal property names, giant-decimal stringifier
performance, and JIT lowering.

## Representation and algorithms

`JavascriptBigInt` remains a sign-magnitude value with little-endian
`digit_t` limbs (`uintptr_t`), canonical zero (one zero limb, never a
negative sign bit) and no high zero limbs. No representation or ownership
change was needed; the new operators build results through the existing
`CreateZeroWithLength`/`New`/`Resize` helpers.

- **Division/remainder** (`DivMod`, `JavascriptBigInt.cpp`): Knuth
  Algorithm D over 32-bit half-limbs. Each `digit_t` is split into
  `sizeof(digit_t)/4` little-endian `uint32` units, so every intermediate
  (32x32 product, normalized subtraction, quotient estimate) fits a
  `uint64` without `__int128`, matching the portability rule already used
  by `DivModInPlace`/`ToDouble`. A single-unit divisor takes a direct long
  division loop. Quotient sign is the XOR of the operand signs; remainder
  sign follows the dividend; both results are canonicalized and the
  remainder is `< |divisor|`. A zero divisor throws `RangeError` before
  any result is published.
- **Exponentiation** (`Exponentiate`): negative exponent throws
  `RangeError` first; `0n ** 0n` is `1n`; a zero base with a positive
  exponent is `0n`; `±1n` is handled without traversing the exponent.
  Otherwise the magnitude is raised by binary exponentiation with the
  existing `MulAbsolute`, and the sign is applied from the exponent's low
  bit. The result is bounded to `2**20` bits before multiplying; a larger
  exponent (including an exponent wider than 64 bits) throws `RangeError`
  instead of attempting an unbounded allocation.
- **Shifts** (`ShiftLeftMagnitude`, `ShiftRightMagnitudeFloor`): left
  shift materializes `magnitude << bits` with a limb shift and a sub-limb
  shift; right shift implements `floor(magnitude / 2**bits)`, including
  the negative rounding rule (`-5n >> 1n === -3n`). Negative counts
  reverse the direction. A count that does not fit 64 bits saturates: a
  huge left shift throws `RangeError` for a non-zero value, a huge right
  shift yields `0n`/`-1n`, and zero always yields `0n`.
- **Bitwise** (`BitwiseOperation`): both operands are expanded into an
  `n = max(length) + 1` limb two's-complement window (the extra limb holds
  the infinite sign extension), AND/OR/XOR is applied limbwise, and a
  negative result is converted back with complement-plus-one. This gives
  the spec's infinite-precision behavior for mixed-sign operands.
- **Coercion** (`JavascriptMath.cpp`, `JavascriptOperators.cpp`): binary
  operators call `ToNumeric` on the left operand and then on the right,
  exactly as the runtime semantics specify. `ToNumeric` now performs
  `ToPrimitive(hint Number)` for objects and returns a BigInt primitive
  unchanged (the wrapper path used by postfix `++`/`--`). Only after both
  conversions does the helper require equal types, so an abrupt left
  coercion is observed before the right operand, and mixed operands throw
  `TypeError` after both conversions. Unary operators use the same
  single-operand rule.
- **Existing defects exposed by the operator matrix**: the parser folded
  a second unary minus onto a BigInt literal by setting the sign bit
  instead of toggling it (`-(-1n)` was `-1n`); `0n - 0n` could acquire a
  negative sign bit; and `AbsoluteDecrement` left a zero-length zero that
  compared unequal to the `0n` literal. The parser now toggles the fold
  flag, the zero path uses `Negate`, and `AbsoluteDecrement` canonicalizes
  to one zero limb; `CompareAbsolute` additionally treats either zero
  shape as equal.

## Tests

New `test/es7/bigint-arithmetic.js`, registered in `test/es7/rlexe.xml`
with `-ESBigInt -args summary -endargs`, covers:

- exhaustive small `+ - * / %` matrices (`-12..12`) against Number
  arithmetic, with zero-divisor `RangeError` on both sides of the sign;
- truncating quotient and dividend-sign remainder cases, plus
  multi-limb quotient/remainder identities (`q*b + r === a`, remainder
  bound and sign) over 128..512-bit values and negative dividends;
- fixed multi-limb division, multiplication-free expected values and
  `q`/`r` constants;
- exponentiation values, sign rules, `0n ** 0n`, negative-exponent and
  oversized-exponent `RangeError`, and a valid large exponent compared
  against `1n << 100000n`;
- exhaustive small `& | ^`, identities (`a & ~a`, `a | ~a`, `~a ===
  -a - 1n`), and fixed multi-limb mixed-sign bitwise results;
- shifts from 0 through 130 bits, negative counts, floor rounding for
  negative values, round trips, saturated huge counts, zero counts and
  accepted/rejected large shifts;
- mixed-type `TypeError` for every binary operator (with `+` keeping its
  string-concatenation behavior), wrapper operands, prefix/postfix
  increment on wrappers, `@@toPrimitive` call order and abrupt-throw
  propagation, and zero-divisor ordering after coercion;
- canonical-zero/unary-minus/decrement regressions;
- `asIntN`/`asUintN` interoperability with arithmetic results;
- a boundary/crash matrix over 64/128-bit limits and division by `-1n`.

`test/es7/bigint-basics.js` was updated where it asserted the old
`1n ** 2n`/`1n << 1n`/`BigInt(1) / BigInt(1)` "unsupported" `TypeError`;
those now assert the arithmetic results, while the mixed forms remain
`TypeError` probes.

## Evidence

All runs are Mac ARM64, `-nonative`, `-Test262 -CollectGarbage -ESBigInt`
for test262 (interpreter only). Builds were run under the shared
`cc-gate-lock.sh` with `--cache-dir "$PWD/.zig-cache"`; test262 used
`--checkout "$PWD/out/test262"` and local output directories.

- `mise exec -- zig build --cache-dir .zig-cache --summary line`:
  **566/566** steps succeeded (test profile).
- `mise exec -- zig build -Dprofile=debug-static --cache-dir .zig-cache
  --summary line`: **544/544** steps succeeded; the debug-static binary
  was used for the focused test262 and local-test reruns below.
- Focused test262 corpus
  (`test/built-ins/BigInt/**/*.js` + `test/language/expressions/**/*bigint*.js`):
  **446 scenarios / 223 files**, baseline
  `out/test262-s2arith-baseline` **364 passed / 82 failed**, final
  `out/test262-s2arith-final3` and debug-static
  `out/test262-s2arith-final-debug` **444 passed / 2 failed**.
  **80 fail→pass / 0 pass→fail**. Split:
  - `test/built-ins/BigInt/**/*.js`: 154/154 before and after (77 files;
    `asIntN`/`asUintN` and conversions were already landed).
  - `test/language/expressions/**/*bigint*.js`: 210/292 before,
    **290/292 after** (146 files); the only two residual scenarios are
    the property-name parser gap below.
  - Flips by directory: bitwise-and 8, bitwise-or 8, bitwise-xor 8,
    bitwise-not 2, division 8, modulus 8, exponentiation 10, left-shift
    8, right-shift 8, unsigned-right-shift 2, unary-minus 4, subtraction
    2, postfix-decrement 2, prefix-decrement 2.
- Broader arithmetic expression directories (addition, subtraction,
  multiplication, division, modulus, exponentiation, bitwise-and/or/xor/
  not, left/right/unsigned-right shift, unary minus/plus): **1027/1027
  scenarios** on the final test binary (`out/test262-s2arith-broad2`).
  This set was not measured against the base binary, so no before/after
  flip claim is made for it. An intermediate version that coerced both
  operands eagerly had 22 order-of-evaluation failures; the sequential
  `ToNumeric` implementation fixed them.
- Local suites on the test and debug-static binaries, all pass:
  `test/BigInt/*.js` (12 files), `test/es7/bigint-*.js` (5 files,
  including the new matrix), and the new test alone.
- Node v26.5.0 oracle: a deterministic (seed 20261018) matrix of 142
  random and edge values from 1 to 1,000 bits (all-zero/all-one/
  single-bit patterns), 642 value pairs for the ten binary operators,
  38 shift counts applied to every value in both directions, and
  exponents 0..5 produced **18,064 output lines**; every line is
  byte-identical to Node on the test binary, and the same matrix is
  byte-identical on debug-static. A separate division matrix of 2,500
  random pairs up to 1,200 bits (including all-one divisors and
  `2**(n-1)+1` shapes) is also byte-identical. No SIGABRT, assertion or
  non-zero exit occurred in either binary.
- `git diff --check` clean. Focused crash probes (division/modulo by
  zero, exponent bounds, negative shifts, mixed types, throwing
  `@@toPrimitive`) exit 0 on both binaries.

## Residual work

- **BigInt64Array/BigUint64Array** and all typed-array BigInt
  conversion/indexing work remain C3.
- **Map/Set key identity** for BigInt values remains C3 (the complex
  dictionary still uses pointer identity for BigInt instead of
  SameValueZero).
- **BigInt literal property names** (`({1n: 1})`, `class C { 0x10n() {} }`)
  remain the pre-existing parser gap; the focused corpus keeps two
  `literal-property-name-bigint.js` scenarios failing for that reason.
- **Giant-value stringification** is still quadratic in the decimal/hex
  digit-array converter. Arithmetic itself completes, but printing a
  ~105,000,000-bit shift result is impractical; this is the existing
  `ToString` performance debt, not part of this slice.
- **Exponent resource limit**: results above `2**20` bits throw
  `RangeError` (V8's implementation limit is larger). The observable
  contract for oversized mathematically defined results is an explicit
  exception, not a host abort; the exact bound is implementation-defined
  and documented here.
- **`>>>`** on BigInt is deliberately `TypeError`; no unsigned BigInt
  right shift exists in the specification.
- **JIT**: no lowering, inline-cache, bailout or native frame behavior was
  added or verified. `ESBigInt` stays off by default until C3 completion
  and the B4 native gate. The existing `Op_*_Full` helper signatures are
  unchanged.

## Ownership record (new arithmetic results and scratch arrays)

```text
Resource / creation call / source location:
  JavascriptBigInt results via CreateZeroWithLength / New / CreateOne in
  JavascriptBigInt.cpp (DivMod, Exponentiate, ShiftLeftMagnitude,
  ShiftRightMagnitudeFloor, BitwiseOperation); digit arrays via
  RecyclerNewArrayLeafZ; uint32 scratch arrays in DivMod and digit_t
  scratch arrays in BitwiseOperation.
Domain and allocator instance:
  The owning ScriptContext's Recycler. All results and scratch arrays are
  Recycler objects; there are no foreign allocations, finalizers or
  external buffers.
Logical length / borrowed views:
  A result owns m_length limbs in m_digits (capacity m_maxLength).
  DivMod scratch units contain no traced pointers and are never retained;
  BitwiseOperation scratch arrays are untraced digit_t storage. Shift
  results allocate exactly the final limb count (plus canonical trim).
  No operation returns a view into an input's digit array.
Owner; transfer/publication point:
  The return value owns its result. Inputs are never mutated by the new
  operators: division/remainder, shifts and bitwise operations copy
  values into results, and exponentiation copies the base into `factor`
  before squaring. Existing Add/Sub still copy their inputs at the Var
  entry points.
Borrow invalidation:
  GC may move/collect any BigInt; no raw digit pointer crosses an
  allocation or user-code boundary. Recycler stack scanning covers the
  C++ locals held across MulAbsolute calls, matching the existing
  arithmetic helpers.
Potential user-code or reentrancy points:
  Only ToNumeric/ToPrimitive on the operands (before the BigInt entry
  points) can run user code or throw. DivMod/Exponentiate/shifts/bitwise
  only allocate and throw engine errors.
Thread/context:
  Created and used on the owning ScriptContext thread; no new cross-
  context sharing or marshaling is introduced.
Release on success, partial failure, exception, OOM:
  Recycler GC; no explicit release. A zero-divisor, negative-exponent or
  oversized-shift/exponent error is raised before a result is published;
  partial scratch arrays become unreachable.
Memory accounting / bound:
  Division is O(n*m) scratch units and O(n) result limbs. Exponentiation
  is bounded before allocating. Shifts use the existing
  BigIntMaxResultLimbs (2**24 limbs) bound; off-by-one/overflow checks are
  before allocation. Bitwise uses max(input limbs)+1 limbs.
Tests / uncertainty:
  test/es7/bigint-arithmetic.js (all operators, limits, coercion order,
  debug-static), test262 focused corpus, Node oracle matrices. No
  GC-stress or concurrent-GC test was run for these results; that remains
  unverified until the B4/GC gate.
```

## B4 note

All evidence is Mac ARM64 no-JIT (interpreter). No new bytecode, static
PropertyIds or helper ABI are introduced. Native x64 JIT execution of the
new operator paths, BigInt inline caches and mixed-type bailouts still
require the B4 native gate; `ESBigInt` remains off by default.
