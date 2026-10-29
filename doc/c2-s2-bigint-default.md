# C2-S2: ESBigInt default-enablement

Status: **landed** — default flip `c2961de3e`, review repairs `66aed0c68`,
final cleanup `cb4e3c720`, merge `f73012a01`. `DEFAULT_CONFIG_ESBigInt` is
now `true` (`lib/Common/ConfigFlagsList.h:669`). The explicit `-ESBigInt`
flag still works and `-ESBigInt-` restores the previous default-off behavior
exactly. Final gates are **2030/0 test** and **2082/0 debug-static**
(`zig build check` **583/583**); the four new entries are default,
serialized, disabled, and debugger-display coverage. All evidence is Mac
ARM64 no-JIT; JIT is not claimed (B4).

## Audit

### Inventory / remaining gaps beyond JIT/B4, TTD, performance

The S2 receipts (`c2-s2-bigint.md` sub-batches 1–4,
`c2-s2-bigint-arithmetic.md`, `c2-s2-bigint64array.md`,
`c2-s2-bigint-collections.md`, `c2-s6-bigint-atomics.md` S6c/S6d) close the
whole bounded surface: conversions/stringification, `asIntN`/`asUintN`,
decimal/non-decimal literals, arithmetic/bitwise/shift, mixed
equality/relational, `BigInt64Array`/`BigUint64Array`, Map/Set identity,
literal property names, DataView BigInt accessors and BigInt Atomics
(flag-on with `ESSharedArrayBuffer`). No required feature gap remained
except the classes below, all verified by measurement in this audit:

- inherited typed-array engine defects (detach-callback semantics,
  `toLocaleString` override, index-descriptor `configurable`,
  cross-realm custom `newTarget` prototypes, resizable shrink tracking) —
  the non-BigInt analogs fail identically (several failures literally run
  "Testing with Float64Array");
- `ESSharedArrayBuffer` stays off by default (separate flag, separate
  decision);
- host/cross-agent Atomics waiter-list tests (no `$262.agent` host support);
- post-ES2025 surfaces that merely mention BigInt (Temporal,
  `Error.isError`, error `stack` accessor, `JSON.rawJSON`,
  `Array.fromAsync`, `Promise.allKeyed`/await-dictionary, Iterator
  `concat`/`includes`, ShadowRealm);
- `String.prototype.matchAll` / `RegExp.prototype` accessor receivers —
  gated by `ES6RegExSymbols` (still default-off): probed identical with
  Number/String receivers, not BigInt-specific;
- JIT/B4 (`Lower.cpp` `!CONFIG_FLAG(ESBigInt)` fast path), TTD, and
  giant-decimal stringification performance.

Two real default-enablement gaps were found and fixed by this slice
(see "Default impact").

### Broad pinned flag-on audit (`-ESBigInt`)

Corpus `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`; selection = every file
under `test/language`, `test/built-ins`, `test/annexB` whose path or
content matches /bigint/i (1,729 source files → 3,435 scenarios; the
selection listed 1,730 files but two blocking-host scenarios are skipped
before execution; BigInt built-ins,
language BigInt expression/literal/operator/property-name directories,
BigInt64Array/BigUint64Array incl. `ctors-bigint`/internals, DataView
BigInt, Map/Set, Atomics BigInt). Pre-flip binary (base `52c010c64`,
`ch` sha256 `b86055cf…`), controls 20/20 every run.

| Run | Flags | Passed | Failed |
| --- | --- | ---: | ---: |
| A1 flag-on | `-ESBigInt` | 2,531 | 904 |
| A2 default (BigInt off) | — | 198 | 3,237 |
| A3b Atomics BigInt | `-ESBigInt -ESSharedArrayBuffer` | 148 (of 216) | 68 |
| A3c Atomics non-BigInt analogs | `-ESSharedArrayBuffer` | 146 (of 216) | 70 |

**Every A1 failure is classified (904/904, zero unexplained)**:

