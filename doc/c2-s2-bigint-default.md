# C2-S2: ESBigInt default-enablement

Status: **flipped** — `DEFAULT_CONFIG_ESBigInt` is now `true`
(`lib/Common/ConfigFlagsList.h:669`), landed on `s2-bigint-default` on top of
`52c010c64`. All evidence is Mac ARM64 no-JIT (`-nonative -Test262
-CollectGarbage` for test262); JIT is not claimed (B4). The explicit
`-ESBigInt` flag still works and `-ESBigInt-` restores the previous
default-off behavior exactly. Regression gates move **2026/2078 →
2027/2079** (one new `test/es7/rlexe.xml` entry); `gate_counts.json` was
NOT edited here — the coordinator owns the pin bump.

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
content matches /bigint/i (1,730 files → 3,435 scenarios; BigInt built-ins,
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
3. **66+6 debugger baselines**: global-object snapshots now list
   `BigInt`, `BigInt64Array`, `BigUint64Array`; snapshots that stringify
   `BigInt.prototype` show `BigInt {...}` instead of `undefined
   undefined`. Promoted machine-generated `.rebase` output with CRLF
   preserved, after a JSON-recursive diff proved the first 66 differ only
   by the three added global keys (0 removals/value changes) and the
   final 6 only by the `BigInt/prototype` value. (Same procedure as the
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
- `zig build check` / `check -Dprofile=debug-static` on the final tree:
  the regression steps fail **only** against the un-bumped pins
  (expected 2026/2078, actual 2027/2079); every other step passed
  (580/583 and 557/560 summaries with exactly the two regression-step
  failures each).

## New default-coverage test

`test/es7/bigint-default.js` (registered once, default flags, no
`-ESBigInt`): 7 checks — global availability, literals/conversions/
stringification, arithmetic/bitwise/shift/comparison incl. `asIntN`/
`asUintN`, Map/Set identity, `BigInt64Array`/`BigUint64Array` round-trips
(INT64_MIN/MAX, >2^53, from/of), DataView BigInt accessors (endianness,
ToBigInt/ToIndex errors), literal property names, JSON rejection,
wrappers. Disable behavior (`-ESBigInt-`) is proven by the B3 run and the
runtime probes above rather than a second pinned entry.

## Residual / B4

- JIT: `Lower.cpp` object-equality fast path, BigInt inline caches and
  any `Conv_Numeric` lowering claims remain B4-gated (no-JIT evidence
  only). TTD BigInt recording/playback unverified.
- `JsGetValueType` reports `JsObject` for BigInt (public API enum
  predates BigInt).
- Debugger: primitive BigInt is displayed like Symbol (type + display
  string); no debugger UI contract beyond `className "BigInt"` is claimed.
- `edition-policy.json` ESBigInt classification is stale (shared pin,
  coordinator follow-up).
- Performance: quadratic giant-decimal stringification, uncapped
  BigInt/String equality parsing (unchanged).
