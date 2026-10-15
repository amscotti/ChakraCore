//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

# C2 flag audits: S2 BigInt, S6 SharedArrayBuffer/Atomics, S7 RegExp symbols

Status: measurement-only batch. No engine, test, gate, or build change; the
regression gates stay 1967/0 (test) and 2019/0 (debug-static). This document
records the enabled-flag runs the S2/S6/S7 rows of
[c1-triage.md](c1-triage.md) asked for, and updates those rows with measured
numbers. Every number below is observed on the recorded runs; nothing is
inferred from tag counts alone.

## Provenance

- Binary: `zig-out/bin/ch`, sha256
  `29570299c3993dec6d0419da3447d325dfc7879d4320e28907cc56c2446fbd8c`
  (test profile, tree `d46223bfb`, clean).
- Corpus: pin `2e0a56762801e275a9fdf96dc49d90ba0cddcf63` (`test262.lock.json`).
- Host: `mise exec -- python3 tools/test262/run.py` (Python 3.12.13, Node
  v26.5.0), default `--jobs 8 --timeout 10000`. Controls passed 18/18 on
  every run; exit code 1 is the runner's conformance failure.
- Comparison baselines: the recorded C1 default-flags run `out/test262-zig`
  (binary `f2817d95…`, 93,141 scenarios) plus, for each audit, a fresh
  default-flags run on the *same binary* (the `-base` artifacts). Joins are
  per scenario on `(file, scenario)`. The same-binary join isolates the flag
  effect; the C1 join additionally shows drift from landed C2 batches.
- Glob note: the runner's `DIR/**/*.js` includes direct children of `DIR`;
  every join here uses that semantics (a session helper script, not
  committed; a plain per-scenario dict join on `(file, scenario)` with the
  semantics below reproduces it). An earlier
  fnmatch-based count in this session missed direct children (21,395 vs
  21,440 for S2); all numbers below are from the corrected joins.
- Known flake: `test/language/expressions/dynamic-import/**` churns ±35–39
  scenarios between identical-config runs (module-loader timing, zero
  BigInt/SAB/RegExp content). Those flips are excluded from flag-effect
  attribution and called out where they appear.

Artifacts (all under ignored `out/`): `test262-audit-bigint`,
`test262-audit-bigint-base`, `test262-audit-atomics`,
`test262-audit-atomics-base`, `test262-audit-resym`,
`test262-audit-resym-base`, `test262-audit-reproto`,
`test262-audit-reproto-sym`, `test262-audit-rsi`. Total runner wall time
553 s (session-recorded only: no artifact carries a wall-clock field, so
the total is unverified).

## S2: `-ESBigInt`

Commands (190 s and 189 s wall):

```text
mise exec -- python3 tools/test262/run.py --binary $PWD/zig-out/bin/ch \
  --output out/test262-audit-bigint \
  --host-flags='-nonative -Test262 -CollectGarbage -ESBigInt' \
  'test/built-ins/BigInt/**/*.js' 'test/language/expressions/**/*.js'
# same with --output out/test262-audit-bigint-base and default host flags
```

| Run | Passed | Failed | Scenarios | Files |
| --- | --- | --- | --- | --- |
| Default flags (same binary) | 15,641 | 5,799 | 21,440 | 11,179 |
| `+ -ESBigInt` | 15,731 | 5,709 | 21,440 | 11,179 |

`analyze.py` (flag run): edition-selected 15,287/5,397 of 20,684 (756
post-ES2025 excluded); buckets required-feature-missing 4,203,
engine-defect-on-existing 559, host 339, post-ES2025 312, disabled-flag 296
(the bucket is tag-implicated and ignores that the flag is on). Per-feature:
`BigInt` 82 passed / 374 failed.

Joins (21,440 common scenarios):

| Comparison | fail→pass | pass→fail | still-fail | still-pass |
| --- | --- | --- | --- | --- |
| vs C1 baseline `out/test262-zig` | 249 | 34 | 5,675 | 15,482 |
| vs same-binary default run | **125** | **35** | 5,674 | 15,606 |

