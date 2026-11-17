# C4 gate verdict (measurement lane, 2026-11-02; refreshed 2026-11-05 and 2026-11-16)

The `c4-certification` lane (`2026-11-02`, `ba796f74f`) delivered the
initial measurement, and the `c4-refresh` lane (`2026-11-05`,
`4e9f95da7`) re-measured after A-E. Following `7412beb56` (which flipped
`ESSharedArrayBuffer=true` by default), B4 D1-D4, A1-A12/JITREG, and the
Stage 2/3 runtime/host/JIT fixes, `refresh-2026-11-16` (`c24266598`,
`zig-out/bin/ch`) re-measured the full corpus and updated
[baseline-register.json](../tools/test262/baseline-register.json).
**No engine source changed in this docs-cert refresh.** Evidence:
[c4-run-evidence.md](c4-run-evidence.md) (certification commands and
controls), [c4-manifest.md](c4-manifest.md) (current `refresh-2026-11-16`
manifest), and [c4-refresh.md](c4-refresh.md) (prior `2026-11-05`
reconciliation).

> Verdict (unchanged in kind, refreshed 2026-11-16): **C4 is
> BLOCKED-with-certified-baseline** -- the runner, measurement, negative
> phase preservation, and all four Mac Zig gate profiles are green
> (`25/25` controls, `8,751` marker-judged negatives, `2063/0` test,
> `2115/0` debug-static, `382/382` WASM P1, `release-shared` green),
> `SharedArrayBuffer`/`Atomics` is now **default-on** (`7412beb56`), and
> edition-selected failures dropped by **-1,136** (`2,949 -> 1,813` of
> `80,544`, **97.75%** pass rate). Two substantive blockers remain:
> (1) the edition-selected suite is not green (`1,813` failures), and
> (2) two required RegExp substrate flags (`ES6RegExSymbols` and
> `ES6RegExPrototypeProperties`) remain intentionally **`false`** by
> default due to Stage 2 empirical blockers (`Lower.cpp` fast-paths +
> 5 legacy failures). See section 3 for the re-ranked path to green.

> **Wave 3 update (`refresh-2026-11-17`, `40c2fe5be`):** **Certified ES2025 edition conformance: 98.89% (79,648 passed / 896 failed / 80,544 scenarios)**; broad conformance **86.41% (80,486 / 12,655 / 93,141)**; failure buckets post-ES2025 `11,759`, `engine-defect-on-existing` `846`, `required-feature-missing` `34`, `host` `16`; negative phase `8,741` passed, `0` shape-suspects, `1` marker downgrade (`refresh-2026-11-17`, macOS ARM64, corpus `2e0a5676`, [baseline-register.json](../tools/test262/baseline-register.json)). Both RegExp substrate flags are now **default-on** (Tier 2), so clause 2 (required features in the claimed default) is satisfied on macOS ARM64; clause 1 remains short of green by `896` edition failures (`846` engine-defect, `34` required-feature-missing, `16` host). The verdict stays **BLOCKED-with-certified-baseline** pending those failures and Linux revalidation. The 2026-11-16 text below is retained as history.

## 1. C4 "Done when", clause by clause

Plan §4.1 C4: *"the qualified, pinned required ES2025 suite is green on
the declared supported engine configurations, required features are
enabled in the claimed default configuration, and all skips/host
applicability are justified. Preserve negative-phase validation,
regression/bytecode/native checks and a published feature/result
manifest."*

| # | Clause | Grade | Evidence (`refresh-2026-11-16` at `c24266598`) |
|---|---|---|---|
| 1 | Qualified, pinned required ES2025 suite **green** on declared supported configurations | **NO** | Pinned corpus `2e0a567628`, qualified runner, controls `25/25`, zero feature regressions -- but edition-selected **1,813 failures** of `80,544` (**78,731 pass, 97.75%**): `disabled-flag` `636`, `engine-defect-on-existing` `960`, `required-feature-missing` `200`, `host` `17` ([c4-manifest.md](c4-manifest.md)). Broad corpus: `79,569 / 13,572` of `93,141` (**85.43%**, down `-1,170` failures from `78,399 / 14,742`) |
| 2 | Required features enabled in the **claimed default** | **NO** | **`ESSharedArrayBuffer` (`SharedArrayBuffer` + `Atomics`) is now default-on** (`7412beb56`, `ConfigFlagsList.h:712,1192`), clearing `1,098` `disabled-flag` failures vs `2026-11-05`. The two RegExp substrate flags remain default **`false`**: `ES6RegExSymbols` (`ConfigFlagsList.h:697-699,1183`; `558` `disabled-flag` tests) and `ES6RegExPrototypeProperties` (`ConfigFlagsList.h:691-693,1174`; `154` `built-ins/RegExp` `required-feature-missing` failures). An empirical Stage 2 audit found JIT fast-paths (`GenerateFastReplace` at `Lower.cpp:20779`, `GenerateFastInlineStringSplitMatch` at `Lower.cpp:20859`) plus `5` legacy interpreted / `5` dynapogo failures that block flipping them ON without targeted fixes |
| 3 | All skips / host applicability justified | **PASS** | `902` skips across `48,598` files, matching the three C1-recorded skip classes (`828 / 70 / 4`) re-verified on `refresh-2026-11-16` (manifest §4) |
| 4 | Preserve **negative-phase validation** | **PASS** | `8,751` marker-judged negatives preserved; controls incl. spoof control `25/25` |
| 5 | Preserve **regression/bytecode/native checks** | **PASS** | At `c24266598`: `test` `zig build check libChakraNodeApi ch-nodeapi check-nodeapi` PASS (**2063/0** regression, `verify-bytecode`, `check-native`, `check-smoke`, `check-metadata`, `check-nodeapi` `150` exports); `debug-static` PASS (**2115/0** regression, `8/8` static `check-native`, `526` archive members); `release-shared` PASS; `jit` + `wasm` PASS (`check-jit` + **382/382 P1** `check-wasm`) |
| 6 | Published **feature/result manifest** | **PASS** | [c4-manifest.md](c4-manifest.md) updated `2026-11-16` with `refresh-2026-11-16` totals, bucket deltas vs `4e9f95da7`, flag state, and registered in [baseline-register.json](../tools/test262/baseline-register.json) |

