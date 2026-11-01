# C1/C4 — Host-reported negative phase

Recorded 2026-11-01 on branch `c1-negative-phase` (base `b490dc6de`, gates
2033/2085). This lane closes the C4-preparation gap identified in
[doc/c1-triage.md](c1-triage.md) §3: the runner used to accept a negative
test whenever the thrown error's *name* matched `negative.type` and could
only re-judge 18 built-in controls through host-specific output-shape
heuristics. Now `ch` **reports the error phase explicitly** and the runner
re-judges **every** negative corpus scenario with it.

## 1. The host marker

`ch` gains a debug-configuration host flag (registered in
`bin/ch/HostConfigFlagsList.h`, parsed case-insensitively like every other
host flag):

```
-NegativePhaseReport
```

When set, the host emits exactly **one** machine-readable marker line on
**stderr** (stdout baselines stay untouched) at the **first unhandled-error
report**, before the human-readable error report:

```
CH_NEGATIVE_PHASE: parse|resolution|runtime
```

Emission is idempotent per process: the first terminal report classifies
the run; later reports do not repeat the marker. With the flag **off** the
host is byte-identical to the base build (proof in §4): every witness and
emission site is guarded by the flag, and no other behavior changed.

### Classification mapping (phase → where it comes from, with source anchors)

The classification uses **where the failure originated** — which JsRT entry
point failed, at which stage, plus host-side stage witnesses — never error
message text, error names, or stack shapes.