- The 249 C1 movers decompose by tag (dir fallback for the untagged) as
  optional chaining 50 + logical assignment 80 (the S4 batch; tag count)
  + dynamic-import 35 + BigInt-tagged 82 + destructuring-assignment 4.
  Those tag-counts sum to 251 because 2 scenarios
  (`lgcl-nullish-assignment-operator-bigint.js`, default/strict) are
  co-tagged BigInt × logical-assignment and counted once in the 82, so
  249 unique movers.
- The flag-attributable movement is the same-binary **125 fail→pass:
  39 of them are dynamic-import churn and 86 are flag-attributable
  (82 BigInt-tagged + 4 destructuring-assignment)**. All 35 pass→fail
  are dynamic-import flakes (zero BigInt-tagged, zero BigInt content in
  the files).
- The 82 BigInt-tagged movers are **accidental passes on a stub**, not
  conformance: 50 scenarios are mixed-operand comparison/arithmetic files
  (`bigint-and-number`, `bigint-errors`, `bigint-wrapped-values`,
  `strict-equals/bigint-and-string`, …) whose assertions are satisfied
  because the stub throws `TypeError: Type mismatch` for mixed operands
  (probes: `0n + Symbol('1')`, `1n + 1`) or because placeholder
  strict-equality returns the expected `false`; the other 32 are
  constructor/prototype existence, `length`/`name`, and descriptor
  checks. Same-type `1n + 2n` (which the spec requires to succeed) is
  context-dependent on the stub, probed all three ways: the bare
  statement or assignment does not throw and yields the placeholder
  (`var x = 1n + 2n` → `typeof x` is `unknown`); converting it throws
  `Error: Internal error` (`print(1n + 2n)`, `String(1n + 2n)`); and
  string-concat coercion throws `TypeError: Type mismatch`
  (`"x" + (1n + 2n)`) — so the C1-era "`Internal error`" and the
  review's "`Type mismatch`" were both context-dependent claims about
  the same stub. Also probed: `typeof 10n` → `unknown`;
  `10n.toString()` → `"[object Object]"`.

### Triage open questions, answered

- **How much of the 2,447-tagged debt moves?** 82 of the edition debt, and
  even that is accidental. The C1 corpus-wide BigInt-tagged failure set is
  2,809 broad / 2,447 edition; only 456 of the broad failures lie inside
  the triage repro's own file set (the rest are in TypedArray 956,
  TypedArrayConstructors 629, Temporal 348 [post-ES2025],
  Atomics 202, DataView 92). 82 moved; 374 still fail. The out-of-set debt
  cannot move on this flag: `BigInt64Array`/`BigUint64Array` are undefined
  under it (probe), so the co-tagged TypedArray spillover the triage
  estimated at ~927 numeric-literal failures stays failing for a different
  reason (parse fixed, constructor missing).
- **Parse errors gone?** Decimal literals yes (`10n`, `-1n`, 30-digit
  values parse to a placeholder); **hex/binary/octal forms still
  SyntaxError** (`0x10n`, `0b101n`, `0o17n`): 82 scenarios. Invalid forms
  (`1.0n`, `1e10n`, `012n`) correctly still throw (negatives pass).
- **Runtime `Internal error` count?** 6 scenarios in-set report
  `Error: Internal error`: 4 logical-assignment
  (`lgcl-{and,or}-assignment-operator-bigint.js`, default/strict) and 2
  unary-minus (`unary-minus/bigint-non-primitive.js`); mixed-type
  arithmetic reports `TypeError: Type mismatch` instead, and ToString of
  the same-type placeholder (`print(1n + 2n)`, probe above) reports the
  same `Internal error`. Worse than §5 of the triage recorded: **calling `BigInt()` with
  any argument aborts the host process** (silent SIGABRT, rc 134) — 80
  in-set scenarios crash the host (constructor/coercion tests plus
  `asIntN/asUintN` fixtures that construct values).
- **Distinct remaining work items** (counts = BigInt-tagged still-fail
  scenarios, 374 total):