Clause 1's second axis ("declared supported engine configurations")
remains: Mac profiles are verified as above (Test262 certified on Mac
`zig-out/bin/ch`), while Linux native re-verification waits on the post-
Stage-2/3 native Linux run ([modernization-plan.md](modernization-plan.md) §6).

## 2. Why this is not CONDITIONAL

Unchanged from the certification verdict: the red clauses are
substantive, not pending-confirmation -- no future CI run turns `1,813`
failures green, and an empirical code fix is required before flipping
`ES6RegExSymbols` / `ES6RegExPrototypeProperties` to `true`. What *is*
green -- the qualified measurement itself, `SharedArrayBuffer`/`Atomics`
default-on, and `-1,136` edition failures since `2026-11-05` -- gives
C4 an exact `c24266598` baseline and an updated owed list.

## 3. Exact path to green (re-ranked with the 2026-11-16 numbers)

1. **The two remaining RegExp flag surfaces (`712` mapped edition failures):**
   - `ES6RegExSymbols` (`558` `disabled-flag` failures) and
     `ES6RegExPrototypeProperties` (`154` `built-ins/RegExp` failures in
     the `200`-test `required-feature-missing` bucket).
   - **Empirical Stage 2 audit blockers (kept `false` intentionally):**
     1. *JIT fast-paths:* `GenerateFastReplace` (`Lower.cpp:20779`) and
        `GenerateFastInlineStringSplitMatch` (`Lower.cpp:20859`) fast-path
        `VtableJavascriptRegExp` without delegating to `Symbol.replace` /
        `Symbol.split` (`DelegateToRegExSymbolFunction`), creating JIT vs.
        interpreter divergence under flag-on.
     2. *5 legacy regression failures (both interpreted and dynapogo):*
        `Regex/regexSplitOptimization.js`, `Regex/crossContext.js` (x2),
        `es6/proto_basic.js`, and `es6/RegExpApisTestWithStickyFlag.js` (#16).
   - Clearing those blockers and flipping both flags accounts for `-712`
     edition failures (`1,813 -> ~1,101`). Note: `636 - 558 = 78` remaining
     `disabled-flag` failures belong to the `ESSharedArrayBuffer=true`
     flag-on tail still classified under frozen `edition-policy.json` tag
     mappings (`+16` more appear under `host`).
2. **`engine-defect-on-existing`: `960` (`-50` from `1,010` at `2026-11-05`):**
   - Stage 2/3 changes (`Float16Array`, `TypedArray` & non-writable
     `lastIndex` guards, AB/SAB/Atomics repairs) retired `50` failures
     in this bucket with `0` feature regressions.
   - Major structural clusters carried forward from the `4e9f95da7`
     decomposition ([c4-manifest.md](c4-manifest.md) §2) remain
     decision-carded or lane-owned (proper tail calls, Unicode property
     escape residuals, dynamic-import host semantics, `Function`/
     `Date`/`TypedArray`/`DataView`/`Array`/`RegExp`/`JSON`/async-iter
     tails; see [open-cards.md](open-cards.md)).
3. **Remaining `required-feature-missing` beyond RegExp (`46` of `200`):**
   - `10` `module-code` failures + `36` misc feature/parse gaps (`-2` vs
     `2026-11-05`).
4. **`host`: `17` (`+14` from `3` at `2026-11-05`):**
   - `1` parser gap (`decl-pos-import-labeled`, card B1-6).
   - `16` `Atomics/{notify,wait,waitAsync}` failures (`10 SIGKILL` + `6`
     `Test timed out`) surfaced when `ESSharedArrayBuffer=true` unlocked
     the cross-agent Atomics corpus (`Atomics/wait/good-views.js`
     moved to `2/2` PASS; note `test/rlexe.xml` `ESSharedArrayBuffer`
     regression `S6e` also remains verified green in `zig build check`).

Zero-failure clauses 3-6 need no work; the refresh remains a single
command (`refresh_baseline.py --binary "$PWD/zig-out/bin/ch" --publish`).

## 4. Scope audit

- `c4-certification` (`2026-11-02`, `becb482b4` at `ba796f74f`): initial
  baseline (`76,464 / 4,080` edition).
- `c4-refresh` (`2026-11-05`, `537e8156a` at `4e9f95da7`): post-A-E refresh
  (`77,595 / 2,949` edition).
- `refresh-2026-11-16` (`2026-11-16`, Stage 3B `docs-cert` at `c24266598`):
  certified `zig-out/bin/ch` refresh (`78,731 / 1,813` edition, `97.75%`),
  recorded in [baseline-register.json](../tools/test262/baseline-register.json)
  and reconciled in [c4-manifest.md](c4-manifest.md) and this file.
  Raw temporary output directory `out/test262-refresh-20261116` was removed
  after recording the counts (`results_retained: false` in the register).