| Phase | Signal (engine-authored or host-authored stage fact) | Source anchor |
|---|---|---|
| `parse` | Root entry point failed in the parse/compile stage: the main `JsRun` (or `JsRunSerialized`/`JsRunScriptWithParserState`) returned `JsErrorScriptCompile` — the engine rejected the root script source before any code executed (`RunScriptCore` returns `JsErrorScriptCompile` only from `HandleScriptCompileError` on the compile wrapper; the execution wrapper converts evaluation failures to `JsErrorScriptException`) | host `bin/ch/ch.cpp` `RunScript` (marker call before `PrintException`); engine `lib/Jsrt/Jsrt.cpp:3680-3687`, execution at `:3729-3743` |
| `parse` | Root module's source rejected at parse: `JsParseModuleSource` returned `JsErrorScriptCompile` (`LoadModuleFromString`'s loud path returns it to the main entry point) | host `bin/ch/WScriptJsrt.cpp` `LoadModuleFromString`; engine `lib/Jsrt/Core/JsrtCore.cpp:101-105` |
| `resolution` | Engine completed module resolution **unsuccessfully**: `NotifyModuleReadyCallback` invoked with a non-null exception (the engine calls `NotifyHostAboutModuleReady` after `ModuleDeclarationInstantiation` — parse+link of the graph — with the record's `errorObject`, before any module-body evaluation) | host `bin/ch/WScriptJsrt.cpp` `NotifyModuleReadyCallback`; engine `lib/Runtime/Language/SourceTextModuleRecord.cpp:371-409` (`PrepareForModuleDeclarationInitialization`), `:909-954` (`ModuleDeclarationInstantiation`, where `JSERR_ModuleResolveImport/Export` is set, `rterrors.h:334-335`) |
| `resolution` | A dependency module failed to **parse** while the graph was loading (the error stays on the child record for the link phase; only the *root* module's own parse error is parse-phase) | host `LoadModuleFromString`'s silent branch (non-root `JsParseModuleSource` failure → `NoteModuleGraphFailure`) |
| `resolution` | Host fetch/loader failure during graph loading: dependency file unreadable (`ModuleMessage::Call` fetch arm), JSON-module wrap OOM, specifier coercion failure, `_fullpath` failure, unsupported import-attribute `type`, module-record initialization failure, fetch-message allocation failure | host `bin/ch/WScriptJsrt.cpp` `ModuleMessage::Call`, `FetchImportedModuleHelper(WithAttributes)` |
| `runtime` | Everything else: `JsRun` returning `JsErrorScriptException` (script body, runtime `eval` SyntaxErrors, `$262.evalScript`/`WScript.LoadScript` nested-parse failures that propagate as exceptions, global declaration-instantiation errors the engine raises during evaluation), `JsModuleEvaluation` failures (module body throws), module-completion exceptions (incl. top-level-await rejections), message-queue callback errors | host `ch.cpp` main entry; `WScriptJsrt.cpp` `ModuleMessage::Call` eval arm, `ReportModuleCompletionCallback`, `CallbackMessage::~CallbackMessage`/`CallFunction` |

Precedence: a main-entry `JsErrorScriptCompile` is `parse`; otherwise a
module-graph witness makes it `resolution`; else `runtime`.

**Memory-management note (per doc/memory-management.md):** this change
adds one host flag and two process-global native `bool`s, written and read
on the main host thread at existing error-report sites. It retains no JsRT
handles, performs no allocation, introduces no GC-visible state, changes
no ownership, callback registration or release path — no ownership record
is required beyond the code comment at the state definition. (The bools
are unsynchronized; see §5.7 for the theoretical S6e agent-thread caveat.)

### Honest limits (what the host still cannot distinguish)

- **Deep-child link errors classify as `runtime`.** A link error in a
  *dependency* module (e.g. the fixture itself imports an unresolvable
  name) never reaches the host through `NotifyModuleReadyCallback` (the
  engine only links root/promise modules there and ignores child results
  in the root's recursion), so no resolution witness fires; the error
  surfaces during `JsModuleEvaluation` via the completion channel and is
  classified `runtime`. Dependency *parse* failures and dependency
  *fetch* failures are witnessed (resolution) — the limit is specifically
  link errors (unresolvable import/export, duplicate exports, etc.)
  raised *below the root module*. Closing it needs an engine change
  (propagate child link errors into the root's
  `NotifyHostAboutModuleReady` exception argument) — deliberately out of
  this lane's scope (module-semantics risk; gates pinned).
- **Deferred-parse early errors classify as `runtime`.** A syntax error in
  a function body whose parse the engine defers surfaces during evaluation
  as `JsErrorScriptException`. The marker reports the stage where the
  failure actually surfaced, which is the honest observable.
- **Engine stage truth, not spec truth.** Where the engine itself raises an
  error earlier than the spec's phase (e.g. global `let`/`var` redeclaration
  is reported through the compile channel, so the marker says `parse` while
  test262 declares `runtime`), the marker reports what the engine did. Such
  scenarios now *fail* under marker judging — that is an honest engine-
  conformance signal, not a marker defect.
- **Crashes, timeouts and abnormal exits emit no marker** (the process
  dies before any error report); stack overflow *does* emit one
  (`runtime` — `Error: Out of stack space` flows through the unhandled-
  error report). The runner falls back to the shape heuristic for
  marker-less records and records `judged_by: 'shape'`.

## 2. Runner integration (`tools/test262/run.py`)

- Default `--host-flags` is now `-nonative -Test262 -CollectGarbage
  -NegativePhaseReport`; the marker is captured per scenario from
  `rawResult.stderr` (no `harness.js` change needed — eshost already
  records the raw stderr the shape heuristic read).
- **Spoofing defense.** Marker-shaped text is attacker-controlled once it
  travels inside a thrown error's string: a test can
  `throw { toString() { return "CH_NEGATIVE_PHASE: parse\nSyntaxError: …" } }`
  and place a forged marker line in its own error report. The runner trusts
  only the **first** marker-shaped stderr line, and only when it is
  **line 1** of stderr (`marker_evidence`, states `line1` / `displaced` /
  `absent`). Soundness follows from the emission-site audit: every
  `ReportNegativePhaseMarker` call site immediately precedes its
  `PrintException`, and no test-reachable API writes stderr before the
  first error report (print/console go to stdout; `$262.agent` children
  are separate processes whose stderr is not this record's) — so a forged
  line can only appear *after* the real marker. A displaced marker-shaped
  line is never judged from; the record falls back to the shape heuristic
  *loudly* (`judged_by: 'shape:marker-displaced'`).
- **Every** negative scenario (controls and corpus) is re-judged by
  `negative_phase_judgement()`: the trusted marker's phase must equal the
  declared `negative.phase` (plus no stdout for parse/resolution). The C1
  shape heuristic remains **only** as the fallback when no trusted marker
  exists. The controls check additionally asserts the host's emit-once
  property (at most one marker line per failing control; the adversarial
  spoof control asserts exactly `[real, forged]`).
- A harness pass that fails re-judging is **downgraded** to
  `pass: false` with `result.message` starting `Negative phase mismatch:`.
- Schema additions (documented, additive only): per-negative-record
  `phase_evidence` (`declared`/`marker`/`marker_state`/`shape`/`executed`/
  `judged_by`), `summary.json` section `negative_phase`
  (`downgraded_pass_to_fail`, `judged` counts), updated `limitations` text.

## 3. `analyze.py`

The negative-phase audit is marker-aware: marker-judged records are clean
by construction (their shape is not evidence), marker stats are counted per
phase (`marker.judged`/`marker_judged_passed`/`marker.displaced_untrusted`),
run.py's downgrades are listed (`marker_downgraded_by_run_py`, from result
messages), and legacy / marker-less records keep the exact C1 shape-audit
semantics. Verified: on the recorded C1 baseline (no markers) every audit
field, bucket and aggregate matches the committed `c1-baseline-analysis.json`
— the only differences in a fresh run are the pre-existing edition-policy
drift (BigInt default-enablement re-buckets `disabled-flag`,
`required-feature-missing` *and* `engine-defect-on-existing`) plus the new
zero-valued marker fields.

Note: `--controls-only` writes `controls.json` as the raw harness stream —
control records do **not** carry `phase_evidence` (the control check
computes it inline and aborts on any mismatch); the evidence-bearing schema
applies to `results.json` corpus records.

## 4. Evidence

All runs on the worktree binary (base `b490dc6de` + this lane, test
profile); corpus = the pinned `2e0a5676` checkout; gates through the shared
lock. *(observed)*

**Marker battery (flag on, direct host runs).** Ten hand-made scenarios
classify exactly per the mapping — script parse → `parse`; script/module
body throw, runtime `eval` SyntaxError → `runtime`; dependency-module parse
error, engine link error (unresolvable import at root), missing fixture
file → `resolution` — each emitting exactly one marker line, before the
human-readable error report; five spot-checks with the flag off emit no
marker. `let x; var x;` at global scope reports `parse` (the engine raises
it through the compile channel — an honest stage report of an engine
conformance choice test262 files as `runtime`).

**Flag-off byte identity.** `out/scratch/byte-identity.py` compares the
base build (`b490dc6de`, saved pre-change) against the lane build with the
flag off, byte-for-byte on stdout+stderr+exit code: **317 cases
identical** — 14 targeted error scenarios (script parse/runtime/eval/
redeclaration; module dependency-parse, stackless throw, body throw, link
error, missing file; four of those again under
`-nonative -Test262 -CollectGarbage`) plus all 303 `test/es6/*.js` and
`test/es7/*.js` fixtures (the es6/es7 error fixtures included).

**Controls.** `run.py --controls-only`: **25/25 scenarios** (20 C1
controls + 4 marker controls + the §6 spoof-defense control), stable
across repeated runs. The two
deliberately-misleading controls are harness-*passed* (error name matches)
and caught only by the marker: `fail-marker-resolution-stackless` /
`fail-marker-parse-stackless` (a stackless module-evaluation throw whose
`thrown at` loader shape the C1 heuristic accepts for resolution/parse) →
marker `runtime`. `pass-marker-resolution-depparse` proves the
dependency-parse witness (marker `resolution`).

**Focused selections (marker re-judging active).**

| Selection | Result | Negatives marker-judged / downgraded |
|---|---|---|
| `test/language/eval-code/**` | 454/454 | 4 / 0 |
| `test/language/import/**` | 28/135 | 19 / 2 |
| `test/language/module-code/**` | 524/602 | 205 / 10 |

analyze.py on the module-code run: 185 passed negatives, **0 shape
suspects** (marker-judged records are clean by construction), 10 downgrades
listed under `marker_downgraded_by_run_py`.

**The 12 recorded C1 suspects, re-judged** (11 edition-selected + the
excluded `import-defer` case):

| Class | Scenarios | Outcome |
|---|---|---|
| Fixed by S12, now genuinely passing | `json-named-bindings.js` (1) | root-level link error now surfaces at resolution; marker `resolution` = declared. The recorded shape evidence (`empty-stack`) could never prove this; the marker does. |
| Still wrong-phase (unchanged cause) | `import-defer-of-syntax-error-fails.js` (1, post-ES2025-excluded) | parser still rejects `import defer` syntax at parse; marker `parse` ≠ declared `resolution` → downgraded (correctly; the shape rule catches this one too) |
| Now-correctly-caught, new wrong-phase kind | `json-invalid.js` + 9 attribute-shape tests (`allow-nlt-before-with`, 8× `import-attribute-*`) (10) | S12 parses the syntax, but the failure now surfaces **during module evaluation** (marker `runtime`) where resolution is declared: `json-invalid` executes `JSON.parse` in the host's synthetic JSON module at evaluation; the 9 attribute tests fail through `ensure-linking-error_FIXTURE.js`, a **child-level** link error (the fixture imports a non-existent binding from itself — reproduced standalone in `out/scratch2`), which this engine raises only when the child is evaluated, not at root instantiation. All 10 carry the stackless loader shape that the C1 heuristic accepts as resolution evidence — **no output-shape heuristic can catch these; the marker does** |

Plus one marker-only catch outside the recorded suspects:
`instn-star-err-not-found.js` (namespace resolution deferred to evaluation;
the spec's step the test quotes — `GetModuleNamespace` during instantiation
— should raise it at resolution). Counterfactual on the same records: of
the 12 downgrades, the shape rule would also have failed only 2
(`json-invalid` runtime-frame, `import-defer` parse-frame); **10 are
marker-only catches**.

**Gates (pins untouched, no rlexe edits).** `zig build check-regression`
(test): **2033/0**. `zig build -Dprofile=debug-static check-regression`:
**2085/0**. The repair iteration re-ran the test gate after runner-only
changes: **2033/0** again (nothing host-side changed, so the debug gate was
not repeated).

**Tooling finding.** eshost's `ConsoleAgent.js:131-140` skips dependency
discovery for `phase: parse` + `type: SyntaxError` negatives ("if the main
test file isn't a negative SyntaxError, check for importable files"), so a
parse-declared module test never gets its fixtures written and fails with a
loader `URIError` instead. Dependency-based parse masquerades cannot be
expressed through this harness; the parse-shape marker control is
stackless-throw-based instead. Recorded here because it also means corpus
parse-phase module negatives with imports always fail on error *name*
upstream — independent of phases.

## 5. Residuals and what C4 still owes

1. **Full pinned-corpus baseline rerun — completed** (2026-11-01 05:21,
   this lane's binary, fresh `out/test262-np-baseline`, marker judging
   active; an earlier mid-stream observation that the run had been
   interrupted was a mis-observation of the timeout-killed *build*
   wrapper — the baseline run itself ran to completion and its results
   stand): broad **77,257 / 15,884 (82.95%)** of 93,141 scenarios,
   controls 24/24 (pre-spoof-control), edition-selected **76,453 / 4,091
   (94.92%)** of 80,544 scenarios. This is the first full-corpus result
   whose passing negatives are phase-proven: 8,673 marker-judged
   harness-passing negatives (8,661 still passing), 0 shape suspects, and
   exactly **12 downgrades corpus-wide — the same 12 characterized in §4**
   (no additional wrong-phase pass exists anywhere in the corpus; 79
   negatives were judged by the shape fallback only because the harness
   failed them without an error report). The numbers are not comparable
   to the recorded C1 headline (61,565/31,576; this base includes every
   S1–S14 batch landed since); C4's certification decision now has a
   phase-qualified starting point plus the engine follow-ups below.
2. **Engine follow-up (closes the 10 marker-only catches honestly at the
   source):** propagate child-module link errors into the root's
   declaration-instantiation so `NotifyHostAboutModuleReady` carries the
   exception (the JsRT contract already supports it). That single engine
   fix turns the 9 `ensure-linking-error`-shaped tests plus
   `instn-star-err-not-found` into resolution-stage failures the host then
   reports as `resolution`. JSON modules additionally need `JSON.parse`
   moved from module evaluation to load/instantiation.
3. **Host/API limits that remain even then:** deferred-parse early errors
   (surface at evaluation → `runtime`); engine errors raised earlier than
   the spec phase are reported at the engine's stage (an honest
   conformance signal, e.g. compile-channel global redeclaration); a
   hypothetical non-root link failure the engine only detects at
   evaluation stays `runtime` until (2) lands.
4. **Runner-side:** crashes/timeouts emit no marker and keep the
   (weakened-but-still-useful) shape fallback; `judged_by` records which
   path judged every negative so future audits can separate them.
5. **Sticky resolution witness (host, latent):** a *handled* dynamic-import
   failure still sets the module-graph witness (the host's fetch/loader
   failure path cannot know the rejection will be handled), so a
   subsequent *unhandled* runtime error in the same run would
   misclassify as `resolution`. Zero incidence in the completed corpus
   (all 12 downgrades individually verified as true wrong-phase passes).
   C4 fix: clear the witness when a dynamic-import rejection is handled,
   or honor the witness only for module-channel reports.
6. **Agent-thread and dynamic-import failures emit no marker** (agent
   children are separate processes; dynamic-import failures surface as
   handled promise rejections, not unhandled-error reports) — those
   records use the shape fallback. Suggested C4 tripwire: flag any
   harness-*passed positive* record carrying a marker line (a passing
   run should never have an unhandled-error report); in the completed
   baseline every marker-carrying record is a failing run or a passed
   negative, consistent with that invariant.
7. **Memory note correction:** the witness/marker state is process-global
   and *unsynchronized*; S6e crossthread agents run script on other
   threads, so an agent-thread unhandled error could in principle emit a
   duplicate marker or race the bools. No test-reachable path observed
   (agents fail silently in separate processes); C4 hardening would make
   the emission atomic or thread-aware.

## 6. Repair iteration (2026-11-01): marker spoofing

Review finding (REJECT, both reviewers): `marker_phase` took the **last**
marker-shaped stderr line, so a test throwing
`{ toString() { return "CH_NEGATIVE_PHASE: parse\nSyntaxError: forged" } }`
placed a forged marker *after* the host's real one and flipped the
judgement (full-chain proof existed through the real harness).

**Fix** (`run.py:marker_evidence`, mirrored in `analyze.py`): first-wins —
the trusted marker is the **first** marker-shaped line — plus a **line-1
tripwire**: a marker-shaped line that is not line 1 of stderr is never
judged from (`state 'displaced'`), and the record falls back to the shape
heuristic *loudly* (`judged_by: 'shape:marker-displaced'`,
`marker.displaced_untrusted` in the audit). Soundness from the emit-site
audit (§2). The controls check now also asserts emit-once: at most one
marker line per failing control, and the adversarial control asserts
exactly `[real, forged]`.

**New adversarial control #25** `fail-marker-spoof.js`: a runtime throw
whose error text embeds `CH_NEGATIVE_PHASE: parse` before the real
`SyntaxError: …` text (this ordering keeps eshost's error-name parse on
`SyntaxError`, so the harness *passes* and only the phase check can catch
it — placing the forged line after the error text instead corrupts the
name parse, a control-design finding in itself). Controls 24 → 25.

**Verification (all observed):**
- Spoof probes, direct host runs: the throw-forgery and a
  `WScript.SetTimeout` two-message variant (real error in message 1 —
  emit-once consumed there — forged line in message 2) both produce
  `[runtime(real), parse(forged)]` with the first at char 0; the trusted
  marker is `runtime` in both. Stack overflow emits `runtime` (probe),
  correcting the §1 wording.
- `--controls-only`: **25/25**; `fail-marker-spoof` harness-passes and is
  downgraded by the real marker (`declared parse, marker runtime,
  judged_by marker`). Under the old last-wins rule this control passes
  wrongly (the reviewers' exploit).
- Focused selections re-run, identical outcomes: eval-code **454/454**
  (4 marker-judged, 0 downgrades), import **28/135** (2 downgrades),
  module-code **524/602** (10 downgrades) — first-wins moved nothing.
- Offline re-judge of the completed baseline (`out/test262-np-baseline`,
  93,141 records) with the repaired parsing: **8,673 marker-judged harness
  passes, 78 marker+harness-failed, 1 shape+harness-failed, the identical
  12-downgrade file set, 0 displaced markers** — no natural corpus test
  emits a marker-shaped line before the host's (23,517 marker-carrying
  records, every marker at line 1, zero multi-marker).
- Test gate after the repair: **2033/0**.