| Class | Scenarios |
| --- | ---: |
| post-ES2025 (Temporal/Error.isError/stack/iterator/fromAsync/rawJSON/allKeyed/ShadowRealm) | 466 |
| `ESSharedArrayBuffer` flag off (incl. all Atomics-undefined) | 278 |
| inherited typed-array internals/detach/descriptor/resizable | 102 |
| inherited `%TypedArray%.prototype.toLocaleString` override (S8) | 22 |
| inherited ES6RegExSymbols-off (RegExp accessor receivers; matchAll) | 22 |
| inherited cross-realm custom newTarget prototype | 10 |
| inherited Array-over-RAB shrink tracking (S8) | 2 |
| known dynamic-import fixture churn | 2 |

A3b's 68 Atomics failures are all `wait`/`waitAsync` **big-int
multi-agent** waiter-list tests (SIGKILL 10 s timeout, `notify` 0-vs-N,
lapse assertions); A3c shows the non-BigInt wait/waitAsync analogs fail
with the same profile (146/70) — host/cross-agent class, documented
out-of-scope in S6c/S6d.

An accidental A3 launch without patterns ran the full default selection
with `-ESBigInt -ESSharedArrayBuffer` (93,141 scenarios, 78,311/14,830,
`out/test262-s2def-a3-atomics`). It is recorded as a diagnostic only, not
gate evidence, and motivated the "no full corpus" rule being respected
everywhere else.

### Default-vs-flag-on differential (same pre-flip binary)

A2 → A1 join on (file, scenario), 3,435 common: **2,333 fail→pass,
0 pass→fail**, 198 both-pass, 904 both-fail. **No scenario in the affected
selection passes only because BigInt is absent**, so the flip cannot cause
a pass→fail in this set. (The 904 both-fail are the documented classes
above and fail identically without the flag.)

### Flag/config inspection

- Single authoritative default: `lib/Common/ConfigFlagsList.h:669`
  (`FLAGR` registration at `:1197` unchanged). Runtime surface gates on
  `IsESBigIntEnabled()` (`JavascriptLibrary.cpp` BigInt
  prototype/constructor/types, `BigInt64Array`/`BigUint64Array`,
  DataView accessors, profiler registration; `Parser/Scan.cpp` literal
  scan; `ByteCodeEmitter.cpp` `Conv_Numeric` opcodes;
  `ThreadConfigFlagsList.h` accessor). `Lower.cpp:23967` keys a JIT
  object-equality fast path on `!CONFIG_FLAG(ESBigInt)` — B4 class.
- Disable syntax: boolean flags parse a trailing `-` as false
  (`CmdParser.cpp ParseBoolean`), the existing `-ES6Unicode-` convention;
  `-ESBigInt-` verified at runtime (see below).
- Test config assumptions: `test/BigInt/rlexe.xml` (11 entries) and
  `test/es7/rlexe.xml` (8 entries) pass explicit `-ESBigInt` and remain
  green; benchmarks mention BigInt only in comments. Debugger
  snapshots of the global object assumed the three BigInt globals absent
  (handled below). `tools/test262/edition-policy.json`
  `disabled_by_default_flags.ESBigInt` is now stale (shared C1 pin — NOT
  edited here; the C1 owner should reclassify, since BigInt-tagged
  failures in future default runs are real, and `BigInt` should move out
  of the disabled-flag bucket).

## Flip and default impact

1. `DEFAULT_CONFIG_ESBigInt (false)` → `(true)` (one line).
2. **Debugger type handling** (`lib/Jsrt/JsrtDebugUtils.cpp`): with the
   globals present, debug snapshots reach BigInt values/objects and hit
   `AssertMsg(false, "Unhandled type")` (debug builds) / silently missing
   type metadata (release). Added `TypeIds_BigInt` (primitive: type
   `"bigint"` + display string, mirroring Symbol) and `TypeIds_BigIntObject`
   (object group + `GetClassName` `"BigInt"`, mirroring `SymbolObject`).
   No new PropertyIds or cached display strings.