| Work item | Count | Example |
| --- | --- | --- |
| Real BigInt value type (literals are `unknown`-typed placeholders; ops throw) | blocks all below | `10n.toString()` → `[object Object]` |
| `BigInt()` call crashes host (SIGABRT) | 80 | `BigInt/call-value-of-when-to-string-present.js` |
| Hex/binary/octal literal grammar | 82 | `expressions/left-shift/bigint.js` |
| `BigInt.asIntN`/`asUintN` missing | 56 | `BigInt/asIntN/asIntN.js` (28 asIntN + 28 asUintN: 20 typeof + 20 missing-method calls + 8 length/name + 4 not-a-constructor + 4 hex-literal SyntaxError in `arithmetic.js`) |
| Wrapped-object/ToPrimitive semantics | 30 unique | `expressions/bitwise-and/bigint-wrapped-values.js` (24 still-fail scenarios across the 12 `bigint-wrapped-values` files + 8 BigInt-tagged still-fail scenarios whose message is `Type mismatch`, in 4 files of which `multiplication/bigint-wrapped-values.js` is already inside the 24; 24 + 8 − 2 overlap = 30) |
| Arithmetic/comparison on real values (`Internal error`/`Type mismatch` shapes) | 6 | `lgcl-{and,or}-assignment-operator-bigint.js` (4) + `unary-minus/bigint-non-primitive.js` (2), plus the probed ToString path `String(1n + 2n)` → `Internal error` |
| `BigInt.prototype` surface (toStringTag own property, radix RangeError, prop-descs) | ~10 | `BigInt/prototype/toString/radix-err.js` |
| Module/import spillover (not BigInt work) | 8 | `dynamic-import/import-attributes/*` |
| `BigInt64Array`/`BigUint64Array` (outside this file set) | 0 in-set / 1,585 corpus-wide co-tagged | `TypedArray` 956 + `TypedArrayConstructors` 629 |

- **Still C3-large?** Yes, strengthened: the flag exposes a stub with no
  BigInt value representation at all, and a host-crash class bug in the
  exposed constructor. Completion (value type, all operators, TypedArray
  integration, mixed-type JIT bailouts) remains C3-large; the 82 accidental
  movers must be re-verified against a real implementation.

## S6: `-ESSharedArrayBuffer`

Commands (17 s and 14 s wall): same shape as S2 with
`--host-flags='-nonative -Test262 -CollectGarbage -ESSharedArrayBuffer'`,
patterns `'test/built-ins/Atomics/**/*.js' 'test/built-ins/SharedArrayBuffer/**/*.js'`
(artifacts `test262-audit-atomics{,-base}`).

| Run | Passed | Failed | Scenarios | Files |
| --- | --- | --- | --- | --- |
| Default flags | 0 | 982 | 982 | 491 |
| `+ -ESSharedArrayBuffer` | 461 | 521 | 982 | 491 |

Edition-selected (flag run): 461/493 of 954; 28 post-ES2025 excluded
(`Atomics.pause` 10 included in that 28). Joins, both vs C1 baseline and vs
same-binary default: **fail→pass 461, pass→fail 0**, still-fail 521. The
default run passes nothing (both globals are `ReferenceError`s), so the
flag's whole measured effect is those 461.

Surface present (probe + corpus): `SharedArrayBuffer` constructor, `byteLength`,
`slice`, `@@species`, `@@toStringTag` all work. **Growable SAB absent**:
`growable`, `grow`, `maxByteLength` (accessor and option) all missing; the
`maxByteLength` constructor option is silently ignored (no RangeError).
`Atomics` present with `store/load/exchange/compareExchange/add/sub/and/or/
xor/isLockFree/wait/notify`; **`waitAsync` absent** (`typeof` undefined,
`Atomics.waitAsync` tag 0/202); `Atomics.pause` absent (post-ES2025 anyway).

Failure classification (521 flag-run failures):

