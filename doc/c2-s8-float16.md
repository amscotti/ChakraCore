//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2-S8: bounded ES2025 Float16 slice — DataView get/setFloat16 and Math.f16round

Status: landed in `514ec7d9c` — implemented in the working tree on Mac no-JIT (JIT
gated at B4). This is the separable Float16 slice named by the S8 triage,
**not** the full resizable/growable ArrayBuffer project and **not** the
`Float16Array` constructor/typed-array element type.

## Scope

1. ES2025 `DataView.prototype.getFloat16 ( byteOffset [ , littleEndian ] )`
   (`length` 1, `name` `"getFloat16"`).
2. ES2025 `DataView.prototype.setFloat16 ( byteOffset, value
   [ , littleEndian ] )` (`length` 2, `name` `"setFloat16"`).
3. ES2025 `Math.f16round ( x )` (`length` 1, `name` `"f16round"`).

All three are installed unconditionally (no config flag). The new
properties use `AddFunctionToLibraryObjectWithPropertyName`, matching the
convention used by the recent Array/TypedArray/BigInt additions: no
static `PropertyIds`/`JnDirectFields.h` entries, no bytecode-header
change (`verify_bytecode_pure`: "source headers unchanged; regen exit 0").

## Conversion design

`lib/Runtime/Library/Float16.h` is a header-only, portable binary16 codec
using integer bit manipulation only (no compiler `__fp16`, no host
floating-point rounding mode, no libm calls):

- **Decode** (`Float16::ToDouble`): exact widening of the 1/5/10 format.
  Normals and subnormals are assembled directly into binary64 bits;
  ±0, ±Infinity and NaN keep their sign, and NaN fraction bits are
  carried into the binary64 high fraction (the Number boxing layer
  subsequently canonicalizes NaN payloads). The DataView getter therefore
  returns the exact represented Number, with NaN as an implementation
  NaN.
- **Encode** (`Float16::ToBits`): round-to-nearest, ties-to-even.
  - binary16 normals (`|x| >= 2^-14`): the 52-bit binary64 fraction is
    rounded to 10 bits, with carry advancing the exponent; a carry past
    the maximum exponent produces ±Infinity.
  - binary16 subnormals (`2^-25 <= |x| < 2^-14`): the significand is
    rounded to an integer number of 2^-24 units; q = 1024 becomes the
    smallest normal (fraction 0, exponent 1).
  - underflow: exactly 2^-25 ties to zero (even); values above it round
    to the smallest subnormal, values below it to ±0; binary64
    subnormals are far below the threshold and round to ±0.
  - overflow: 65520 is the tie between the largest finite 65504 and the
    unrepresentable 65536; it and everything above round to ±Infinity.
  - NaN encodes as quiet NaN `0x7E00` with the input sign. NaN payload is
    implementation-defined; tests only require NaN, and Node's choice of
    a different payload is treated as an equally valid encoding.
- **`Math.f16round`** applies the same encode/decode round trip and
  returns the represented Number, not a 16-bit integer.

## DataView wiring

- `DataView::EntryInfo::{Get,Set}Float16` are declared in
  `DataView.h` and registered in `JavascriptBuiltInFunctionList.h`
  (appended, so existing built-in ids do not shift) plus
  `JavascriptLibrary::InitializeDataViewPrototype`.
- The generic DataView templates now share `GetViewIndex` (ToIndex) and
  `CheckViewRange` (detached then range) helpers. This fixes the
  detached/range order for existing methods too: for offsets within
  `MaxArrayBufferLength`, ToIndex precedes the detached check, and the
  detached check precedes the out-of-range check. The Float16 entries use
  the same helpers. Existing setters still convert their value before
  ToIndex (pre-existing); offsets above the engine ArrayBuffer cap are a
  separate residual.
- `getFloat16`: brand check → `ToIndex(byteOffset)` (missing argument is
  `undefined` → 0) → `ToBoolean(littleEndian)` → detached check → range
  check → read two bytes and widen. Detached-buffer order is therefore
  `getFloat16(-1)` → RangeError (ToIndex first), while `getFloat16(13)`
  on a detached view → TypeError (detach before range).
- `setFloat16`: brand check → `ToIndex(byteOffset)` → `ToNumber(value)`
  (missing value is `undefined` → NaN) → `ToBoolean(littleEndian)` →
  detached check → range check → encode and store two bytes. Negative or
  infinite indices throw before the value is converted; a value-side
  abrupt completion wins over the later range check.
- Byte assembly is explicit in both endiannesses (no alignment or
  platform-endian assumption).

## Math wiring

