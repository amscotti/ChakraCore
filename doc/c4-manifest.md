# C4 — Feature/result manifest (certification-grade baseline, refreshed)

Published 2026-11-02 by the `c4-certification` lane; refreshed
2026-11-05 by `c4-refresh` (`4e9f95da7`); **republished 2026-11-16 by
Stage 3B (`docs-cert`) on integrated `HEAD` (`c24266598`)** after
`7412beb56` (`ESSharedArrayBuffer=true`), B4 D1–D4, A1–A12/JITREG, and
Stage 2/3 landed. This is the published feature/result manifest required by
[plan §C4](modernization-plan.md); the gate verdict against the C4 "Done
when" text is [c4-gate.md](c4-gate.md), the raw run evidence of the
original certification is [c4-run-evidence.md](c4-run-evidence.md), and
the 2026-11-05 refresh delta reconciliation is [c4-refresh.md](c4-refresh.md).
**This manifest records a measurement; it does not claim ES2025
conformance** — 1,813 edition-selected failures remain (§2).

## 1. Engine, build and tool identity (refresh run, register `refresh-2026-11-16`)

| Field | Value |
|---|---|
| Engine commit | `c24266598c91d866b4024fdf71b82508b25f00a7` (`c24266598`; zero `lib`/`pal`/`bin` edits in Stage 3B — tree held only in-progress Stage 3B doc updates over the `c24266598` binary) |
| Declared configuration | Mac ARM64, **shared / no-JIT `test` profile** (RelWithDebInfo/Test), `aarch64-macos.26.0`, SDK 26.5, ICU 79.1 (Homebrew), Zig 0.17.0 |
| Host shell | `zig-out/bin/ch`, SHA-256 `6cf5c1d0ee027952f96646fb042bdebab307bb5910c30af4dbc9406cfac5d11f` · Engine dylib `zig-out/lib/libChakraCore.dylib` SHA-256 `4e4fd4c411f0fd350e1646aebce28afb60a6c342efbc082f4a67fe76258762a3` |
| Corpus | tc39/test262 **`2e0a56762801e275a9fdf96dc49d90ba0cddcf63`** (2026-10-08, unchanged), patterns `test/language/**`, `test/built-ins/**`, `test/annexB/**` |
| Runner | test262-harness 10.0.0 / eshost, Node v22.22.3, Python 3.14; one-command refresh via `tools/test262/refresh_baseline.py --publish` |
| Host flags | `-nonative -Test262 -CollectGarbage -NegativePhaseReport` (runner defaults; **`ESSharedArrayBuffer=true` default-on since `7412beb56`**; no extra CLI feature flags) |
| Jobs / timeout | 8 / 10,000 ms per scenario |
| Controls | **25/25** (incl. 5 marker-phase controls + the forged-marker adversarial control) |

JIT note: the Mac no-JIT gates (`2063/0` `test`, `2115/0` `debug-static`)
and the Mac ARM64 JIT+WASM gates (`check-jit`, `check-wasm` `382/382` P1)
pass on `c24266598` (§7). The full 93,141-scenario Test262 run above
measures `zig-out/bin/ch` (`-nonative`); Mac ARM64 per-batch JIT corpus
runs (`c2-jit-test262.md`) cover 17/19 C2 batches, while Linux x64 JIT
and post-D1–D4 Linux Test262 remain B4-gated.

## 2. Result totals and failure inventory

| Measure | Broad diagnostic | Edition-selected (ES2025 policy) |
|---|---:|---:|
| Scenarios | 93,141 | 80,544 |
| Passed | **79,569** | **78,731** |
| Failed | **13,572** | **1,813** |
| Rate | **85.43%** | **97.75%** |
| Excluded (post-ES2025 tags) | — | 12,597 (838 pass / 11,759 fail) |
| Skipped pre-execution (§4) | 902 | 902 |

