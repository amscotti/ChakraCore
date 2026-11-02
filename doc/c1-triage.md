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
pal/ bin/` was empty when this was recorded on 2026-10-13 *(observed;
superseded by the C2 batches, which change `lib/`)*; the tree-wide changes
since that commit at the time were build/tooling/docs files (`build.zig`, `tools/`, `doc/`, CI and root
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

## 3. Negative-phase qualification — host marker delivered (C1 condition met; certification-grade gaps tracked)

**Update (2026-11-01):** the host now reports the phase explicitly. Lane
`c1-negative-phase` (merge `c381c2e37`, receipt
[c1-negative-phase.md](c1-negative-phase.md)) added `ch
-NegativePhaseReport` — one stderr marker line per failing run
(`CH_NEGATIVE_PHASE: parse|resolution|runtime`), classified solely from
failure origin — and `run.py`/`analyze.py` now re-judge every negative
corpus scenario against the declared `negative.phase` (first-marker-wins
+ line-1 tripwire after an adversarial spoofing repair; shape heuristic
is the marker-less fallback only; 25 controls incl. a forged-marker
control). Flag-off output is byte-identical (317/317). A fresh full
pinned-corpus baseline was recorded with phase re-judging (93,141
scenarios: 77,257 passed / 15,884 failed, controls 25/25, 8,751
marker-judged negatives, 12 downgrades = the audit's known 12; zero
natural forgeries/displacements across 23,517 marker-carrying records).
What still blocks certification-grade phase claims (engine gaps the
marker exposes, tracked for C4): child module link errors surface at
evaluation instead of propagating to root declaration-instantiation
(10 corpus scenarios), JSON module `JSON.parse` runs at evaluation
rather than instantiation, the sticky resolution witness on handled
dynamic-import rejections (latent, 0 incidence), agent-thread and
dynamic-import failures emit no marker (shape fallback), and the
deferred-parse/compile-channel stage-reporting limits. The original
audit text follows.

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
still owed (§9). At the default `--jobs=8` the same alone-vs-combined
comparison flips ~79 scenarios with unstable membership before *and*
after the cache fix, and jobs=8 alone totals (1,214/1,207 passed) sit
below the jobs=1 total (1,267) *(observed)*: that instability is not the
fixture cache. All 8 agents share one per-run temp directory in which
module tests and their fixtures are written under deterministic basenames
and removed with fire-and-forget unlinks, so concurrent agents can
overwrite or delete each other's files — the jobs=8 instability above is
consistent with that intra-run race (not isolated here from load/timeout
effects), and per-agent temp isolation does not yet exist.

**Update (post-C1 fix pass):** per-agent temp isolation has landed —
`tools/test262/harness.js` now gives every agent its own `agent-N/`
subdirectory of the per-run temp dir, and the controls grew a
duplicate-basename fixture pair (`dup-a`/`dup-b`, 20 controls) that
discriminates both the fixture cache and the shared-temp race: with
either protection removed, one side of the pair deterministically
receives the other's fixture and the controls abort *(observed A/B)*.
With isolation in place, `test/language/module-code/**` (602
scenarios) shows **0 flips** across two `--jobs=8` runs and one
`--jobs=1` run *(observed)*. The §2 baseline rerun below is still
owed (it predates both fixes), but it no longer needs `--jobs=1`.

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

*(sub-batch 1, 2026-10-17, [c2-s2-bigint.md](c2-s2-bigint.md): landed the
scoped conversion/stringification/crash-safety slice on Mac no-JIT. The
scoped corpus is 94/154 with 0 SIGABRT (pre-slice 32/154 with 40 SIGABRT;
interrupted-slice pre-completion 68/154), 62 fail→pass and 0 pass→fail vs
the pre-slice artifact, 26/0 vs the interrupted-slice artifact. `typeof
10n` is `bigint`, `10n.toString()` is `10` (radix 2–36 and `valueOf`
landed), multi-digit `NumberToBigInt` and `StringToBigInt` work, `String`/
template/concat stringify BigInt, `ToBoolean(0n)` is false, `Number(BigInt)`
is correctly rounded (including `Number(Object(1n))`), `JSON.stringify`
throws for BigInt unless `toJSON`/a replacer converts it, cross-realm
wrapper `valueOf` marshals correctly, and
all unsupported operators now throw a controlled `TypeError` (no
assert/abort). Remaining scoped failures are
exactly `asIntN` 28 + `asUintN` 28 (methods absent; `arithmetic.js` also
blocked by `0x`/`0b`/`0o` BigInt literal parsing), wrapper arithmetic/
equality 2, and `toString/a-z` 2 (its loop condition is the still
fail-closed mixed BigInt/Number relational comparison). C3 completion
therefore still owns asIntN/asUintN, all remaining arithmetic, BigInt64
typed arrays, mixed comparison/abstract equality, wrapper arithmetic, and
non-decimal BigInt literal grammar. Gates moved 1974/2026 → 1975/2027.)*

*(sub-batch 2, 2026-10-17, [c2-s2-bigint.md](c2-s2-bigint.md): implemented
`BigInt.asIntN`/`asUintN` in the working tree with ToIndex coercion/order, static descriptors,
`length=2`, names, `ErrorOnNew`, and bounded limb-accurate
two's-complement truncation on Mac no-JIT. Also corrected abstract
`ToBigInt(Number)` to throw `TypeError` (the BigInt constructor performs
its own NumberToBigInt step), which the asIntN/asUintN fixtures require.
The scoped corpus is 146/154 on both test and debug-static binaries
(baseline 94/154), with 52 fail→pass (26+26) and 0 pass→fail. The 8
residue are the four `arithmetic.js` scenarios blocked first by the
non-decimal BigInt literal parser gap, wrapper arithmetic/equality 2, and
the mixed relational comparison in `toString/a-z.js` 2; the 150/154
target is unreachable without the excluded parser work. Gates moved
1978/2030 → 1979/2031. Remaining C3 work is unchanged otherwise:
arithmetic, BigInt64 typed arrays, mixed comparison/abstract equality,
wrapper arithmetic, Map/Set BigInt keys, and non-decimal literals.)*

*(sub-batch 3, 2026-10-17, [c2-s2-bigint.md](c2-s2-bigint.md): accepted
`0x`/`0o`/`0b` (both prefix cases) BigInt literals with the existing
numeric-separator placement rules, in sloppy and strict code, on Mac
no-JIT. Bare prefixes, invalid digits, separator edges, leading-zero
decimal forms (`08n`, `00n`, `0_0n`), decimal points, bad suffixes and
trailing identifiers match Node's `SyntaxError` behavior; hex `e` remains
a digit (`0x1e2n` is 482n). Decimal `_` separators (`1_2n`) are fixed as
collateral; prefixed digit accumulation is chunked for long literals. The scoped corpus
is 150/154 on both test and debug-static binaries (sub-batch 2 baseline
146/154), exactly the four `asIntN`/`asUintN` `arithmetic.js` scenarios
flipping fail→pass with 0 pass→fail. The 4 residue are wrapper
arithmetic/equality 2 and the fail-closed mixed relational comparison in
`toString/a-z.js` 2. Gates moved 1979/2031 → 1980/2032. Remaining C3
work: full arithmetic, BigInt64 typed arrays, mixed comparison/abstract
equality, wrapper arithmetic, Map/Set BigInt keys, and JIT/B4.)*

*(sub-batch 4, 2026-10-17, [c2-s2-bigint.md](c2-s2-bigint.md): implemented
mixed BigInt/Number abstract equality and relational comparison, plus
wrapper multiplication, on Mac no-JIT. Equality covers Number/String/
Boolean/null/undefined/Symbol/objects/wrappers with exact large-integer
and NaN/Infinity/fractional handling; relational comparison does
ToPrimitive(hint Number) in left-first order, both-string lexicographic
comparison, exact BigInt/String StringToBigInt (invalid strings compare
false), mathematical BigInt/Number ordering, and Symbol TypeErrors;
`Object(1n) * 1n` unwraps the wrapper before operator selection. The
scoped corpus is **154/154** on both test and debug-static binaries
(sub-batch 3 baseline 150/154), exactly the four `toString/a-z.js` and
`wrapper-object-ordinary-toprimitive.js` scenarios flipping fail→pass
with 0 pass→fail; the requested target is reached. Gates moved
1980/2032 → 1981/2033. Remaining C3 work is full BigInt
arithmetic/bitwise/shift, BigInt64 typed arrays, Map/Set BigInt key
identity, literal property names, giant-`ToString` performance, and
JIT/B4.)*

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
C2-small batch together with the two defect-class fixes.
**Landed 2026-10-15 as C2-S7** ([c2-s7-matchall.md](c2-s7-matchall.md)):
all four surfaces are behind `ES6RegExSymbols`, the 42 flag-on
regressions are 42/42 fixed with 0 new pass→fail on the same universe
(136 fail→pass total after review fixes, 4,285→4,421 passes), and the matchAll corpus is
132/140 — the 8 residue is BigInt/v-flag parse debt plus the 4
`species-regexp-get-{global,unicode}-throws` scenarios that need the
`ES6RegExPrototypeProperties` substrate. Default flags and the JIT stay
untouched (B4).)*

**S8. Resizable/growable ArrayBuffer + transfer (and TypedArray
consequences).** Repro: `python3 tools/test262/run.py --binary zig-out/bin/ch --output out/test262-c1-rab 'test/built-ins/ArrayBuffer/**/*.js' 'test/built-ins/TypedArray/**/*.js'`.
Subsystem: Runtime (ArrayBuffer/TypedArray length tracking) + Backend
(bounds checks). Prerequisite: a design for length-tracking views; plan
C3 lists resizable buffers as a larger project. Expected gain:
`resizable-arraybuffer` 898, `arraybuffer-transfer` 114 (with
`align-detached-buffer-semantics-with-web-reality` failures in the same
area). Uncertainty: high. JIT-x64? **yes**. Effort: **C3-large**;
`Float16Array` (76 failures) is a separable smaller TypedArray batch.
*(Bounded Float16 slice landed 2026-10-18 on Mac no-JIT, receipt
[c2-s8-float16.md](c2-s8-float16.md): `DataView.prototype.getFloat16`/
`setFloat16` and `Math.f16round` with a portable integer bit converter.
The pinned 100-scenario Float16 corpus moved 22/100 -> 94/100 with 72/0
fail->pass/pass->fail; the 6 residue are 4 `resizable-buffer` scenarios
blocked on `ArrayBuffer.prototype.resize` (S8 proper) and 2
`immutable-buffer` scenarios tagged post-ES2025 `immutable-arraybuffer`
(`transferToImmutable` is absent). The `Float16Array` constructor and
the typed-array element type remain the separable S8/C3 batch. Gates
moved 1981/2033 -> 1982/2034.)*

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
*(Sub-batch 1 — named groups core — landed 2026-10-16 on Mac no-JIT, see
[c2-s9-namedgroups.md](c2-s9-namedgroups.md)): `(?<name>...)`, `\k<name>`,
the null-prototype `groups` object on exec/match results, and `$<name>`
substitution in replace/replaceAll on both the ES5 and `@@replace` paths
— unconditional (ES2018, no flag, no new opcodes, no PropertyId change).
Corpus `RegExp/named-groups` + `language/literals/regexp/named-groups`:
158/184 default flags, 162/184 under `-ES6RegExSymbols`; residue is
exactly the deferred sub-batches (20 duplicate-names ES2025, 2
lookbehind, 4 subclass scenarios that need the symbols flag). Resym
universe joins: 168/0 (default) and 194/0 (flag-on) fail→pass/pass→fail.
Gates moved 1968/2020 → 1970/2022.
Sub-batch 2 — the `d` flag / match `indices` — landed 2026-10-16 on
Mac no-JIT (same receipt, "Sub-batch 2"): unconditional `d`-flag
parsing (both flag parsers + the four flag-string builders, canonical
`dgimsuy` order), the `indices` array on exec/non-global-match results
(`d` only; own property via a second, 4-entry regex-result type), and
`indices.groups` (present-with-`undefined` without named groups);
`RegExp.prototype.hasIndices` and the `flags` getter's `d` are wired
under `ES6RegExPrototypeProperties` (corpus-pinned, not own-instance
built-ins). Corpus: `RegExp/match-indices` 28/28 (default and symbols),
`RegExp/prototype/hasIndices` + `flags` 42/48 under the proto flag
(residue: 4 BigInt `4n` parses, 2 `v`-flag SyntaxErrors); resym joins
28/0 and 28/0. Gates moved 1970/2022 → 1972/2024. Sub-batch 3 —
ES2025 duplicate named groups — landed 2026-10-16 on Mac no-JIT (same
receipt, "Sub-batch 3"): duplicate names parse iff a disjunction
separates them (pass-0 alternative-scope check), `groups`/
`indices.groups`/`$<name>` resolve to the last participating group
(undefined when none), and `\k<name>` with duplicates compiles to a
new regex-internal `MatchNamedGroup` instruction implementing the
participation rule (participating value must match; empty only when
no group with the name is live — corpus `duplicate-names-exec.js`).
Corpus: the 190-scenario named-groups set 182/8 default / 188/2
symbols (residue: 2 lookbehind + 6 symbols-gated), the 20
duplicate-name scenarios flipped 0 collateral; match-indices 28/28;
resym joins 28/0 and 30/0 incremental (224/0 and 252/0 vs C1). Gates
moved 1972/2024 → 1973/2025. Sub-batch 4 — ES2018 lookbehind
assertions — landed 2026-10-16 on Mac no-JIT (same receipt, "Sub-batch
4"): `(?<=`/`(?<!` parse (never quantifiable, both modes), the body is
syntactically mirrored at parse time and executed right-to-left as a
self-contained sub-match over a reversed window (reverse-order
alternation/greed/capture semantics per the spec's reverse matcher;
lookaheads inside lookbehinds run as real-suffix sub-matches, nested
lookbehinds as ordinary lookaheads; `^`/`$`/`\b` inside are
frame-aware; group spans stay in real coordinates). The entire
`RegExp/lookBehind/` corpus directory passes; the 190-scenario set is
118/6 default (residue: the 6 long-classified symbols-gated scenarios)
and 124/124 under `-ES6RegExSymbols`; match-indices+duplicate-names
30/30; joins vs C1 96/0, 102/0, 28/0; resym universe 4,113/2,086 with
260/0 vs C1 (36/0 incremental). Gates moved 1973/2025 → 1974/2026.
Sub-batch 5 — ES2025 scoped modifiers — landed 2026-10-17 on Mac no-JIT
([c2-s9-modifiers.md](c2-s9-modifiers.md)): `(?ims-ims:Disjunction)`
parses as a non-capturing group in both passes, nested add/remove
update the current state with the top-level flags as the base, the
parser saves/restores effective `i`/`s`/`m` around the body, and the
instruction compiler uses per-node multiline/case state (including
frame-aware `^`/`$`/`\b` inside lookbehinds); whole-program special
cases are disabled for modified patterns. The scoped corpus
(`RegExp/regexp-modifiers/**` plus `language/literals/regexp/
*modifiers*.js`, 306 scenarios) moved 166/140 → 298/8 with exactly
132 fail→pass and 0 pass→fail; all 154 top-level negative modifier
tests stay green (and so do all 166 negative scenarios inside the
scoped corpus), and the 8 residue are `\p{...}` property escapes
deferred to S3. Gates moved 1977/2029 → 1978/2030. Remaining S9 work: the `v`
flag, which needs S3.)*

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
`String.prototype.toWellFormed` — same receipt); matchAll has since landed
via S7 ([c2-s7-matchall.md](c2-s7-matchall.md)).

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
*(symbols-as-keys implemented 2026-10-17 as C2-S13 on Mac no-JIT
([c2-s13-symbol-keys.md](c2-s13-symbol-keys.md)):
unregistered symbols are accepted as `WeakMap`/`WeakSet` keys through a
lazy, traced per-collection recycler dictionary (WeakSet uses the non-leaf
allocator), registered `Symbol.for` keys are rejected per
`CanBeHeldWeakly`, and object keys keep the existing weak path. Scoped
corpora moved WeakMap 214/281 → 232/49 with 18/0 fail→pass and WeakSet
154/170 → 168/2 with 14/0; the remaining WeakMap failures are 47 ES2026
`getOrInsert*` scenarios plus 2 pre-existing `proto-from-ctor-realm`
failures, and the 2 WeakSet failures are the same cross-realm defect.
Gates moved 1976/2028 → 1977/2029. WeakRef/FinalizationRegistry stay C3,
and B4 still owns JIT-configuration claims.)*

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