| Bucket | Count | Class | Example |
| --- | --- | --- | --- |
| BigInt literal parse / `BigInt64Array` undefined | 188 | S2 dependency | `Atomics/add/bigint/bad-range.js` |
| `Atomics.waitAsync` missing | 202 (tag count; 128 are `typeof` probes, the rest behavior tests co-tagged with BigInt etc.) | engine | `Atomics/waitAsync/bad-range.js` |
| Growable SAB surface (`growable`/`grow`/`maxByteLength`) | ~64 | engine | `SharedArrayBuffer/prototype/growable/invoked-as-func.js` |
| Atomics on **non-shared** buffers throws `SharedArrayBuffer object expected` | 24 | engine (ES2020 semantics) | `Atomics/add/non-shared-bufferdata.js` |
| Validation order RangeError-vs-TypeError / count-eval order | 16 | engine | `Atomics/notify/retrieve-length-before-index-coercion-non-shared.js` |
| Timing assertions (`lapse >= TIMEOUT`) | 18 | mixed engine/agent | `Atomics/wait/no-spurious-wakeup-no-operation.js` |
| Notify-count assertions (need real waiters) | ≈22 | agent-dependent | `Atomics/notify/notify-all.js` |
| SIGKILL (10 s timeout, blocked host) | 6 | host/agent | `Atomics/notify/undefined-index-defaults-to-zero.js` |
| Harness arg-factory decline (immutable/non-shared buffer) | 18 | host adaptation | `Atomics/add/immutable-buffer.js` |
| `Atomics.pause` | 10 | post-ES2025 | `Atomics/pause/length.js` |