3. **66 debugger baselines total**: global-object snapshots now list
   `BigInt`, `BigInt64Array`, `BigUint64Array`; snapshots that stringify
   the `BigInt` constructor's `prototype` child show `BigInt {...}`
   instead of `undefined undefined`. 60 of the 66 files differ only by
   the three added global keys (0 removals/value changes); the remaining
   6 (`ES6_proto_simple`, `ES6_proto_userDefinedObject`,
   `array_prototest`, `bug_os_2946365`,
   `step_in_from_interpreted_function_attach`, `targeted`) additionally
   expand the `BigInt` constructor one level, so they also show its
   `length`/`name`/`asIntN`/`asUintN` properties and the corrected
   `prototype: "BigInt {...}"` value. All 66 were promoted from
   machine-generated `.rebase` output with CRLF preserved after a
   JSON-recursive diff verified additions-only. (Same procedure as the
   S8 Float16Array flip.)

Public `JsGetValueType` classifies a BigInt as `JsObject` (no
`JsBigInt` JsValueType exists); inherited API-surface limitation,
recorded as residual.

## Post-flip evidence (final binary: test `ch` sha256 `89e5833d…`,
dylib `59e1481b…`, debug-static `2ebd4a6a…`)

- **B1 default (affected selection, 3,435 scenarios): 2,531/904 —
  2,333 fail→pass / 0 pass→fail vs A2**, and **0/0 flips vs A1**
  (default ≡ flag-on, per-scenario).
- **B2 explicit `-ESBigInt`** (BigInt built-ins + literals + language
  expressions + BigInt64Array + Map/Set bigint, 582 scenarios): **582/0**;
  result keys identical to A1's join on the same scenarios.
- **B3 `-ESBigInt-`** (full affected selection): **198/3,237 — 0/0 flips
  vs A2**: the disable flag restores the previous default behavior
  exactly.
- Runtime probes: default `typeof BigInt === "function"`, `typeof 1n ===
  "bigint"`, `2n**64n/3n`, Map identity, BigInt64Array round-trip,
  DataView.setBigInt64; `-ESBigInt-` → all globals `undefined`, `1n` is
  `SyntaxError`.
- Focused rlexe (`test/es7` + `test/BigInt`, `--variants=disable_jit`):
  **103/103** default flags on test and debug-static; also green with
  `-ESBigInt`, `-nodeferparse`, `-forcedeferparse` extra flags except a
  non-BigInt synthetic-mode artifact (`finalization-registry-errors.js`
  under duplicated `-CollectGarbage`; output byte-identical with
  `-ESBigInt-`, matches its checked-in baseline). Defer/GC are exercised
  by the registered `-CollectGarbage`/defer entries and the B-runs.
- Regression: **2027/0 test, 2079/0 debug-static**
  (`check_regression.py --expected-passed` override; `gate_counts.json`
  pins 2026/2078 must be bumped by the coordinator — consumers follow
  per its `_lineage`).
- `zig build check` / `check -Dprofile=debug-static` on the final tree
  (re-verified after the repair): the regression steps fail **only**
  against the un-bumped pins (expected 2026/2078, actual 2030/2082);
  every other step passed (580/583 and 557/560 summaries with exactly
  the two regression-step failures each).

## Coverage tests (default, serialized, disabled, debugger)

- `test/es7/bigint-default.js` (default flags, no `-ESBigInt`): 7 checks —
  global availability, literals/conversions/stringification,
  arithmetic/bitwise/shift/comparison incl. `asIntN`/`asUintN`, Map/Set
  identity, `BigInt64Array`/`BigUint64Array` round-trips (INT64_MIN/MAX,
  >2^53, from/of), DataView BigInt accessors (endianness, ToBigInt/
  ToIndex errors), literal property names, JSON rejection, wrappers.
- `test/es7/bigint-serialized.js` (`-Serialized`): literal constants
  through the bytecode round trip — decimal/hex/octal/binary/separated/
  negative/zero/multi-limb literals, array literals, literal property
  names, arithmetic and value identity on restored constants, and a
  deferred function body whose literals restore on first call.
- `test/es7/bigint-disabled.js` (`-ESBigInt-`): no top-level BigInt
  syntax; `eval("1n")`, `Function("return 1n")`, separated/non-decimal
  forms and a literal property name all throw clean `SyntaxError`, the
  BigInt global surface is absent, and plain dynamic code still runs.