*(S14 sub-batch 1, 2026-10-17, [c2-s14-blockscope.md](c2-s14-blockscope.md):
landed the `Block : { StatementList }` static-semantics early errors for
lexical-vs-var declarations and duplicate lexical declarations, including
function/generator/async names and nested blocks, while preserving Annex B
sloppy duplicate block functions. The block-scope corpus moved 167/287 to
287/287 with exactly 120 fail→pass and 0 pass→fail, including the three
`in-statement-position-*` loop-body negatives. The remaining S14 stream is
the separate for-head/switch/catch/eval/with rules recorded in the receipt.

*(S14 sub-batch 2, 2026-10-17, [c2-s14-statement-early-errors.md](c2-s14-statement-early-errors.md):
closed the measured statement-level early-error clusters: for-head
lexical-vs-var 4/4, switch CaseBlock 59/59, catch-parameter-vs-body-lexical
4/4, and unlabelled `with` function declaration 1/1. Corpora moved
for 736→740, switch 129→188, try 355→359, with 169→170, 68 fail→pass and 0
pass→fail; block-scope stays 287/287. Gates unchanged at 1976/2028. Residual
S14: catch pattern/var edge cases, eval-introduced var conflicts and the
broader defect bucket.)*
The `test/LetConst` expectations (`letvar`, `defer3`, `for-loop`) that
asserted the old non-conforming block `var`/`let` behavior were updated
deliberately. Gates moved 1975/2027 → 1976/2028. No bytecode change; B4
still owns JIT-configuration claims.)*