Buckets overlap through co-tags (a waitAsync test can carry a BigInt
literal); they do not sum to 521. Recount methods for the rows above:
non-shared 24 = every still-fail scenario whose message is
`TypeError: SharedArrayBuffer object expected` (18 with a "Testing
with …" prefix + 6 plain); growable ~64 = 66 still-fail scenarios under
`prototype/{grow,growable,maxByteLength}/` minus 2 BigInt-literal
spillover in `grow/this-is-not-object.js` (tilde because other
co-failures can contribute).

- **Agent-host answer:** `$262.agent` works in this harness (eshost chakra
  agent, child processes). 112 files / 224 scenarios reference it; 33 pass.
  Truly agent-shaped failures (notify counts, wake timing, SIGKILL
  timeouts) are ≈44 of 521 — the large majority of failures are engine
  feature gaps even inside agent files.
- **TypeError/ReferenceError shapes:** with the flag off both globals are
  `ReferenceError`s (982/982 fail). With it on, wrong-input shapes are
  TypeErrors (`Atomics function called with invalid typed array object`),
  OOB is `RangeError: Access index is out of range`, and non-shared
  buffers throw `TypeError: SharedArrayBuffer object expected` — the
  pre-ES2020 semantics, which is an engine work item, not host debt.
- **Product/security question (data only, no decision):** defaulting SAB in
  an embedder build would turn on both globals with **zero measured
  regressions** (0 pass→fail in either join) and 461 immediate edition
  passes on this file set. What a default flip does *not* get without
  further work: waitAsync (202 scenarios), growable buffers (~64),
  non-shared Atomics (~58 = 24 TypeError + 16 validation-order + 18
  arg-factory declines). The security tradeoff (SAB-enabled Spectre
  threat model for embedders) is a product decision outside this audit.

## S7: `-ES6RegExSymbols` and `-ES6RegExPrototypeProperties`

### (a) Symbols flag, `RegExp` + `String` (55 s / 57 s wall)

| Run | Passed | Failed | Scenarios | Files |
| --- | --- | --- | --- | --- |
| Default flags | 3,981 | 2,218 | 6,199 | 3,102 |
| `+ -ES6RegExSymbols` | 4,285 | 1,914 | 6,199 | 3,102 |

Joins: vs C1 baseline fail→pass 474 / pass→fail 42; **vs same-binary
default fail→pass 346 / pass→fail 42** (the C1 delta beyond 346+42 is
landed-batch drift, e.g. `replaceAll` and `RegExp.escape`). All 346 movers
are `Symbol.*`-tagged; per directory: `RegExp/prototype/Symbol.replace` 98,
`Symbol.match` 74, `Symbol.split` 74, `Symbol.search` 38,
`String/prototype/replaceAll` 26 (the S11-known `@@replace` dispatch
tests), String dispatch/IsRegExp 36.

The 42 regressions are two engine defect classes plus a groups-coercion
edge in the flag-on path, both main classes already suspected in the
c2-s11 receipt:

- 24 × `cstm-*-on-{string,number,boolean,...}-primitive.js` —
  `String.prototype.{match,replace,search,split}` still read `Symbol.*`
  **on primitive search values** (live probe: getter on
  `String.prototype[Symbol.replace]` runs during `replace('a','-')`).
  `replaceAll` was fixed in S11; these four were not.
- 18 more, every one failing with `Expected a TypeError to be thrown but
  no exception was thrown at all`: 16 across 8
  `RegExp/prototype/Symbol.{match,replace}` lastIndex files
  (`builtin-failure-{g,y}-` and `builtin-success-{g,y}-set-lastindex-err`,
  `g-init-lastindex-err`, `g-match-empty-set-lastindex-err`,
  `y-fail-lastindex-no-write`), plus 2 ×
  `Symbol.replace/result-coerce-groups-err.js`, a separate
  groups-coercion defect. Mechanism (probe): the failed-match `lastIndex`
  reset bypasses the observable `[[Set]]` — a writable `lastIndex` *is*
  reset to 0, but an accessor or non-writable `lastIndex` never throws
  the expected TypeError. None involve `@@matchAll`.

Per-surface state under the flag (edition; C1 in-set fail → flag-on fail):

| Surface | C1 in-set fail | Flag-on pass/fail | Remaining blockers |
| --- | --- | --- | --- |
| `Symbol.match` | 142 | 114/52 | lastIndex reset (20), flags/unicode getter order (18), primitive divergence (24 shared), v/u-flag parse (S3/S9), u-empty-advance (4) |
| `Symbol.replace` | 170 | 132/52 | primitive divergence, `coerce-unicode` surrogate handling (4), named groups (S9, 6) |
| `Symbol.split` | 94 | 84/20 | mostly S3/S9 parse spillover (`Syntax error in regular expression` 18) |
| `Symbol.search` | 56 | 48/20 | getter/order details + primitive divergence |
| `Symbol.matchAll` | 76 | 10/76 | **feature absent; see (b)/(c)** |

Scope note: the per-surface "C1 in-set fail" column counts
`Symbol.*`-tagged scenarios inside this audit's 6,199-scenario file
universe that failed in the C1 baseline run (`out/test262-zig`); a
scenario carrying two surface tags (10 in-set do) counts in both rows.

`analyze.py` edition buckets (flag run): required-feature-missing 1,596,
disabled-flag 220, engine-defect-on-existing 98; the set excludes nothing
post-ES2025. The dominant top messages are S3/S9 debt, not S7
(`\p{…}` escapes 886, `Syntax error in regular expression` 264,
`Unexpected quantifier` [named groups] 258).

### (b) `ES6RegExPrototypeProperties` before/after (15 s each)

Subset: `RegExp/prototype/**` plus
`String/prototype/{match,matchAll,replace,search,split}/**` — 1,558
scenarios, 781 files. This flag installs the ES6 prototype getters
(`global`, `ignoreCase`, `multiline`, `options`, `source`, `flags`;
`sticky`/`unicode`/`dotAll` stay behind their own flags) and drops the
legacy own-instance built-ins (`JavascriptRegularExpression.cpp:1330-1345`,
`JavascriptLibrary.cpp:4353+`); it has no feature tag, hence the compare.

| Run | Passed | Failed | fail→pass | pass→fail |
| --- | --- | --- | --- | --- |
| Default flags (subset of the (a) base run) | 872 | 686 | — | — |
| `+ -ES6RegExPrototypeProperties` | 994 | 564 | **+122** | **0** |
| `+ -ES6RegExSymbols` (no proto) | 1,132 | 426 | **+302** | −42 (the (a) regressions) |
| `+ both flags` | 1,278 | 280 | **+146 over symbols alone** | **0** |

Row arithmetic closes: 302 (default→symbols) + 146 (symbols→both) = 448
= default→both fail→pass, consistent with the pass columns (1,278 − 872
= 406 net passes = 448 movers − the 42 (a) regressions, which stay
failing under both flags).

The two flags are complementary and individually non-regressing. The proto
flag fixes the 124 `Unable to get property 'get' of undefined or null
reference` failures (descriptor tests on `RegExp.prototype.flags` and
friends) and the 12 `Cannot redefine non-configurable property 'global'`
failures (own-instance built-ins were non-configurable). What still fails
on the subset with **both** flags (280; 182 `Symbol.*`-tagged): matchAll
absence ~70, primitive divergence 24, lastIndex reset 20,
flags/unicode coercion order 18, `hasIndices`/`unicodeSets` getters 20
(S9/S3 flags), v/u parse 10, BigInt literals 10, assorted semantics 10.

### (c) matchAll supplemental (1 s)

`test/built-ins/RegExpStringIteratorPrototype/**` +
`test/built-ins/Symbol/matchAll/**` with `-ES6RegExSymbols`:
**2 passed / 36 failed**; 0 movers vs C1. `Symbol.matchAll` is `undefined`
under every flag combination probed (default, symbols, proto, both).

- **Is `String.prototype.matchAll` implementable on the enabled flag set
  now?** No. The corpus needs `Symbol.matchAll` (well-known symbol),
  `RegExp.prototype[@@matchAll]`, the RegExp String Iterator
  (`%RegExpStringIteratorPrototype%.next`/`@@toStringTag`), and
  `String.prototype.matchAll` itself — all four are absent, and none is
  provided by any existing `ConfigFlagsList` entry. It is a new-surface
  library batch (S10/S11 shape), most of whose corpus additionally needs
  the (a) fixes (lastIndex reset, `flags` getter from (b)) to go green:
  of the 76 in-set `Symbol.matchAll` failures, ~46 are
  `RegExp/prototype/Symbol.matchAll` behavior tests that assume a working
  `@@match`/species/flags substrate, not just a new method.

## Recommendations (measured value / risk ranking)

1. **S7 → `matchAll` + the two flag-on defect classes as a C2-small
   batch.** What was measured is that the protocol machinery passes most
   non-parse-dependent tests with the flags on — 346 same-binary
   fail→pass for `-ES6RegExSymbols` (against the 42 flag-on regressions
   above), +122/0 for the proto flag alone and +146/0 on top of symbols —
   not that the cluster goes green after two fixes. Of the 182
   `Symbol.*`-tagged in-set failures that remain with both flags on,
   ≈114–132 are directly addressable by this batch (matchAll ~70 +
   primitive divergence 24 + lastIndex reset 20, +18 if the
   flags/unicode-order defects are included); the remaining ~50 are
   S3/S9/S2 spillover. The `ConfigFlagsList.h:1180-1182` JIT-inlining
   warning still gates any default flip (B4), and the flags themselves
   stay off by default until then.
2. **S6 default-flip decision for the embedder profile (product call), then
   a C2-small completion slice** (waitAsync 202, growable SAB ~64,
   non-shared Atomics ~58 in that order of measured debt). Zero measured
   regressions today; the security question is not a data question.
3. **S2 stays C3-large and gains a safety flag:** before any further
   BigInt-flagged work, the silent `BigInt()` SIGABRT (80 scenarios crash
   the host) is worth a one-line defensive fix even while the feature
   stays disabled — a crash class behind a default-off flag is still a
   crash class. The remaining work is the full value-type project; the 82
   accidental movers must be re-verified against a real implementation.
4. Cross-cutting: ~188 of S6's failures and 42+ of S7's are BigInt-literal
   parses — every future enabled-flag audit on numeric-adjacent corpora
   will keep hitting S2 debt.

## Commands and hygiene (this batch)

- The nine runner commands above; all `mise exec --`, all exit 1
  (conformance failures only; controls 18/18 each).
- `git status` after all runs: only this file, `doc/c1-triage.md`, and
  `doc/modernization-plan.md` modified (plus ignored `out/` artifacts); no
  engine, test, build, or gate file touched.
- `mise exec -- zig build check-regression --summary line` on the final
  tree: `Total: passed 1967, failed 0`, no rebuild (steps cached).
- `git diff --check` — exit 0.