Failure buckets — `tools/test262/analyze.py` first-match rules against
pinned `edition-policy.json` (edition-selected failures **1,813**, down from
**2,949** at `refresh-2026-11-05` — **−1,136 failures**):

| Bucket | Count (`2026-11-05` -> `2026-11-16`) | Dominant clusters (2026-11-16) |
|---|---:|---|
| disabled-flag | **636** (1,734 -> 636, **−1,098**) | `ES6RegExSymbols` **558** (unchanged: Symbol.replace 162, Symbol.split 98, Symbol.match 142, Symbol.matchAll 112, Symbol.search 56, String matchAll/replaceAll substrate); plus **78** flag-on `ESSharedArrayBuffer=true` tail scenarios (`Atomics` / `SharedArrayBuffer` / `Atomics.waitAsync` that still match `edition-policy.json`'s frozen `disabled_by_default_flags` map after `7412beb56` flipped the engine default to `true`) |
| engine-defect-on-existing | **960** (1,010 -> 960, **−50**) | RegExp property escapes 112 (S3 lead-surrogate lacunarity); dynamic-import semantics 68 (B1-1…B1-10); Function/prototype 56 (fn own-key order + anonymous `name`); Date/prototype 52; TypedArray internals 44 + ctors 4; Array/prototype 40 (incl. E11-2 unshift 6); RegExp/prototype ~34; DataView/prototype 34; JSON/parse 28; `AsyncFromSyncIteratorPrototype` 28; for-await-of 24; Promise 20; untagged 363 (down from 405) |
| required-feature-missing | **200** (202 -> 200, **−2**) | `built-ins/RegExp` accessor getters (**154**, `-ES6RegExPrototypeProperties` substrate); misc 46 (`language/module-code` string-export names 10 = B1-8, parse/eval gaps 36) |
| host | **17** (3 -> 17, **+14**) | 1 `decl-pos-import-labeled` URIError (B1-6) + **16** `Atomics/{notify,wait,waitAsync}` timeout/SIGKILL runs (10 SIGKILL + 6 `Test timed out`) newly executing under `ESSharedArrayBuffer=true` (`Atomics/wait/good-views` 2/2 now pass) |

Top edition-selected failure subdirectories (2026-11-16):
`RegExp/prototype` 580 · `language/expressions` 156 · `String/prototype` 114 ·
`RegExp/property-escapes` 112 · `language/statements` 81 · `Atomics/wait` 56 ·
`Function/prototype` 56 · `Date/prototype` 52 · `language/module-code` 47 ·
`TypedArrayConstructors/internals` 44 · `Array/prototype` 40 ·
`DataView/prototype` 34 · `RegExpStringIteratorPrototype/next` 30 ·
`JSON/parse` 28 · `Atomics/notify` 22 · `Object/prototype` 22.

## 3. Negative-phase statistics (host-reported phase)

- **8,751 negatives marker-judged** (8,652 parse + 34 resolution + 65
  runtime) — every marker at line 1 of stderr, **0 displaced** markers,
  0 natural forgeries; **0 shape suspects**.
- **8,728 passed negatives** (8,637 parse + 33 resolution + 58 runtime;
  up **+4** parse negatives over `8,724` at `refresh-2026-11-05`); **1 downgrade**
  (`Negative phase mismatch`): the post-ES2025-excluded `import-defer`
  scenario — correct: the parser still rejects `import defer` at parse
  where resolution is declared.
- 24 failing negatives (down from 28 at `refresh-2026-11-05`).
- Known host limits carried from [c1-negative-phase.md](c1-negative-phase.md)
  unchanged.

## 4. Skip / host-applicability inventory (re-verified on this run)

| Skip class | Count | Justification (unchanged) |
|---|---:|---|
| `duplicate-module-scenario` | 828 | Upstream harness generates a strict-mode scenario variant for module tests; module code is always strict, so the duplicate is skipped and the original runs once (`host-filter.js`; C1-recorded). |
| `unsupported-optional-HTMLDDA-host` | 70 | `IsHTMLDDA` tests require the optional HTML `document.all` exotic; this host defines none (`harness.js` deletes it). Optional per INTERPRETING.md. |
| `blocking-host` (CanBlockIsFalse) | 4 | Tests requiring a non-blocking main agent; the `ch` host agent can block (INTERPRETING.md agent/CanBlock). |
| *(arch / backend tag skips)* | **0 — none exist** | `host-filter.js` is the only skip source and encodes exactly the three rules above (902 = total). Platform limits surface as *failures*, never silent skips. |

## 5. ES2025-required features vs the default configuration

**(a) Default-enabled, with probe evidence.** All nine ES2025 additions
plus `SharedArrayBuffer` and `Atomics` (`ESSharedArrayBuffer=true`, flipped
default-on in `7412beb56`, `lib/Common/ConfigFlagsList.h:712`) are now
enabled by default.

**(b) Flag-only in the default configuration** (the remaining
off-by-default flags in `ConfigFlagsList.h:691-699,1171-1183`):

| Flag | Default state (2026-11-16) | Edition failures owed | Decision / empirical blocker status |
|---|---|---:|---|
| `ESSharedArrayBuffer` | **default `true`** (`7412beb56`) | **94** flag-on tail (78 policy-labeled `disabled-flag` + 16 `host` timeouts; down **−1,082** net from 1,176) | Enabled default-on; `SharedArrayBuffer` 912→94, `TypedArray` 812→144, `Atomics` 746→88, `Atomics.waitAsync` 202→10, `ArrayBuffer` 206→0, `DataView` 112→0, `resizable-arraybuffer` 106→4 |
| `ES6RegExSymbols` | **default `(false)`** (`ConfigFlagsList.h:697-699,1183`) | **558** | Intentionally kept `(false)` per `#40` empirical audit (`Lower.cpp` `GenerateFastReplace` `:20779` & `GenerateFastInlineStringSplitMatch` `:20859` fast-paths + 5 legacy failures) |
| `ES6RegExPrototypeProperties` | **default `(false)`** (`ConfigFlagsList.h:691-693,1174`) | **154** `built-ins/RegExp` in `rfm` | Intentionally kept `(false)` per `#40` empirical audit (enabling alongside `ES6RegExSymbols` fails 5 interpreted + 5 dynapogo legacy tests) |

**(c) Remaining partials and `refresh-2026-11-05` -> `refresh-2026-11-16` movers.**
Every per-feature edition mover was monotonic (zero regressions). Key
edition failure reductions between `4e9f95da7` and `c24266598`:
`SharedArrayBuffer` 912→94 (−818) · `TypedArray` 812→144 (−668) ·
`Atomics` 746→88 (−658) · `BigInt` 284→51 (−233) · `Symbol` 317→91 (−226) ·
`ArrayBuffer` 206→0 (−206) · `Atomics.waitAsync` 202→10 (−192) ·
`arrow-function` 192→32 (−160) · `DataView` 112→0 (−112) ·
`resizable-arraybuffer` 106→4 (−102) · `async-functions` 102→10 (−92) ·
`Symbol.toPrimitive` 92→16 (−76) · `Reflect.construct` 68→24 (−44) ·
`untagged` 405→363 (−42) · `computed-property-names` 44→6 (−38) ·
`Reflect` 94→74 (−20) · `Symbol.species` 88→70 (−18) ·
`Int8Array` 10→0 · `Float32Array`/`Float64Array`/`Uint16Array`/`UintClampedArray`/`arraybuffer-transfer` 8→0 ·
`destructuring-binding` 64→54 (−10) · `Uint8Array` 10→2 (−8) ·
`Proxy` 100→94 (−6) · `align-detached-buffer-semantics-with-web-reality` 28→22 (−6) ·
`cross-realm` 35→29 (−6) · `Symbol.toStringTag` 19→15 (−4) ·
`Array.prototype.includes` 2→0 · `let` 4→3 · `top-level-await` 6→5.

## 6. Gate-relevant deltas vs prior certified runs

| Run | Date | Engine | Broad (P/F) | Edition (P/F) | Edition Rate |
|---|---|---|---:|---:|---:|
| `c1-baseline` | 2026-10-30 | `b490dc6de` | 61,565 / 31,576 | 60,896 / 19,648 | 75.61% |
| `c1-negative-phase` | 2026-11-01 | `b490dc6de+c1` | 77,257 / 15,884 | 76,453 / 4,091 | 94.92% |
| `c4-cert` | 2026-11-02 | `ba796f74f` | 77,268 / 15,873 | 76,464 / 4,080 | 94.93% |
| `refresh-2026-11-05` | 2026-11-05 | `4e9f95da7` | 78,399 / 14,742 | 77,595 / 2,949 | 96.34% |
| **`refresh-2026-11-16`** | **2026-11-16** | **`c24266598`** | **79,569 / 13,572** | **78,731 / 1,813** | **97.75%** |

Deltas `refresh-2026-11-05` -> `refresh-2026-11-16` (`4e9f95da7..c24266598`):
**+1,136 passed / −1,136 failed** edition-selected (**+1,170 / −1,170**
broad, incl. +34 post-ES2025 passes); buckets: `disabled-flag` 1,734→636
(−1,098), `engine-defect-on-existing` 1,010→960 (−50),
`required-feature-missing` 202→200 (−2), `host` 3→17 (+14). Primary
drivers: `7412beb56` (`ESSharedArrayBuffer=true`), B4 D1–D4 (exact Number
formatting/radix), A1–A12/JITREG (A1 template/string escapes, A9 exact
BigInt Intl formatting, A11 negative `lastIndex` + Unicode 9.0 case-fold,
A12 cross-module lexical TDZ), and Stage 2/3 fixes (`35152953b`,
`50570a36e`, `98c81ec88`, `20ea96941`). Note: `results_retained=false` in
`tools/test262/baseline-register.json` because temporary `out/test262-refresh-20261116/`
was removed after extracting totals and bucket breakdowns.

## 7. Regression / bytecode / native checks (`c24266598`)

- `test` (`zig build check libChakraNodeApi ch-nodeapi check-nodeapi`):
  **PASS** — `check_regression` **2063/0**, `check_native` (`test-shared-basic` +
  Phase-3 JsRT tests) PASS, `check_smoke` PASS, `check_metadata` PASS,
  `verify_bytecode_pure` PASS, `check_nodeapi` PASS (`150` symbols), `check-selftest` PASS.
- `debug-static` (`zig build -Dprofile=debug-static check`): **PASS** —
  `check_regression` **2115/0**, `check_native` **8/8** static suites PASS,
  `check_smoke` PASS, `check_metadata` PASS (`526` archive members), `verify_bytecode_pure` PASS.
- `release-shared` (`zig build -Dprofile=release-shared check ch-nodeapi`): **PASS**.
- `jit` + `wasm` (`zig build -Dprofile=jit -Dwasm=true check check-wasm libChakraNodeApi ch-nodeapi check-nodeapi`):
  **PASS** — `check_jit` PASS, `check_wasm_p1` **382/382 P1 PASS** (`runtests`
  438 passed / 15 non-P1 failed).

## 8. What this manifest does and does not claim

It certifies the *measurement*: a pinned, control-qualified,
negative-phase-proven full-corpus result on the declared Mac no-JIT
configuration (`c24266598`), with every skip justified and deltas
explained (`94.93%` -> `96.34%` -> **`97.75%`**, `4,080` -> `2,949` ->
**`1,813`**). It does **not** claim the C4 gate: the edition-selected
suite is not green (`1,813` failures remain), and the two RegExp
feature surfaces (`ES6RegExSymbols`, `ES6RegExPrototypeProperties`) remain
off by default. The clause-by-clause grading and ranked path to green are
in [c4-gate.md](c4-gate.md).