## 7. C2 integration status (2026-10-29)

| Batch | Status | Integrated evidence | Remaining |
|---|---|---|---|
| S1 public instance fields | Landed `bd0de1694` | class corpus 1,989/5,897, 539/0; 27 focused cases, normal + forced defer | JIT (private/static/computed fields, static blocks and eval-super closed by later S1 rows) |
| S1 private instance fields | Landed `da0351e8e` | private subset 176/30, 160/0; es7 64/0 | direct-eval privates, JIT (methods/statics/computed closed by later S1 rows) |
| S1 private methods/accessors | Landed `30a81731c` | 202/20, 198/0; es7 68/0 | direct-eval privates, JIT (statics/blocks/computed/async-gen closed by later S1 rows) |
| S1 public static fields | Landed `041a9bb8f` | 75/75, 35/0; local 75/0 | JIT (private static/blocks/computed/super.prop closed by later S1 rows) |
| S1 private static fields | Landed `226e7e1b4` | 600/602, 571/0; local 2012/0 | direct-eval privates, JIT (blocks/methods/computed closed by later S1 rows) |
| S1 private static methods/accessors | Landed `197d381d7` (merge `c318fa42d`) | class delta 182 fixed / 0 new; es7 25/25 + 13/13; dual-review ACCEPT | direct-eval privates, JIT (async/gen + static blocks closed by later S1 rows) |
| S1 `super` in static field initializers | Landed `920822ac0` (merge `0c01875b5`) | node-parity probes; committed tests green; dual-review ACCEPT | JIT (instance-field super closed by computed row; eval-super closed below) |
| S1 computed field names + instance-field `super` | Landed `894ba5394` (merge `fd893c83f`) | class-elements 1833/255 vs base 1699/389 (2,088 scenarios, lane-run), 67 files f→p / 0 p→f; dual-review ACCEPT | JIT (private names in computed keys closed by S1 tail; eval-super closed below) |
| S1 async/generator private methods | Landed `7cc057124` (merge `1caf23d67`) | scoped test262 272 f→p / 0 p→f (lane-run); dual-review ACCEPT | JIT |
| S1 static blocks | Landed `9ba243adb` + fix `6ff088e19` (merge `02f99fc63`) | loop-2 dual-review ACCEPT after defer-scan scope fix; 69/69 node differentials ×3 modes | JIT; bare-`await`/nested-super defer residuals shared with field initializers |
| S1 tail: direct-eval + computed-key privates, field semantics | Landed `1418c292f` (merge `de92ddb80`); default-BigInt rerun 2026-10-29 | elements corpus 5,888/5,897 (`out/test262-c2-wave3`) → **5,892/5,897** (`out/test262-c2-s1-default-bigint`) after BigInt default-on; +4/0 parse-barrier scenarios | JIT; 4 post-ES2025 accessor grammar; 1 Stage-1 non-extensible |
| S1 direct-eval `super` early errors | Landed `851d0646f` (merge `ea8f253ac`) + review `39e9f2a16` | `super-prop*` 12/0; eval-code 454/0 (10 f→p / 0 p→f); class elements 5,888/9 unchanged; object-literal function/arrow false accepts fixed; 3 parse-mode entries; legacy expectations corrected | JIT/serialization (B4); pre-existing super[expr] before super() order |
| S8 Float16Array | Landed `de89e6d27` + fix `161639b48` (merge `a39aa8c45`) | gates 2013/2065 (new float16array.js 17/17); loop-2 dual ACCEPT after GetDiagTypeString fix | JIT kind lowering (B4); Atomics exclusion by allowlist |
| S8 immutable ArrayBuffer | Landed `ce2179b2a` + review `2fb37b4d6`, `cbf91b796` (merge `4ac863379`) | focused 84/38 default, 118/122 flag-on; adjacent +110 f→p / 0 p→f; new test entry; gates 2022/2074 | JIT fast path, TTD snapshot fidelity, JsRT raw storage, B4 |
| S3-a Unicode property escapes (`u`) | Landed `b9bb103c3` + repairs `96c563e1f`, `3fd125328`, `f622a4218` (merge `1d92d7b37`) | focused 17 groups/400 assertions; bounded RegExp corpus 1,020/960 → 1,814/166, 794 f→p / 0 p→f; 96 timeouts removed; ICU 79.1 | `v` flag/strings, ICU-vs-corpus data drift, pre-existing u-mode `{`/`]` leniency, JIT/B4 |
| S3-b RegExp `v` / Unicode Sets | Landed `7bd66a59d` + repairs `aadf5aa88`, `ce0ba752a`, `d6d2bc1eb` (merge `dd60bdadd`) | focused 33 groups/560 assertions; v corpus 346/28 default, 372/2 flag-on; S3-a selection +14 f→p / 0 p→f; set ops, `\q`, string properties, unicodeSets accessor | RegExp substrate failures, inherited u-mode lacunarity/leniency, Unicode data drift, JIT/B4 |
| S9 u-mode RegExp strictness | Landed `a7b60e94a` + repairs `4c2ccf463`, `b27b3e8d5` (merge `b8080f59f`) | literals 458/22 → 480/0; total +74 f→p / 0 p→f over 4,192; canonical v 346/28 unchanged; sticky OOB and deferred identity abort fixed | literal-scanner surrogate advance, mid-pair manual lastIndex, positive lone-surrogate classes, ICU drift, B4 |
| S9-b code-point RegExp search | Landed `9c2c6d85b` + repairs `f8a7a3f7a`, `52e02ccec`, `167deae86` (merge `33b1192e7`) | focused matchall 25/25 + u-strictness 21/21; mid-pair lastIndex/trail-literal scans fixed; RegExp builtin `v` fullUnicode + substring flags fixed; listed test262 selections 0 p→f; gates unchanged 2026/2078 | lead-surrogate lacunarity, substrate-off undefined flags fallback, non-progress safety deviation, B4 |
| S6a growable SAB + non-shared Atomics (flag-on) | Landed `01923087f` + review `eaa526f89` (merge `e3371d313`) + final `39e9f2a16` | focused S6 462/982 → 574/982, 112 f→p / 0 p→f; new test 13/13; debug index-detach abort fixed; SAB/Atomics remain off by default | BigInt Atomics, multi-agent wait/grow, B4 |
| S6b `Atomics.pause` + single-thread `waitAsync` (flag-on) | Landed `0a1c592b3` + review `5fa9524e2` (merge `eef3b93c5`) | pause 10/10; focused pause+waitAsync 0/124 → 80/124, 72/202 exact, 80/0 f→p; short calls/counts/cross-realm/FIFO fixed; new test 1 entry | finite timeout scheduling, cross-agent waitAsync, BigInt waitAsync, B4; SAB default off |
| S6c BigInt Atomics (flag-on) | Landed `d13f5bdaa` + cleanup `cda8a2fd3` (merge `981fe000d`) | exact BigInt dirs 22/206 → 136/206; Atomics+SAB 698/284 → 814/168, 116/0 f→p/f; shared+non-shared BigInt64/BigUint64 RMW, wait/notify/waitAsync validation | finite timeout, cross-agent suspension, SAB constructor-order, B4; BigInt/SAB defaults off |
| S6d finite `waitAsync` timeouts (flag-on) | Landed `9a6524b8b` + repairs `d702ea085`, `2a2b37148`, `c71f35ff3` (merge `dd6434127`) | additive optional host timer callback; recycler-owned timeout task; waitAsync 116/202 → 122/202, +6/0; huge-delay host wrap fixed; gates unchanged 2026/2078 | cross-agent waitAsync, no timer callback fallback, stale-task shutdown latency, TTD replay, B4 |
| S6e cross-agent `waitAsync` (flag-on) | Landed `d972c53fe` + repairs `e547fa345`, `e23fc6f6b`, teardown-only `a78a5dba4` + receipt `866a03c82` (merge `6900598e4`); FIFO resolution repair `533e41d37`/`d99ab67c0` + receipt `d9a6f2d7b`, `a16657242` (merge `02c45f214`) | native wake latch + registrant-thread monitor tasks + child message queues; POSIX `WScript.Quit` flush + raw `::_exit` teardown (no PAL/runtime cleanup), eliminating the deterministic Quit crash families; waitAsync selection 122/80 → 201/1 (repeat 199/3), 0 pass→fail; Atomics selection 691/83, SAB 204/4, merged ≈895/982; new test 1 entry; gates 2030/2082 → 2031/2083. FIFO repair: per-notify-pass ordered chain + head-first claims restored guaranteed per-agent FIFO resolution (30/2400 pre → 0/2400 post; 7,800 adversarial pass-observations clean), gates unchanged | pre-Quit multi-agent startup SIGBUS fixed by E1 (merge `3574b6168`), exit assert under >64 agents fixed by host-exit-join `d640a56e6`/`17c05c557` (merge `5f4e722d1`; pre-fix release UAF 33/50 at 100 agents, post 50/50 clean; 400-agent adversarial scale clean), unnotified cross-runtime waits poll, blocked-stdout Quit hang (pre-existing), TTD+Quit log truncation (pre-existing), SAB default off, B4 |
| E1 agent-startup SIGBUS (engine defect) | Fixed `944921636` + receipt `58d02c90b`, `e28b11109` (merge `3574b6168`) | card-table stack-coverage gap: `X64WriteBarrierCardTableManager::OnThreadInit` rounded stack pages down, dropping the unaligned top partial page (macOS pthread `stackaddr`) from section commit; stack write barriers on `WriteBarrierPtr` temporaries then read uncommitted PROT_NONE card pages (deterministic `+0x5c000` signature, not a race); one-line round-up fix, ownership unchanged; 55/11200 → 0/2450 interposed churn (clean 0.24% → 0/500); dual ACCEPT; gates unchanged 2031/2083 | PAL low-end up-round coverage gap (documented), malloc'd `WriteBarrierPtr` theoretical gap, statistical bounds, B4 |
| Host robustness follow-ups (join/spawn/broadcast) | join `17c05c557` (merge `5f4e722d1`); spawn-check `fd7b022b1` (merge `a77b1a49d`); broadcast-type `91f4db697` + leak repair `eb1c49f8f` (merge `da9911395`) | end-of-main join bounded to per-child waits (PAL 64-handle cap made >64 agents crash with a release UAF, 33/50 at 100 agents; post 50/50 clean, 400-agent scale clean); failed agent spawns roll back and report catchable `JsErrorOutOfMemory` instead of hanging the parent forever (12/12→12/12 both profiles); `WScript.Broadcast` non-SAB argument now throws instead of SIGSEGVing with live agents (15/15→20/20 both profiles) with balanced agentless acquire-and-release after a review-found leak; new `s6-broadcast-type.js` entry moved gates 2031/2083 → 2032/2084; each lane dual-ACCEPT | pre-existing 5s broadcast-consumed timeout release, no-arg Broadcast silent undefined, plain-Error-not-TypeError, engine-side mis-released handle undefended, B4 |
| S12 import attributes + JSON modules | Landed `ee826e889` + fixes `36043391d`, `d0223ff1a` (merge `78cc4f412`) | import corpus delta +12 net / 0 p→f (lane-run 565/172 vs 553/184); loop-3 dual ACCEPT (user-authorized) | `OP_ImportCallWithOptions` JIT lowering (B4); BOM-JSON; debug metadata leg pre-existing |
| S2 arithmetic/bitwise/shift | Landed `1c5d5b2fc` | scoped 444/2, 80/0; broad 1,027/1,027; Node differentials | BigInt64Array/BigUint64Array, Map/Set identity, literal property names, giant ToString, JIT |
| S2 BigInt64Array | Landed `f5f0efef5` | 944/0; 958/1016; ctors 24/24, 166/0; internals 163/0 | DataView BigInt, internals, Map/Set identity, SAB, JIT |
| S2 BigInt collections/DataView | Landed `10154e3f5` | 102/0; 140/4; broad 104/0; class names/DataView length | immutable buffers, SAB, static fields, JIT |
| S2 BigInt default enablement | Landed `c2961de3e` + repairs `66aed0c68`, `cb4e3c720` (merge `f73012a01`) | affected audit 198/3,237 old-default → 2,531/904 default-on, 2,333/0 f→p/p→f; `-ESBigInt-` restores old profile 0/0; serialization/debugger crashes fixed; 4 new entries | JIT/TTD, JsValueType enum, quadratic ToString, B4 |
| S8 resizable/transfer | Landed `6ceb23d6d` | focused 424/834, 236/0; ArrayBuffer 358/442; DataView 44/52; Float16 4/4 | immutable buffers, SAB, Float16Array, cross-realm/Reflect order, JsRT/Wasm, JIT |
| S8 TypedArray tracking | Landed `859b07d13` | focused 132/340, 110/0; combined 150/378, 118/0; expanded 54/0 | BigInt-gated residual; generic Array helpers, toLocaleString element formatting, JIT/TTD |
| S13 WeakRef | Landed `803331b51` | WeakRef 56/58, 56/0; agent-wide kept set; 200/500-run stability | FinalizationRegistry, symbol WeakMap/WeakSet keys, host-dispatch, concurrent GC/JIT |
| S13 FinalizationRegistry | Landed `51787b576` | 94/94; WeakRef 58/58; 94/0; focused 100 | cross-realm Symbol, Reflect.construct double read, host queue, JIT |
| S14 P1 | Landed `a4eba5a38` | 152/4 on integrated binary; 112/0 | S1 class completion; unsupported `using`; pre-existing labeled-break/finally crash (P7) |
| S14 P3 | Landed `0a723970e` | 280/0; full Annex B 765/17, 282/0 | 17 P4 declaration-instantiation collisions |
| S14 P6 | Landed `8b52cc063` | 18/18, 10/0; broad 10/0 | unordered/global/let and dictionary snapshot cost; JIT |
| S14 P4 | Landed `be630c7f6` | 150/0; 112 controls; 38 engine/4 host flips | body eval with non-simple params; R1/R2; JIT |
| S14 P2 | Landed `526931bc5` | 192/0; 157/0; controls preserved | JSC/Node delete difference; JIT |
| S14 hidden-outer eval `arguments` | Landed `d9a1998d1` (merge `13e925a75`) | catch-hidden top-level lexical now SyntaxError; parameter/simple-catch/with/arrow controls preserved; eval-code 454/0; no gate change | JIT/TTD (B4) |
| S14 P5 | Landed `8b5f28c06` | 50/50; broad 50/0; wider 65/0 non-import | eval/function-expression; proxy descriptor; JIT |
| S14 eval-nonsimple-var + catch preflight | Landed `c1b769e1f` + repair `0d1c1e771` + receipts `9b45dd45d`…`216d6160e` (merge `e0751e70d`) | body eval over non-simple params no longer over-reports SyntaxError (post-body-parse scope split, compound guard); eval preflight checks destructured catch bindings for all names (pre-existing gap); param named `arguments` shadows; 1,000+ review shapes Node-exact; legacy fixtures updated; new test entry; gates 2032/2084 → 2033/2085 | compound redeclaration family, pattern-default `[arguments=5]=[]` corner, `arguments.callee` poisoning card, eval-declaration JIT fidelity (separate), B4 |
| C1 negative-phase marker + fresh baseline | Landed `ccd0814c6`…`291d9c45a` + spoof repair `c4f6c1031` (merge `c381c2e37`) | `ch -NegativePhaseReport` origin-classified phase marker; runner re-judges every negative (first-wins + line-1 after adversarial spoof proof); 25 controls; flag-off byte-identical 317/317; fresh full baseline 93,141 → 77,257/15,884, 8,751 marker-judged, 12 downgrades (the known 12), zero natural forgeries; dual ACCEPT after one repair | child link-error propagation + JSON.parse instantiation (engine gaps now exposed), sticky witness, agent-thread/dynamic-import marker-less, certification-grade rerun owed to C4 |
| S12 module phase fidelity | Landed `37725d8c1` + receipt `34296622b`, `d1ad6f0a2` (merge `8f17ba38d`) | child link errors propagate to root declaration-instantiation (first-wins `errorObject`, mirroring OnChildModuleReady) and JSON modules parse at load (`JsRun`-validated synthetic source, exception recorded pre-parse with identity preserved); module-code 524/602→534/602 with 10 wrong-phase downgrades→0; import 28/135→29/135 (json-invalid flips; import-defer parser gap stays); zero pass→fail; gates unchanged 2033/2085 | multi-error dictionary-order selection (pre-existing), stateful global-JSON tamper (host harness), NUL truncation (pre-existing), sticky witness, JIT/TTD out of scope |
| C2 test262-under-JIT campaign | Landed `ee6af9618` + receipt `d1bed69fd`, `38e519225` (merge `37123ee48`) | 19 batches × 3 modes, 57 runs, controls 25×57; **17/19 per-batch JIT obligations discharged at zero JIT-vs-noJIT diff**; 172 diffs = one root cause (C2-JIT-1/2 Set-methods/groupBy reentrancy aborts, **default-flags reachable** — severity-upgraded by review); 20 waitAsync flakes mode-invariant; campaign.py tooling committed for reruns; no engine source changed; dual ACCEPT | C2-JIT-1/2 (→ fix lane below), s12 base drift −2 (→ forensics), Linux JIT via B4 |
| JIT reentrancy fix (C2-JIT-1/2) | Landed `3b609690a` + receipts `f4b97fa59`, `33af3ae2a` (merge `88c11c4d0`) | armed the 7 un-armed C++→JS transitions (Set.cpp ×4, Map.cpp, Object.cpp ×2 — Object.groupBy has its own copy) with the EntryForEach combined idiom; pre-fix 20/20 battery aborts incl. default flags → post-fix 30/30 green with asserted values; exception semantics preserved (close-once, original-error-beats-throwing-return); 51-probe callback sweep clean pre+post; corpora recovered to exact nojit parity (s10 372/0, s11 170/0); **19/19 JIT obligations now discharged on Mac ARM64**; new `setmethods-groupby-warmed.js` entry; gates 2033/2085 → **2034/2086**; dual ACCEPT | Map-groupBy wrapper-arm asymmetry (cosmetic), FRR-cleanup-callback arming unexercisable in host, Linux JIT via B4 |
| JIT closure bundle (audit/test/exclusion) | Landed `102dc188a` + receipts `63356c904`, `fd8554874`, `50c9dc66c` (merge `90f65f75f`) | S8 immutable inlined-store AUDIT complete: safety does NOT hold under JIT — the profile-fed full-JIT fast path silently writes immutable buffers (9 numeric classes, strict no-throw); repros + review-corrected blast radius recorded; strict-generator-args JIT test added (`strict-generator-args-jit.js`, tier-up proven); asm.js/WASM ARM64 exclusion formally recorded (interpreted 2429/397, dynapogo 2004/286, zero failures outside tagged folders, zero signals); gates 2034/2086 → **2035/2087**; dual ACCEPT | immutable-store hole (→ fixed below), try/catch keeps coroutine bodies interpreted, Zig 0.17 pin trap (doc note), Linux via B4 |
| JIT immutable-store fix | Landed `68d52ee6f` + receipts `ad1b9e7a3`, `cf8d8a325` (merge `e3621ff0a`) | `isImmutable` moved into the shared `ArrayBufferBase` header (type-safe for SAB views; flag set only pre-view on fresh allocations); JIT store fast path guards via one byte test routing to the helper (exact interpreter semantics); hole-2 `DirectSetItemAtRange` checks destination upfront (zero partial writes, exactly-once replay); 12-class matrix all clean every mode; perf flat (+0.17 ns/store on store-saturated shapes only); new `immutable-store-jit.js` per-class entry; gates 2035/2087 → **2036/2088**; dual ACCEPT (reviewer dynapogo observations resolved as invocation artifacts; es7 dynapogo 99/0) | immutable-check-before-ToNumeric (pre-existing), sloppy-detached-throws TypeError (pre-existing card), x64/Linux execution via B4, view-bit perf sketch |
| B5 bytecode GUID rotation | Landed `59ddac730` + receipts `6c78f414b`, `3b148c014` (merge `8961cdec4`) | dual-variant regen from restored-baseline headers; tool-minted GUID `d31c196c…`→`55c5da6d…`; regenerated headers byte-identical (net change = 2-line GUID header); stale-cache rejection proven at source + runtime (both cross-variant directions, third-GUID control, end-to-end deserialization); gates 2036/2088 + jit check green; dual ACCEPT | single-variant `--binary` overwrite hazard (tooling guard required before B5 package/floor), Linux case-sensitivity of the mint path, stale pre-rotation out/ copies, B5 package/floor scope open |
| dbg-async-attach (suspended async resume) | Landed `26055a0c2` + receipts `c7de4bf50`, `e906a4a49` (merge `7f91cea0b`) | inverted separate-body: the ORIGINAL body stays whole as suspended frames' execution body; the debug reparse gets a fresh body taking over function identity (deferred-parse promotion mechanism; frame-body reads for yield register/IsDone/inner scopes); repro 20/20 aborts → 25/25 clean across 3 profiles; AsyncDynamicAttach locals baseline intact (breakpoints fire on fresh-body invocations); Debugger suites green; new `AsyncSuspendedResume.js` entry; gates 2036/2088 → **2037/2089**; dual ACCEPT | TLA/global-body no-debug-bytecode until completion (benign), assert keys on state not site, OOM rollback analysis-only, non-Apple targets keep original bug, GC-pressure validation blocked by the continuation-drop defect (→ next lane) |

