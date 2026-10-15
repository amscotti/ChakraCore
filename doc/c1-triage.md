# C1 — Edition policy, runner qualification and failure triage

Recorded 2026-10-13 at HEAD `3c597e4cc` (rounds 2–3 corrections from
independent review are folded in below; the headline baseline did not
change). This completes the analysis half of
[plan milestone C1](modernization-plan.md) (qualify the runner, triage the
debt). **No engine source was changed.** Every count below comes from running
[`tools/test262/analyze.py`](../tools/test262/analyze.py) this session on the
recorded baseline data; the committed tool output is
[`tools/test262/c1-baseline-analysis.json`](../tools/test262/c1-baseline-analysis.json),
including the derived figures this document cites (passed-negatives total,
excluded passed/failed split, and the `class*` failure union, under
`derived_aggregates`).
Statements marked *(observed)* were executed or read from tool output;
*(inferred)* marks interpretation.

Inputs (all pre-existing, uncommitted under `out/`):

| Input | Value |
|---|---|
| Recorded results | `out/test262-zig/results.json` (93,141 scenarios) + `summary.json` |
| Corpus | tc39/test262 `2e0a56762801e275a9fdf96dc49d90ba0cddcf63`, patterns `test/language/**`, `test/built-ins/**`, `test/annexB/**` |
| Recorded baseline binary | `ch` SHA256 `f2817d95…a0e29e`, engine source `97e68c917` ([conformance-baseline.md](conformance-baseline.md)) |
| Validation binary (this session) | `zig-out/bin/ch` SHA256 `d39d5133…ae092`, dylib `dcd0d5d5…843f1` *(observed via scoped-run summary)* |

The validation binary **differs** from the recorded baseline binary, so the
headline numbers in §2 remain tied to the recorded baseline. The engine
source is nevertheless the same lineage: `git diff 97e68c917..HEAD -- lib/
pal/ bin/` is empty *(observed)*; the tree-wide changes since that commit
are build/tooling/docs files (`build.zig`, `tools/`, `doc/`, CI and root
metadata) plus `test/native-tests/` helpers — no engine source.
Controls, the scoped run and the feature probe (§3, §5) therefore validate
the *tooling and qualification* on the current binary, not a rebaselined
conformance score.

## 1. Edition policy — method and limits

Machine-readable policy:
[`tools/test262/edition-policy.json`](../tools/test262/edition-policy.json).
It classifies all **178 feature tags** appearing in the recorded results
(`summary.json` lists 179 keys; an additional key, `untagged` (run.py's
label for scenarios with no `features:` metadata), is not a corpus tag):

- **151 required-ES2025**, **27 post-ES2025-proposal**, plus 2 host-optional
  entries (`IsHTMLDDA`, `CanBlockIsFalse`) that `host-filter.js` already
  skips before execution and which therefore have zero scenarios in
  `results.json` *(observed from the policy file and analyze output)*.

**Selection rule.** A scenario is edition-selected iff *none* of its feature
tags is post-ES2025-proposal or host-optional. Untagged scenarios are
edition-selected: feature tags alone do not define the edition (plan C1),
and later semantic changes to existing builtins are often untagged, so they
stay in and their failures count against the edition score.

**Conservative rule.** Anything uncertain stays required; no required
feature is excluded on doubt. `analyze.py` enforces the same direction for
future data: a tag absent from the policy is treated as required and
reported in `unknown_feature_tags_treated_required` (empty for the recorded
baseline *(observed)*).

**Anchors.** Each exclusion carries a one-line rationale and source in the
policy file, from exactly three checkable sources:

1. The [TC39 finished-proposals list](https://github.com/tc39/proposals/blob/main/finished-proposals.md)
   cited by plan §4.2, by *expected publication year* (fetched 2026-10-13):
   ES2026 — `upsert`, `json-parse-with-source`, `iterator-sequencing`,
   `uint8array-base64`, `Math.sumPrecise`, `Error.isError`,
   `Array.fromAsync`; ES2027 — `iterator-chunking`, `iterator-includes`,
   `explicit-resource-management`,
   `Atomics.pause`, `joint-iteration`, `Temporal`. The same list confirms
   the required ES2025 set (`RegExp.escape`, `Float16Array`, `promise-try`,
   `iterator-helpers`, `json-modules`, `import-attributes`,
   `regexp-modifiers`, `set-methods`, `regexp-duplicate-named-groups`) and
   the ES2023/ES2024 sets named per-tag in the policy.
2. The pinned corpus's own `features.txt` annotations: its "Proposed
   language features" section supplies the other 14 not-yet-finished
   exclusions
   (`decorators`, `ShadowRealm`, `source-phase-imports`, `import-defer`,
   `import-text`, `import-bytes`, `immutable-arraybuffer`,
   `legacy-regexp`, `await-dictionary`, `error-stack-accessor`,
   `nonextensible-applies-to-private`, `Intl.Era-monthcode`,
   `source-phase-imports-module-source`, `Iterator.prototype.join`); its
   "Standard language features" section is a supporting anchor for the
   remaining required tags. That Standard section is itself stale: the
   13 year-based exclusions in anchor 1 (e.g. `Temporal`) are already
   listed there even though their expected publication years are
   2026/2027, so presence in the Standard section is **not** used as
   required-evidence for the year-excluded set — those 13 rest solely on
   the live TC39 finished-proposals list in anchor 1.
3. ECMA-262 edition knowledge, used only where the two sources above are
   silent and always in the required direction (e.g. `tail-call-optimization`
   is ES2015 and stays required despite wide non-implementation; Annex B
   tags stay required — claiming Annex B for non-browser embedders is a
   product decision, not a silent filter). One disclosed exception: the 52
   excluded `annexB` scenarios are all tagged `legacy-regexp` — 48
   legacy-accessor scenarios with `esid: pending` (proposal semantics, not
   the published Annex B text) plus 4 `RegExp.prototype.compile` scenarios
   whose ES2025 Annex B section exists (`esid: sec-regexp.prototype.compile`)
   but whose tests carry the `legacy-regexp` proposal tag *(observed from
   the recorded records)*. They stay excluded under the proposal rule above;
   reinstating all 52 (all currently failing) would give 60,896 / 19,700 /
   80,596 (75.56%).

**Limits.** (a) Tag granularity: a scenario tagged only with required tags,
or untagged, can still encode a post-ES2025 draft semantic; `features.txt`
asks proposal tests to carry a dedicated flag, but upstream compliance is
not verified here, so the edition-selected score is a *policy-defined*
score, not proof that every selected test is ES2025-normative. (b) Exclusion
affects only the edition-selected score; excluded scenarios remain in the
broad diagnostic and in `results.json`, so both snapshots stay comparable.
(c) Feature counts overlap — one scenario carries several tags — so
per-feature failure counts are upper bounds on what fixing that one feature
could recover, and they must never be summed.

## 2. Edition-selected baseline (tool output)

Command *(observed, 1.5 s on the 113 MB results file)*:

```sh
python3 tools/test262/analyze.py --self-test
# analyze.py self-test: OK
python3 tools/test262/analyze.py --results out/test262-zig/results.json \
  --summary out/test262-zig/summary.json \
  --out-json tools/test262/c1-baseline-analysis.json
```

Headline output, quoted:

```text
broad: 61565 passed / 31576 failed / 93141 scenarios / 48598 files (66.10% passed)
edition_selected: 60896 passed / 19648 failed / 80544 scenarios / 42203 files (75.61% passed)
excluded_from_edition: {'scenarios': 12597, 'passed': 669, 'failed': 11928, 'post-ES2025-proposal': 12597}
skipped: {'scenarios': 902, 'reasons': {'blocking-host': 4, 'duplicate-module-scenario': 828, 'unsupported-optional-HTMLDDA-host': 70}, ...}
unknown_feature_tags_treated_required: []
failure_buckets:
  post-ES2025: 11928
  required-feature-missing: 11595
  disabled-flag: 3937
  engine-defect-on-existing: 3822
  host: 294
```

| Measure | Broad diagnostic | Edition-selected (ES2025 policy) |
|---|---:|---:|
| Scenarios | 93,141 | 80,544 |
| Unique files | 48,598 | 42,203 |
| Passed | 61,565 | 60,896 |
| Failed | 31,576 | 19,648 |
| Passed rate | 66.10% | 75.61% |
| Skipped (host filter, pre-execution) | 902 | 902 (same skips; none are in results) |

12,597 scenarios are excluded from the edition score, all via
post-ES2025-proposal tags; **669 of the excluded scenarios pass**
(tool fields `excluded_from_edition.passed` /
`derived_aggregates.excluded_passed`; also 61,565 − 60,896), so exclusion
is not a failure-hiding device *(observed)*. `Temporal` alone accounts for 9,226 broad failures at
0 passed. The edition-selected failure count decomposes exactly as
19,648 = 11,595 required-feature-missing + 3,937 disabled-flag + 3,822
engine-defect-on-existing + 294 host *(observed; bucket rules in §4)*.

This is an **edition-selected diagnostic baseline**, comparable across
future runs that keep the corpus revision, patterns, host flags and policy.
It is not an ES2025 conformance claim: the negative-phase residual gap in
§3 and the tag-granularity limit in §1 still apply, and C4 additionally
requires the default configuration to enable the features.

## 3. Negative-phase qualification — CONDITIONAL

**The gap.** The upstream validator (`test262-harness/lib/validator.js`,
read this session) accepts a negative test when the thrown error's *name*
matches `negative.type`; it never inspects `negative.phase`. A runtime
`throw new SyntaxError` therefore "passes" a parse-phase negative
*(observed: §3 probe below)*.

**What was added.**
[`tools/test262/controls/`](../tools/test262/controls/test/) gained eight
qualification scenarios (nine files including one `_FIXTURE`; 10 → 18
controls), exercised by every
`run.py` invocation and by `--controls-only`:

| Control | Phase intent | Observable evidence required by run.py |
|---|---|---|
| `pass-parse-negative-noexec.js` | parse error after `print` + `$DONOTEVALUATE()` | no stdout; tab-prefixed `\tat code (` parse frame |
| `pass-resolution-negative.js` (+ `_FIXTURE`) | missing imported binding | no stdout; loader `thrown at …:` prefix **and** empty JS stack |
| `pass-runtime-negative-eval.js` | `eval` SyntaxError at runtime | runtime (Global code) frame, not a parse frame |
| `fail-wrong-phase-parse.js` | runtime `throw new SyntaxError` vs declared parse | must be caught: stdout marker + Global code frame |
| `fail-wrong-phase-parse-codefn.js` | silent runtime throw from a function named `code` vs declared parse | must be caught: frame is space-prefixed `   at code (…)`, not the tab-prefixed parse frame |
| `fail-wrong-phase-resolution.js` | module evaluates then throws vs declared resolution | must be caught: stdout marker + Module code frame |
| `fail-wrong-phase-resolution-silent.js` | module silently evaluates then throws vs declared resolution | must be caught: Module code frame (no stdout marker to lean on) |
| `fail-wrong-phase-runtime.js` | parse-time SyntaxError vs declared runtime | must be caught: tab-prefixed parse frame |

`run.py` now downgrades a harness pass for *controls* whose recorded
evidence (`phase_evidence`) contradicts the declared phase: parse requires
no stdout and a parse/loader shape, resolution requires no stdout and the
loader prefix together with an empty JS stack (an empty stack alone is no
evidence, and a loader prefix accompanied by a JS stack is classified by
its frame), runtime requires a non-parse frame. The parse frame is the
tab-prefixed `\tat code (` spelling only — a runtime throw from a function
literally named `code` frames as space-prefixed `   at code (…)` and does
not match (the pre-round-2 substring rule accepted it; the new
`fail-wrong-phase-parse-codefn.js` control is the regression fixture, and
replaying the old rule over its recorded control output returns
parse-compatible *(observed)*). Before this check existed, the fixtures
demonstrated the gap on the current binary *(observed)*:

```text
ABORT: control fail-wrong-phase-parse.js produced the wrong outcome: {'pass': True}
```

After the check, with the exact commanded invocation *(observed)*:

```text
$ python3 tools/test262/run.py --binary zig-out/bin/ch --controls-only
Running controls: ['test/*.js']
Controls passed: 18 scenarios (including deliberate failures)
```

**Recorded-data audit.** `analyze.py` applies the same shape heuristic to
the 8,399 passed negatives in the recorded baseline (tool fields
`negative_phase_audit.passed_negatives_total` /
`derived_aggregates.passed_negatives_total`) *(observed)*:

```text
negative_phase_audit (passed negatives / suspects):
  parse: 8315 passed, 0 suspects {}
  resolution: 32 passed, 12 suspects {'parse-style-frame-for-resolution': 12}
  runtime: 52 passed, 0 suspects {}
  passed_negatives_total: 8399
```

The tightened round-2 rules (tab-anchored parse frame; loader prefix must
come with an empty stack) reproduce this audit exactly (the round-2
regenerated `c1-baseline-analysis.json` was byte-identical to round 1;
the current committed copy additionally carries the full 12-record
suspect list and the `derived_aggregates` fields, with every count
unchanged) *(observed)*:
all 8,315 passed parse negatives already carry the tab-prefixed frame, and
no loader-channel record in the baseline carries a JS stack. All 12
suspects are individually listed in the committed JSON under
`negative_phase_audit.resolution.suspect_records`, including the
excluded `import-defer` case below.

The 12 resolution suspects are **true wrong-phase passes**, not link
errors surfacing through the parser path: their records show the parser
rejecting import-attribute syntax itself — `SyntaxError: Expected ';'`
at the `with` clause (10 cases, e.g.
`test/language/import/import-attributes/json-invalid.js`), one
`'with' statements are not allowed in strict mode`, and one `import-defer`
module-syntax error — each with the tab-prefixed parse frame and no loader
prefix *(observed from the records)*. The upstream validator accepted them
because the error *name* matches. They remain counted as harness-passed in
the recorded baseline by design (the audit reports, it does not re-judge
corpus scenarios), so the headline in §2 is unchanged; as a sensitivity
figure, downgrading the 11 edition-selected suspects (the 12th is the
excluded `import-defer` case) would give 60,885 / 19,659 (75.59%).

**Residual gap — why this is CONDITIONAL, not closed.** (i) The shape
heuristic is host-specific (frame spellings and the `thrown at` loader
prefix come from `ch`/eshost, not from the spec) and only downgrades
*controls*; corpus scenarios are audited after the fact, not re-judged.
(ii) Some wrong-phase passes are indistinguishable in `rawResult`: a
runtime *instantiation* SyntaxError (e.g. the global lexical
redeclaration errors visible among passed parse negatives in the recorded
data) can be reported with a parse-style frame, so the audit's 0 parse suspects
is a floor on suspicion, not proof of phase correctness. (iii) On the
resolution channel, what remains indistinguishable is exactly the class
of errors the host reports *only* through the loader — `thrown at <file>:`
with no JS stack. A module that evaluates and then throws is **not** in
that class (it carries a Module code frame; the silent-masquerade control
above is caught), but any error raised while the host loads, parses or
instantiates the module graph — including early errors inside dependency
modules, whose recorded loader-channel messages include texts such as
`Can't have 'break' outside of loop` alongside genuine link errors like
`Module import x cannot be resolved` *(observed)* — is indistinguishable
from a resolution error in `rawResult`, whatever its spec phase. Closing
(ii)/(iii) requires the host to report the phase explicitly (an
engine/host change, out of C1 scope). **Do not certify a full edition
result on phase grounds until that exists**; C1 qualifies the runner to
*detect the demonstrated masquerades* and to audit recorded data, which is
what the controls above prove.

**Residual harness limitation — fixture-source cache (fixed after the
baseline was recorded).** eshost's `lib/dependencies.js` caches
module/fixture source by *basename only* in a process-wide map that
upstream never clears, so within one run a later test whose fixture shares
a basename with an earlier test's fixture deterministically receives the
first test's source *(observed from the installed source and the runs
below)*. The selected trees contain **18 duplicate `_FIXTURE` basenames
with differing contents**, and **888 recorded scenarios (478
edition-selected)** sit in test files mentioning them — that is the
exposure ceiling for the recorded baseline, whose run order is lost, so
the actually-affected subset of §2 and its direction are unknown
*(observed counts; subset inferred unknowable)*. Measured on the current
binary over exactly the 30 affected directories (2,365 scenarios,
alone-per-directory vs one combined run, `--jobs=1` so the cache effect is
deterministic): **2 flips**, both pass-alone → fail-combined
(`language/import/import-defer/syntax/valid-default-binding-named-defer.js`
and `language/module-code/namespace/internals/get-nested-namespace-dflt-skip.js`,
both recorded as failed in the baseline and passing alone on the current
binary) *(observed)*. `tools/test262/harness.js` now calls upstream's
exported-but-uncalled `rawSource.clear()` before each scenario (safe under
the agent pool because eshost reads the cache entirely within
`evalScript`'s synchronous prefix); after the fix the same measurement
shows **0 flips**, controls stay 18/18 and the §5 scoped regexp run still
reproduces 456/24 *(observed)*. The fix changes nothing in `analyze.py`
(which operates on the recorded data; its rerun is byte-identical), and
the §2 baseline was recorded pre-fix, so rerunning the affected subsets is
still owed (§8). At the default `--jobs=8` the same alone-vs-combined
comparison flips ~79 scenarios with unstable membership before *and*
after the cache fix, and jobs=8 alone totals (1,214/1,207 passed) sit
below the jobs=1 total (1,267) *(observed)*: that instability is not the
fixture cache. All 8 agents share one per-run temp directory in which
module tests and their fixtures are written under deterministic basenames
and removed with fire-and-forget unlinks, so concurrent agents can
overwrite or delete each other's files — the jobs=8 instability above is
consistent with that intra-run race (not isolated here from load/timeout
effects), and per-agent temp isolation does not yet exist.

## 4. Failure groups by root cause

Bucket rules (first match wins; implemented in `analyze.py`, applied to all
31,576 broad failures). Buckets are *classification by recorded signal*,
not per-test root-cause proof; the per-feature table in the committed
analysis JSON allows re-bucketing.

| Bucket | Rule (signal) | Count |
|---|---|---:|
| post-ES2025 | any post-ES2025-proposal tag | 11,928 |
| host | abnormal exit/timeout, or `URIError` naming a module path (ch host loader) | 294 |
| disabled-flag | any tag in the policy's `disabled_by_default_flags` (tag-implicated) | 3,937 |
| required-feature-missing | positive test failing with SyntaxError, `'…' is not defined`, or an undefined-probe TypeError/typeof | 11,595 |
| engine-defect-on-existing | everything else: assertions, wrong types/values, failing negatives | 3,822 |

Dominant edition-selected failure messages with their top tags (tool
fields `edition_top_failure_messages` / `edition_top_message_tags`;
message strings throughout this section are shortened for display — the
tool truncates at 100 characters and the table drops the
`Expected no error, got` prefix — full strings are in the committed
analysis JSON):

| Message (truncated by tool) | Count | Top co-occurring tags | Reading |
|---|---:|---|---|
| `got SyntaxError: Invalid character` | 6,850 | `class` 6,094; `class-methods-private` 2,673; `class-static-methods-private` 2,649 | scanner rejects `#` private names *(inferred; sampled source shows `async * #method()`)* |
| `got SyntaxError: Unexpected identifier after numeric literal` | 1,694 | `BigInt` 1,441; `TypedArray` 927 | `1n` literals with `ESBigInt` off |
| `got SyntaxError: Expected '('` | 1,198 | `class-fields-public` 1,143; `class` 955 | class field declarations rejected |
| `got ReferenceError: 'BigInt64Array' is not defined` | 1,016 | `BigInt` 734; `TypedArray` 724 | BigInt typed arrays absent |
| `invalid escape in unicode pattern` | 886 | `regexp-unicode-property-escapes` 886 | `\p{…}` unsupported |
| `got ReferenceError: 'SharedArrayBuffer' is not defined` | 564 | `SharedArrayBuffer` 512 | `ESSharedArrayBuffer` off |
| `got ReferenceError: 'Iterator' is not defined` | 480 | `iterator-helpers` 480 | `Iterator` global/helpers absent |
| `got Test262: This statement should not be evaluated` | 327 | `untagged` 159; `generators` 55 | invalid syntax was *accepted and executed* — early-error defects, not missing features |
| `got ReferenceError: 'Atomics' is not defined` | 242 | `Atomics` 242 | gated with SAB |

Two cross-cutting readings *(inferred from the clusters above)*: the
class-element and BigInt absences leak into nominally unrelated tags
(`generators` 2,174 edition failures, `destructuring-binding` 2,024,
`async-iteration` 2,075 — largely generated tests whose templates embed
class/private/BigInt syntax), so fixing the root syntax features should
recover failures far beyond their own tags; and the 327
`$DONOTEVALUATE` executions plus the untagged cluster ("An initialized
binding is not created prior to evaluation…", 160 in the defect bucket's
top messages) are defects in *existing* semantics, invisible to
feature-presence probes.

## 5. Validation on the current binary (observed)

Feature probe (`zig-out/bin/ch tools/test262/feature-probe.js` →
`FEATURE-PROBE-COMPLETE`): optional chaining, logical assignment and class
fields are SyntaxErrors; `BigInt`, `SharedArrayBuffer`,
`RegExp.prototype[Symbol.match]`, RegExp prototype `source` descriptor,
`String.prototype.matchAll/replaceAll`, `WeakRef`, `Intl.Locale` and
`Array.prototype.toSorted` are `undefined`; named groups, lookbehind,
property escapes, `d`/`v` flags and import attributes are SyntaxErrors;
WeakMap/WeakSet symbol keys throw TypeError; flat/flatMap/error-cause work.
This matches the recorded-baseline inventory, as expected for unchanged
engine source.

Default-off flag presence probes (ad-hoc scripts under `/tmp`, this
session): `-ESSharedArrayBuffer` → `function object` (both globals
appear); `-ES6RegExSymbols` → `function` for
`RegExp.prototype[Symbol.match]`; `-ESBigInt` → `function undefined` for
`typeof BigInt, typeof BigInt64Array`, and `1n + 2n` fails with
`Error: Internal error`. Presence is not conformance (plan C2); for BigInt
the flagged path is observably incomplete before any test262 run.

Scoped fresh run proving run → analyze on new data (separate output dir;
new binary, so not comparable to the headline baseline):

```text
$ python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-scoped \
    'test/language/literals/regexp/**/*.js'
Controls passed: 18 scenarios (including deliberate failures)
test262: 456 passed / 24 failed; 480 scenarios, 240 files
$ python3 tools/test262/analyze.py --results out/test262-c1-scoped/results.json \
    --summary out/test262-c1-scoped/summary.json
broad: 456 passed / 24 failed / 480 scenarios / 240 files (95.00% passed)
edition_selected: 456 passed / 24 failed / 480 scenarios / 240 files (95.00% passed)
failure_buckets:
  engine-defect-on-existing: 22
  required-feature-missing: 2
  parse: 358 passed, 0 suspects {}
```

This directory is negative-heavy (358 of 480 are passed parse negatives),
so its `regexp-named-groups 110/2` and `regexp-modifiers 166/0` lines
mostly prove invalid syntax is rejected; they do not contradict the
feature probe, which rejects the *valid* forms.

## 6. Shortlist (ordered by expected value/risk)

Conventions: *Expected gain* is the tag's edition-selected failure count
from the tool — an upper bound (§1c); overlapping tags mean the class and
BigInt items also unlock failures filed under other tags. *Repro* spells
out `--binary zig-out/bin/ch` verbatim; a batch substitutes its own build
and re-runs `analyze.py` on the new `results.json` to measure. *JIT-x64?* asks whether
the batch needs SimpleJIT/FullJIT verification beyond the standard C2
matrix (which requires B4's native x64 runner, currently BLOCKED).

**S1. Class elements: public/private fields, private methods, static
blocks.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-class 'test/language/statements/class/**/*.js' 'test/language/expressions/class/**/*.js'`.
Subsystem: Parser → ByteCodeGenerator → Runtime (brand checks, private-name
environments) + Backend. Prerequisite: split design (public fields;
private names/brands; static blocks) and bytecode regeneration per C2.
Expected gain: the union of edition failures carrying any `class*` tag
is 8,238 (tool field
`derived_aggregates.class_family_union_edition_failures`: scenarios
failing in the edition-selected set with at least one feature tag
starting with `class`) *(observed)*. The `class` tag alone accounts for 7,147, but the
subtags are **not** subsets of it (scenarios can carry e.g.
`class-fields-public` without `class`), and their counts overlap each
other, so they must not be summed: `class-fields-public` 3,517,
`class-methods-private` 2,721, `class-static-methods-private` 2,693,
`class-fields-private` 1,479, `class-static-fields-private` 653,
`class-static-fields-public` 365, `class-static-block` 72,
`class-fields-private-in` 22. Plus unblocking spillover in generated
generator/destructuring tests. Uncertainty: high — largest single debt,
touches object layout assumptions. JIT-x64? **yes** (new bytecode/IC paths
for private access). Effort: **C3-large** (plan §4 C3 names this exactly).

**S2. BigInt: enabled-flag measurement, then completeness/default.**
Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-bigint --host-flags='-nonative -Test262 -CollectGarbage -ESBigInt' 'test/built-ins/BigInt/**/*.js' 'test/language/expressions/**/*.js'`.
Subsystem: Parser (literals) + Runtime/Library (BigInt, BigInt64Array) +
Backend bailouts. Prerequisite: the enabled run itself — current evidence
is only that `-ESBigInt` exposes `BigInt` but not `BigInt64Array` and
`1n + 2n` hits `Internal error` (§5). Expected gain: `BigInt` 2,447
edition failures plus `TypedArray` spillover (927 numeric-literal failures
co-tagged). Uncertainty: high until the enabled run sizes the flagged
implementation. JIT-x64? **yes** (mixed-type arithmetic bailouts).
Effort: measurement **C2-small**, completion/default **C3-large**.
*(measured 2026-10-15, [c2-flag-audits.md](c2-flag-audits.md) §S2: the
enabled run on 21,440 scenarios moves only 125 same-binary fail→pass,
of which 82 BigInt-tagged and all accidental (the stub throws
`TypeError: Type mismatch` for every mixed op, so `assert.throws(TypeError)`
tests pass for the wrong reason; `typeof 10n` is `unknown`,
`10n.toString()` → `[object Object]`, and same-type `1n + 2n` yields a
placeholder that throws `Internal error` when stringified and
`Type mismatch` under string-concat coercion);
hex/binary/octal literals still SyntaxError (82); `BigInt()` with any
argument SIGABRTs the host (80 scenarios, crash class); asIntN/asUintN and
BigInt64Array/BigUint64Array absent. Only 456 of the 2,809 broad
BigInt-tagged C1 failures lie in this file set; the rest cannot move on
the flag. Completion confirmed **C3-large**; the SIGABRT is a candidate
one-line defensive fix independent of the feature.)*

**S3. RegExp Unicode property escapes (`\p{…}`, `\P{…}`).** Repro:
`python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-reprop 'test/built-ins/RegExp/property-escapes/**/*.js'`.
Subsystem: RegExp (Runtime/Library) + Unicode data/ICU. Prerequisite: a
maintained Unicode property data source/version decision (plan P1 raises
the same question for Intl/ICU); `regexp-v-flag` string properties depend
on it. Expected gain: 1,060 edition failures at 302 passed. Uncertainty:
medium (data size/versioning, not semantics). JIT-x64? **no**
feature-specific work expected (RegExp executes in the runtime library;
standard matrix still applies). Effort: **C2** batch, size set by the data
decision.

**S4. Optional chaining + logical assignment.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-optchain 'test/language/expressions/optional-chaining/**/*.js' 'test/language/expressions/logical-assignment/**/*.js'`.
*Status: implemented, interpreter-verified, and default-enabled in C2
batch S4 — see [c2-s4.md](c2-s4.md) (44/164 → 154 passed / 54 failed of 208
on the repro; remaining failures are class-private, BigInt, and a
pre-existing key-ordering issue; x64 JIT verification gated on B4 before
any JIT-configuration claim).*
Subsystem: Parser + ByteCodeGenerator + Backend. Prerequisite: none beyond
the usual bytecode regeneration. Expected gain: 226 tagged edition
failures exactly (58 `optional-chaining` + 168
`logical-assignment-operators`, disjoint tag sets *(observed)*). The case
for doing this batch first is not the count: plan P0 ranks these first for
real-world breakage since both syntaxes are rejected outright by the
parser, and the batch is bounded. Uncertainty: low-medium. JIT-x64? **yes** (short-circuit
bytecode reaches both JIT tiers). Effort: **C2-small/medium** — best
value-per-risk first batch.

**S5. Iterator helpers (`Iterator.prototype.*`, `Iterator.from`).** Repro:
`python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-iter 'test/built-ins/Iterator/**/*.js'`.
*Status: closed in C2 batch S5 — see [c2-s5.md](c2-s5.md). Mac no-JIT
verified and default-enabled (directory edition selection 22/766 →
776 passed / 12 failed of 788; every remaining failure is a
BigInt-literal parse in the test or its includes). The review
follow-up is in the same batch: wrap `next` returns the underlying
result unchanged, and suspended-start `return()` reentry returns a
done result. Post-fix regression: test 1962/0, debug-static 2014/0.
x64 JIT verification stays gated on B4 before any JIT-configuration
claim.*
Subsystem: Runtime/Library (new `Iterator` global and helper intrinsics).
Prerequisite: none identified. Expected gain: 772 edition failures at 14
passed. Uncertainty: low-medium (self-contained builtins; laziness/closing
semantics are the fiddly part). JIT-x64? **no** feature-specific work
expected. Effort: **C2**.

**S6. SharedArrayBuffer/Atomics enabled-flag audit (incl. `waitAsync`).**
Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-atomics --host-flags='-nonative -Test262 -CollectGarbage -ESSharedArrayBuffer' 'test/built-ins/Atomics/**/*.js' 'test/built-ins/SharedArrayBuffer/**/*.js'`.
Subsystem: Runtime (Atomics, agents in the host) + Backend. Prerequisite:
the enabled run; a product/security decision on ever defaulting SAB in an
embedder-facing build. Expected gain: `SharedArrayBuffer` 912 and
`Atomics` 746 edition failures (overlapping; `Atomics.waitAsync` 202
within). Uncertainty: medium-high (host agent support in `ch` bounds what
is even measurable). JIT-x64? **yes** (atomic lowering). Effort: audit
**C2-small**; completion/default decision **C3**.
*(measured 2026-10-15, [c2-flag-audits.md](c2-flag-audits.md) §S6: with
`-ESSharedArrayBuffer`, 461/982 pass on the repro file set with **zero
regressions** (default run passes 0/982 — both globals are
ReferenceErrors). Present: constructor, `slice`, `@@species`,
`@@toStringTag`, core Atomics RMW/wait/notify/isLockFree on shared
buffers. Missing: `waitAsync` (202 tag scenarios), growable SAB
(`grow`/`growable`/`maxByteLength`, ~64, option silently ignored),
non-shared-buffer Atomics per ES2020 (`SharedArrayBuffer object expected`
+ validation-order + harness arg-factory declines, ~58), `Atomics.pause` (post-ES2025). 188 failures are BigInt-literal or
BigInt64Array spillover. `$262.agent` works in this harness (33 of 224
agent-file scenarios pass); only ≈44 failures are genuinely
agent/timing-shaped. Defaulting SAB in an embedder build remains a
product/security decision; the data alone shows no regression and +461.)*

**S7. RegExp `Symbol.*` protocol + prototype properties enabled-flag
audit.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-resym --host-flags='-nonative -Test262 -CollectGarbage -ES6RegExSymbols' 'test/built-ins/RegExp/**/*.js' 'test/built-ins/String/**/*.js'`.
Subsystem: Runtime/Library (String/RegExp) + Backend-intrinsified helpers.
Prerequisite: enabled runs for `ES6RegExSymbols` and
`ES6RegExPrototypeProperties` (the latter has no feature tag, so it needs
its own before/after compare). Expected gain: `Symbol.match` 146,
`Symbol.replace` 174, `Symbol.matchAll` 112, `Symbol.split` 100,
`Symbol.search` 58 edition failures (overlapping). Uncertainty: medium;
`ConfigFlagsList.h:1177` itself warns to re-check JIT-inlined
String/RegExp helpers before enabling. JIT-x64? **yes**, for exactly that
reason. Effort: audit **C2-small**, default flip gated on results.
*(measured 2026-10-15, [c2-flag-audits.md](c2-flag-audits.md) §S7: the
symbols flag moves 346 same-binary fail→pass on `RegExp`+`String`
(6,199 scenarios), all `Symbol.*`-tagged, against 42 regressions that
are two defect classes plus a groups-coercion edge —
`match/replace/search/split` still read `Symbol.*` on primitive search
values (24, the divergence the S11 receipt noted for `replace`), the
failed-match `lastIndex` reset bypassing the observable `[[Set]]` (16
across 8 `Symbol.{match,replace}` lastIndex files: a writable
lastIndex is reset, an accessor/non-writable one never throws), and
`Symbol.replace/result-coerce-groups-err` (2, a separate
groups-coercion defect; none involve `@@matchAll`). `ES6RegExPrototypeProperties` (tagless, measured by before/after on
a 1,558-scenario subset) adds +122 alone and +146 on top of symbols with
zero regressions; it installs the prototype getters and removes legacy
instance built-ins. `Symbol.matchAll` is `undefined` under every flag
combination; `RegExpStringIteratorPrototype`/`Symbol/matchAll` corpus is
2/36 — matchAll is new engine surface (Symbol.matchAll,
`RegExp.prototype[@@matchAll]`, RegExp String Iterator,
`String.prototype.matchAll`), not a flag flip: recommended as the next
C2-small batch together with the two defect-class fixes.)*

**S8. Resizable/growable ArrayBuffer + transfer (and TypedArray
consequences).** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-rab 'test/built-ins/ArrayBuffer/**/*.js' 'test/built-ins/TypedArray/**/*.js'`.
Subsystem: Runtime (ArrayBuffer/TypedArray length tracking) + Backend
(bounds checks). Prerequisite: a design for length-tracking views; plan
C3 lists resizable buffers as a larger project. Expected gain:
`resizable-arraybuffer` 898, `arraybuffer-transfer` 114 (with
`align-detached-buffer-semantics-with-web-reality` failures in the same
area). Uncertainty: high. JIT-x64? **yes**. Effort: **C3-large**;
`Float16Array` (76 failures) is a separable smaller TypedArray batch.

**S9. RegExp named groups family, lookbehind, `d` indices, `v` flag,
modifiers remainder.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-regroups 'test/built-ins/RegExp/named-groups/**/*.js' 'test/language/literals/regexp/**/*.js'`.
Subsystem: RegExp parser/compiler (Runtime/Library). Prerequisite: S3 for
the `v`-flag property-of-strings parts; named groups precede duplicate
named groups (plan P0 ordering). Expected gain: `regexp-v-flag` 272,
`regexp-modifiers` 140 (of 460; the rest already pass, mostly negatives),
`regexp-named-groups` 82, `regexp-match-indices` 60, `regexp-lookbehind`
38, `regexp-duplicate-named-groups` 32 — counts understate because each
missing construct SyntaxErrors whole test files filed under other tags
*(inferred)*. Uncertainty: medium (lookbehind is the deep one). JIT-x64?
**no** feature-specific work expected. Effort: **C2** per sub-batch;
lookbehind may prove **C3**.

**S10. Set methods + copy-by-change family.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-setcopy 'test/built-ins/Set/**/*.js' 'test/built-ins/Array/**/*.js' 'test/built-ins/TypedArray/**/*.js'`.
*Status: both halves are closed on Mac no-JIT — see
[c2-s10.md](c2-s10.md). The copy-by-change half landed seven
unconditional library methods; the Set-methods half (S10 proper) added
`union`, `intersection`, `difference`, `symmetricDifference`,
`isSubsetOf`, `isSupersetOf`, and `isDisjointFrom`, with no flag and
no opcode. Interpreter evidence: es7 `change-array-by-copy.js` and
`setmethods.js` pass; the seven array/typed-array prototype
directories are 246 passed / 28 failed of 274 (234 fail→pass, 0
pass→fail against the C1 baseline; 16 BigInt-literal parses, 8 missing
`resize`, 4 resizable-buffer RangeError cases); the seven Set
prototype directories are recorded in the receipt. x64 JIT
verification stays gated on B4.*
Subsystem: Runtime/Library. Prerequisite: none identified. Expected gain:
`set-methods` 316, `change-array-by-copy` 262 edition failures.
Uncertainty: low (well-specified builtins; set-like protocol edge cases).
JIT-x64? **no** feature-specific work expected. Effort: **C2-small** each.

**S11. Small missing builtins (individually selectable).** Repro:
`python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-smallbuiltins 'test/built-ins/String/**/*.js' 'test/built-ins/Promise/**/*.js' 'test/built-ins/Object/**/*.js' 'test/built-ins/RegExp/escape/**/*.js'`
(narrow the patterns to the single builtin being batched). Subsystem:
Runtime/Library.
Expected gains: `String.prototype.replaceAll` 80,
`String.prototype.matchAll` 30 (plus S7's `Symbol.matchAll` dependency),
`array-grouping` 48, `RegExp.escape` 40, `promise-try` 26,
`String.prototype.isWellFormed/toWellFormed` 16 + 16,
`promise-with-resolvers` 11. Prerequisite: matchAll rides on S7.
Uncertainty: low. JIT-x64? **no**. Effort: **C2-small** — good filler
batches alongside a C3 item. `String.prototype.replaceAll` is landed on Mac no-JIT (see [c2-s11.md](c2-s11.md)),
as is the S11 remainder (`Object.groupBy`, `Map.groupBy`, `RegExp.escape`,
`Promise.try`, `Promise.withResolvers`, `String.prototype.isWellFormed`,
`String.prototype.toWellFormed` — same receipt); matchAll still rides on S7.

**S12. Import attributes + JSON modules, and the module host-loader
cluster.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-importattrs 'test/language/import/**/*.js' 'test/language/module-code/**/*.js'`.
Subsystem: Parser (attribute syntax) + JsRT module callbacks + `bin/ch`
host loader. Prerequisite: an embedder-facing decision on how attributes
reach the host fetch callback (API surface, plan C3 note). Expected gain:
`import-attributes` 132, `json-modules` 12 edition failures; separately,
the *host* bucket's 294 failures are dominated by `URIError: ./…_FIXTURE.js`
module-load failures in the `ch` host (top host messages in the analysis
JSON), which may belong to the host harness rather than the engine —
triage them together. Uncertainty: medium-high (API design). JIT-x64?
**no**. Effort: **C2/C3** (syntax small, host contract is the work).

**S13. WeakRef / FinalizationRegistry + symbols as weak-collection
keys.** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-weakref 'test/built-ins/WeakRef/**/*.js' 'test/built-ins/FinalizationRegistry/**/*.js' 'test/built-ins/WeakMap/**/*.js' 'test/built-ins/WeakSet/**/*.js'`.
Subsystem: Recycler GC + Runtime (cleanup jobs/microtask retention).
Prerequisite: an ownership record per
[memory-management.md](memory-management.md) (host job queue contract for
cleanup callbacks). Expected gain: `FinalizationRegistry` 98, `WeakRef`
74, `symbols-as-weakmap-keys` 40. Uncertainty: medium-high (GC timing in
tests). JIT-x64? **no** feature-specific work expected. Effort:
symbols-as-keys **C2-small** candidate; WeakRef/FR **C3**.

**S14. Inherited-semantics defect clusters (existing features).** Repro:
`python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-blockscope 'test/language/block-scope/**/*.js'`
(example cluster; pick further clusters from the defect bucket in the
committed analysis and substitute their patterns).
Subsystem: Parser (early errors) + Runtime (TDZ/global scope). Evidence:
3,822 engine-defect-on-existing failures overall; 327 parse negatives
*executed* invalid code (`$DONOTEVALUATE` reached); an untagged TDZ
cluster ("An initialized binding is not created prior to evaluation…",
160); `for-in-order` still split 8P/10F; `untagged` totals 1,460 edition
failures. Prerequisite: per-cluster bisection — this item is a standing
triage stream, not one batch. Uncertainty: high per cluster, low that it
matters (these are the failures no feature flag explains). JIT-x64?
**case-by-case** (any bytecode-semantics fix re-enters the JIT matrix).
Effort: **C2-small per cluster**.

## 7. Separate inventories

**Engine defects on existing features** — the 3,822-failure defect bucket
and S14; includes the 327 executed parse negatives and failing negatives
generally. These do not need new features, only correctness work.

**Disabled-feature incompleteness** — default-off flags named in the
policy, with `ConfigFlagsList.h` anchors: `ESBigInt` (false at :669 —
flagged path observably incomplete, §5), `ESSharedArrayBuffer` (false at
:709 — globals appear when enabled, conformance unmeasured),
`ES6RegExSymbols` (false at :692-696 — protocol appears when enabled,
JIT-helper warning at :1177), `ES6RegExPrototypeProperties` (false at
:687-690 — no feature tag; measured only by before/after runs). The
disabled-flag bucket is 3,937 failures, but it *understates* flag effects
where a test lacks the gating tag (e.g. BigInt64Array failures filed under
`TypedArray` land in required-feature-missing).

**Host limits** — recorded skips: `IsHTMLDDA` 70 and `CanBlockIsFalse` 4
scenarios skipped by `host-filter.js`, plus 828 duplicate strict module
scenarios (not a limit; the source runs once). In-results host bucket:
294 failures, dominated by `ch` module-loader `URIError`s (S12) — host or
harness triage, not edition debt — plus 6 `Host exited abnormally:
{"code":null,"signal":"SIGKILL"}` records in its top messages, which the
recorded data cannot distinguish between harness timeout-kill and engine
crash; they stay in the host bucket pending a scoped rerun *(observed
from tool output; classification inferred)*.

**Future proposals (excluded from the edition score, retained in the
broad diagnostic)** — 27 tags / 12,597 scenarios / 11,928 failures,
dominated by `Temporal` (9,226 failures, ES2027), then
`explicit-resource-management` (709), `iterator`-family 2026/2027 items,
`source-phase-imports`, `import-defer`, `decorators` and the other
stage-≥3 proposals itemized with sources in `edition-policy.json`. None
of these block C2 batches; `Temporal` and decorators are plan-level
stretch scope, not C4 debt.

## 8. Reforecast and open items

C1's evidence supports plan §4's shape with one sharpening: the edition
debt is **19,648 scenario failures**, of which class elements (S1) and
flag-gated surfaces (S2/S6/S7) plausibly account for the majority once
co-tag spillover is credited — but two of those three are C3-sized or
measurement-gated, so the fastest *rate* of edition gain is S4 + S10/S11
+ S3/S5 while S1/S2 designs proceed *(inferred)*. No calendar estimate is
offered; per-batch receipts under C2/C3 rules remain the forecasting
instrument.

Unresolved after C1:

- Explicit host-reported negative phase (closes §3's residual gap);
  engine/host change, prerequisite for any C4 phase claim. The 12
  resolution-phase suspects are dispositioned in §3 as true wrong-phase
  passes retained in the recorded baseline by design.
- Rerun of the fixture-cache-affected subsets (§3: 30 directories, 888
  recorded scenarios / 478 edition-selected exposed, 2 flips measured at
  `--jobs=1`). The cache itself is fixed in `tools/test262/harness.js`,
  but the §2 baseline was recorded pre-fix: disposition is to rerun those
  subsets with the fixed harness before any C4 certification, and before
  treating baseline counts for those directories as exact. The same rerun
  should use `--jobs=1` (or per-agent temp isolation, also owed) so the
  shared-temp agent race in §3 cannot reintroduce flips.
- Enabled-flag test262 runs for `ESBigInt`, `ESSharedArrayBuffer`,
  `ES6RegExSymbols`, `ES6RegExPrototypeProperties` (S2/S6/S7) — the
  measurements that turn tag-implicated buckets into facts.
- B4 native x64 execution, which every "JIT-x64? yes" item waits on for
  its C2 completion gate.
- The Unicode/ICU data-version decision behind S3 (and plan P1 Intl).