- `test/DebuggerCommon/bigint-display.js` (`-dbgbaseline`): primitive
  display (`bigint 123n` etc.), wrapper display, and evaluation
  (`decimal + 7n`, `BigInt.asUintN(8, 300n)`, `typeof 1n`).

Total new `rlexe.xml` entries in this batch: 3 es7 + 1 DebuggerCommon
(bigint-default, bigint-serialized, bigint-disabled, bigint-display).
Regression pins move **2026/2078 → 2030/2082** (test/debug);
`gate_counts.json` remains with the coordinator.

## Repair addenda (review blockers)

1. **Bytecode serialization** (`ByteCodeSerializer.cpp`): BigInt literal
   constants previously reached `PrependVarConstant`'s `default:` and
   aborted the process (`-Serialized`, `-GenerateParserStateCache`,
   `JsSerialize`). New `ctBigInt` constant kind: the writer emits the
   value's exact decimal string (leading `-` for negatives) through the
   regular string-constant table; the reader re-creates the constant with
   `FunctionBody::RecordBigIntConstant` — the same entry point the emitter
   uses — from the digits plus a trailing `n`, with the sign passed
   separately (a negative zero canonicalizes inside the factory). No
   binary BigInt limb format; existing
   cache/version checks are untouched. Cross-version protection is the
   pre-existing header version-scheme check: a cache file written by a
   different build fails that check (`InvalidByteCode`) and falls back to
   parsing from source **before any constant table is read**, so an old
   reader never reaches the new `ctBigInt` tag. (The `ReadConstantsTable`
   and `DeserializeVarArray` default cases assert rather than skip, which
   is why the version-scheme check — not unknown-tag tolerance — is the
   safety mechanism; the same holds for every past constant-kind
   addition.)
2. **Debugger primitive display** (`JavascriptBigInt.{h,cpp}`): added
   `GetDiagValueString` (exact decimal digits + trailing `n`, e.g.
   `-45n`, mirroring Symbol's override shape; one recycler allocation on
   the debug evaluation thread, the same class the base performs for
   Number) and `GetDiagTypeString` (`"bigint"`), replacing the previous
   `"bigint undefined"` value text.

## Residual / B4

- JIT: `Lower.cpp:23967` keys an object-equality lowering fast path on
  `!CONFIG_FLAG(ESBigInt)`, read when a function is JIT-compiled; with the
  flag on (the new default) the generic helper path is used, and a
  `-ESBigInt-` run re-activates the fast path — either way the choice is
  per-JIT-compile of the live flag value, so no baked-in default is
  assumed. No JIT lowering/inline-cache evidence is claimed (B4-gated,
  no-JIT runs only).
- TTD: BigInt values in time-travel recording/playback snapshots are not
  covered by TTSnapValues' typed paths; TTD BigInt recording/playback
  remains unverified (no TTD gate ran in this batch).
- `JsGetValueType` reports `JsObject` for a BigInt primitive: the public
  `JsValueType` enum predates BigInt and has no BigInt member
  (`lib/Jsrt/Jsrt.cpp` default case). Hosts must use `JsConvertValueToString`
  or engine-internal type queries.
- `tools/test262/edition-policy.json` line 33
  (`disabled_by_default_flags.ESBigInt`) is now doubly stale: the default
  it cites is flipped (`ConfigFlagsList.h:669` now `true`) and its
  registration reference `:1194` was already wrong (the FLAGR entry is at
  `:1197`). Shared C1 pin; NOT edited here — the C1 owner should move
  `BigInt` out of the disabled-flag bucket so future default-run
  BigInt-tagged failures classify as real.
- Performance: quadratic giant-decimal `ToString`, uncapped BigInt/String
  equality parsing, and (new with serialization) decimal re-encoding of
  BigInt literal constants at serialize time share the same quadratic
  converter (unchanged behavior class).


### Repair evidence

- Crash repros before the fix: `ch -Serialized` and
  `ch -GenerateParserStateCache` on `var q=1n;` both exited 134
  (`PrependVarConstant` default case -> `Throw::FatalInternalError`,
  crash report `ch-2026-10-29-142719.ips`). After the fix both run clean
  (`1 255 -3 5` round trip; parser-state hex dump; rc 0), on the test and
  debug-static binaries, including arrays and deferred function bodies.