Full gates at this checkpoint: **2037/0 test**, **2089/0 debug-static**
(single-sourced in `tools/build/gate_counts.json`; repair-loop
`-noDeferParse` rlexe variants moved 2013/2065 → 2016/2068, S6a added
one entry, eval-super added three parse-mode entries, S6b added the
waitAsync/pause entry, S8 immutable added one entry, S3-a property
escapes added one entry, S3-b `v` mode added one entry, S9 u-mode
strictness added one entry, S6c BigInt Atomics added one entry, S2
BigInt default enablement added four entries, S6e cross-agent
waitAsync added one entry, the host-broadcast-type repair added one
entry (2031/2083 → 2032/2084), and the s14-eval-nonsimple-var lane
added one entry (2032/2084 → 2033/2085), the jit-reentrancy-fix lane added
one entry (2033/2085 → 2034/2086), the jit-closure-1 lane added one
entry (2034/2086 → 2035/2087), and the jit-immutable-store lane added one
entry (2035/2087 → 2036/2088)). Integrated
class-elements corpus re-runs 2026-10-20: wave-2
(`out/test262-c2-s1-wave2`) **5,854/5,897**, 3,865 f→p / 0 p→f vs
2026-10-19; wave-3 (`out/test262-c2-wave3`, after the S1-tail lane)
**5,888/5,897 passed (9 failed)**, +34/0. With BigInt now default-on,
the 2026-10-29 rerun (`out/test262-c2-s1-default-bigint`) is
**5,892/5,897**: the four former `-ESBigInt` parse-barrier scenarios
pass; the four post-ES2025 `accessor` grammar scenarios and one Stage-1
non-extensible scenario remain. All active feature/triage worktrees are
merged. The separate E1 engine lane fixed the pre-Quit multi-agent
startup SIGBUS (card-table stack coverage; merge `3574b6168`, gates
unchanged), and the S6e FIFO resolution-order repair restored guaranteed
per-agent FIFO fulfillment across a foreign notify
pass (per-notify-pass ordered chain + head-first registrant claims;
30/2400 pre → 0/2400 post; merge `02c45f214`, gates unchanged). The
end-of-main join card was then fixed by the host-exit-join lane (merge
`5f4e722d1`): the PAL caps one wait at 64 handles, so >64 accumulated
agent children made the old join fail (debug assert; release-profile
delete-without-join UAF, 33/50 SIGSEGV at 100 agents) — the fix joins
each child individually and deletes only after observing exit (post-fix
50/50 clean, 400-agent adversarial scale clean, gates unchanged
2031/2083). The unchecked-`CreateThread` follow-up was then closed by
the host-spawn-check lane (merge `a77b1a49d`): a failed agent-thread
spawn used to hang the parent forever on the initial-script event
(reproduced 12/12 per profile via a libc-boundary `EAGAIN` injection);
the fix rolls the failed child back and surfaces a catchable
`JsErrorOutOfMemory` (12/12 caught, post-failure spawns consistent,
dual ACCEPT, gates unchanged). The `WScript.Broadcast` type-validation
card was then closed by the host-broadcast-type lane (merge
`da9911395`: non-SAB argument now throws instead of SIGSEGVing with
live agents; balanced agentless acquire-and-release after a
review-found leak; new `s6-broadcast-type.js` entry moved the gates to
2032/2084; dual ACCEPT after one repair iteration). The S14
eval-residuals lane then closed the P4 body-eval item (merge
`e0751e70d`: non-simple-param body-eval vars, catch-binding preflight,
`arguments`-param shadowing; gates 2032/2084 → 2033/2085). The
c1-negative-phase lane then delivered the host-reported negative phase
and a fresh marker-era full baseline (merge `c381c2e37`; see §3 update
and [c1-negative-phase.md](c1-negative-phase.md)). The two phase-exposed
engine gaps were then closed by the s12-module-phase lane (merge
`8f17ba38d`, receipt [c2-s12-module-phase.md](c2-s12-module-phase.md)):
child module link errors now propagate to root declaration-instantiation
and JSON modules parse at load — module-code 534/602 with all 10
wrong-phase downgrades eliminated, `json-invalid` genuine, zero
pass→fail, gates unchanged 2033/2085, dual ACCEPT. Next work items are
the test262-under-JIT per-batch campaign, the JIT follow-ups, the B5
bytecode-GUID rotation, and the certification-grade C4 full rerun once
those land. P5 residual: eval/function-expression
reference paths and pre-existing proxy descriptor duplication.