`Math::F16round` is declared in `MathLibrary.h`, defined in
`MathLibrary.cpp`, and registered in `InitializeMathObject` with a
dynamic property name (and in `ProfilerRegisterMath` through
`REG_OBJECTS_DYNAMIC_LIB_FUNC`). ToNumber/NaN/error handling matches the
other Math unary functions; the descriptor is writable, non-enumerable,
configurable, and the function is non-constructible.

## Tests

New `test/es7/float16.js`, registered in `test/es7/rlexe.xml` with
`-args summary -endargs`:

- exhaustive decode of all 65,536 binary16 patterns in both
  endiannesses against an independent arithmetic decoder;
- exhaustive set-side round trip of every non-NaN pattern (NaN patterns
  write and read back as NaN);
- ±0 (including `-0`), ±Infinity, NaN, max finite, overflow tie, the
  underflow tie, smallest/largest subnormal, smallest normal, exact
  boundary doubles and ties-to-even at 2049/2051 and in the subnormal
  range;
- little-endian byte order, custom offset, return values;
- argument coercion and error order (brand, ToIndex before value
  conversion, value conversion before range, missing arguments,
  Symbol/abrupt conversions), detached-buffer ordering, descriptors,
  names, lengths and non-constructibility;
- `Math.f16round` values, special values, missing argument, Symbol error,
  descriptor and non-constructibility, and agreement with DataView for
  every non-NaN pattern.

## Evidence

- `zig build --summary line`: 566/566 steps succeeded.
- `zig build check --summary line`: **1982 passed / 0 failed**,
  bytecode up to date, 572/572 steps.
- `zig build check -Dprofile=debug-static --summary line`:
  **2034 passed / 0 failed**, bytecode up to date, 549/549 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative
  controls pass with the 1982/2034 pins.
- Focused Float16 corpus (`getFloat16/**`, `setFloat16/**`,
  `math/f16round/**`, 100 scenarios, 50 files), same binary/flags:
  **22/100 before → 94/100 after** (72 fail→pass, 0 pass→fail). The
  debug-static final rerun is also 94/100. The 6 residue are classified
  under Residuals below.
- Full `test/built-ins/DataView/**` corpus: 1,122 scenarios,
  **806/316**, with **174 fail→pass / 0 pass→fail** against the C1
  baseline records. 62 flips are the Float16 DataView scenarios; the
  other 112 are existing get/set methods whose detached-buffer ordering
  now matches test262 thanks to the shared check helpers. The 10
  `Math.f16round` scenarios are outside this DataView corpus.
- Node v26.5.0 conversion matrix: 331,105 output lines per engine,
  covering all 65,536 decode patterns, exhaustive non-NaN set-side
  round trips, boundary/ties, and 200,000 deterministic arbitrary
  binary64 inputs. Every non-NaN line is byte-identical. Decode lines
  (including NaN patterns) are identical; encode differs only for NaN
  inputs because ChakraCore writes canonical `0x7E00` while Node
  preserves a payload — both valid NaN encodings.
- `git diff --check` clean; no generated or stray files.

## Residuals

- **Resizable buffers (4 scenarios, S8 proper)**: both
  `DataView/prototype/getFloat16/resizable-buffer.js` and
  `setFloat16/resizable-buffer.js` (default + strict) stop at
  `typeof ArrayBuffer.prototype.resize !== "function"`. This is the
  untaken resizable/growable ArrayBuffer project, not a Float16 defect.
- **Immutable ArrayBuffer (2 scenarios, post-ES2025)**:
  `setFloat16/immutable-buffer.js` (default + strict) needs
  `ArrayBuffer.prototype.transferToImmutable` and the
  `immutable-arraybuffer` proposal (`features: [immutable-arraybuffer]`
  is excluded from the ES2025 edition policy).
- **Detached/ToIndex edge (inherited)**: detached DataView offsets above
  the engine `MaxArrayBufferLength` cap still throw RangeError before the
  detached TypeError, and pre-existing setters convert `value` before
  ToIndex. These are inherited DataView order edges, not Float16 codec
  defects.
- **`Float16Array` (not in this slice)**: the constructor, prototype and
  typed-array element kind (`%TypedArray%` methods, species, JIT/typed
  array storage) remain the separable S8/C3 batch (76 C1 failures).
- **JIT/B4**: this receipt is Mac no-JIT only. No new JIT opcodes,
  PropertyIds or bytecode were added, so the B4 work is regenerating
  and re-running the jit variants, not porting a new operation.

## Ownership / allocation

No allocation, ownership, retained-pointer, GC-tracing or memory
accounting changes: `Float16.h` is pure arithmetic, the DataView
entries read/write the existing view buffer after the existing
detached/range checks, and `Math.f16round` allocates only the normal
Number result. The existing `DataView` buffer lifetime and detach
notification are unchanged.