- `test/es7/bigint-serialized.js` (`-Serialized`): **pass** on both
  profiles; `test/es7/bigint-disabled.js` (`-ESBigInt-`): **pass** on
  both; `test/DebuggerCommon/bigint-display.js` (`-debuglaunch`,
  `-dbgbaseline`): **pass** on both with a machine-generated CRLF
  baseline showing `decimal: "bigint 123n"`, `negative: "bigint -45n"`,
  `zero: "bigint 0n"`, `multiLimb: "bigint <large string>"`, the wrapper
  expansion (`BigInt {...}`), and evaluations `decimal + 7n ->
  "bigint 130n"`, `BigInt.asUintN(8, 300n) -> "bigint 44n"`,
  `typeof 1n -> "string bigint"`.
- Focused matrix (`test/es7` + `test/BigInt`, `--variants=disable_jit`):
  default **105/105** on both profiles; also green with `-ESBigInt`,
  `-nodeferparse`, `-forcedeferparse` (one non-BigInt synthetic-mode
  artifact, `finalization-registry-errors.js` under duplicated
  `-CollectGarbage`, byte-identical output with `-ESBigInt-`). Extra-flag
  `-Serialized` mode fails six pre-existing non-Serialized tests
  (deferparseclass, eval-super-early x3, PromiseRejectionTracking,
  finalization-registry-errors) that have no `-Serialized` registration —
  assertion/baseline mismatches, not crashes.
- No-literal-source-crash sweep: every `test/es7/bigint*.js`,
  `s6-bigint-atomics.js` and `test/BigInt/*.js` file run under
  `-Serialized`, `-GenerateParserStateCache` and plain modes on the test
  binary with zero signal exits, and under `-Serialized` on debug-static
  with two exceptions that are a pre-existing non-BigInt host assert:
  `WScript.LoadScript(..., 'samethread')` cross-context marshalling under
  `-Serialized` asserts at `JsrtHelper.cpp:54` on debug builds —
  reproduced with `var val = 5;` and no BigInt content.
- Full regression after the repair: **2030/0 test** and **2082/0
  debug-static** (`check_regression.py --expected-passed` overrides;
  +4 entries over the 2026/2078 base pins).

### Final post-repair joins (repaired binaries: test launcher `ch`
`89e5833d…` — unchanged, it loads the engine dynamically — test engine
`libChakraCore.dylib` `fec27ca2…`, debug-static `ch` `87a6c88e…`)

- **Final default (B1c) vs prior flag-on (A1), 3,435 scenarios:
  0 fail→pass / 0 pass→fail** — per-scenario identical, so the repair
  changed no engine-observable result.
- **Final `-ESBigInt-` (B3b) vs prior default-off (A2): 0/0 flips** —
  the disable flag still restores the previous default exactly.
- **Final default (B1c) vs prior default-off (A2): 2,333 fail→pass /
  0 pass→fail** — unchanged from the pre-repair measurement.

### Serialization verification scope and pre-existing (non-S2) issues

Only `-Serialized` is a registered gate entry
(`test/es7/bigint-serialized.js`). The parser-state cache pair was
manually verified on both binaries but is not pinned:
`-GenerateParserStateCache` (rc 0, state dump) and
`-UseParserStateCache -ParserStateCache -Force:DeferParse` (literal
probe prints `1 255 -3 5`; the full `bigint-serialized.js` content
passes 3/3) on the test and debug-static builds — ch generates the
parser-state buffer in memory, so both flags exercise the same
constant-serialization code path.

Two **pre-existing** serialization issues are recorded as follow-ups,
not S2 regressions:

- A truncated/corrupt buffer handed to `JsRunSerialized` crashes rather
  than failing cleanly: the deserializer's read helpers do not
  bounds-check against the buffer end, a class of defect that predates
  `ctBigInt` (any malformed constant table can over-read). A bounds
  audit of the reader is the follow-up.
- `-Serialized` + cross-context `WScript.LoadScript(..., 'samethread')`
  asserts on debug builds at `JsrtHelper.cpp:54`
  (`originalJsrtContext` switch check), reproduced with `var val = 5;`
  and no BigInt content.