## 8. Separate inventories

**Engine defects on existing features** — the 3,822-failure defect bucket
and S14; includes the 327 executed parse negatives and failing negatives
generally. These do not need new features, only correctness work. The
first dedicated engine-defect lane is closed: E1 fixed the card-table
stack-coverage gap behind the agent-startup SIGBUS
([e1-agent-startup-sigbus.md](e1-agent-startup-sigbus.md)).

**Disabled-feature incompleteness** — default-off flags named in the
policy, with `ConfigFlagsList.h` anchors: `ESSharedArrayBuffer` (false at
:709 — measured 2026-10-15 in [c2-flag-audits.md](c2-flag-audits.md) §S6:
zero regressions; defaulting is a product/security decision),
`ES6RegExSymbols` (false at :692-696 — protocol appears when enabled,
JIT-helper warning at :1177), `ES6RegExPrototypeProperties` (false at
:687-690 — no feature tag; measured only by before/after runs). BigInt is
now default-on with `-ESBigInt-` as the explicit opt-out. The disabled-flag
bucket is being reforecast for the remaining flags; the historical BigInt
portion is closed by [c2-s2-bigint-default.md](c2-s2-bigint-default.md).
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

## 9. Reforecast and open items

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
- Fixture-cache-affected subsets rerun: completed on 2026-10-21 with the
  fixed harness and per-agent temp isolation over the exact 30 directories
  (2,365 scenarios): **1,367 passed / 998 failed**, **0 pass→fail / 102
  fail→pass** against the old precombined result. See
  [c4-fixture-rerun.md](c4-fixture-rerun.md). This removes the pre-fix
  baseline ambiguity for these directories but does **not** close the
  host-negative-phase gap or certify C4.
- Enabled-flag test262 runs for `ESBigInt`, `ESSharedArrayBuffer`,
  `ES6RegExSymbols`, `ES6RegExPrototypeProperties` (S2/S6/S7) — measured
  2026-10-15 in [c2-flag-audits.md](c2-flag-audits.md); the S7 follow-up
  (matchAll + protocol fixes) landed as C2-S7
  ([c2-s7-matchall.md](c2-s7-matchall.md)).
- B4 native x64 execution, which every "JIT-x64? yes" item waits on for
  its C2 completion gate.
- The Unicode/ICU data-version decision behind S3 (and plan P1 Intl).
