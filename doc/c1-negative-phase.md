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
only on the host thread at existing error-report sites. It retains no JsRT
handles, performs no allocation, introduces no GC-visible state, changes
no ownership, callback registration or release path — no ownership record
is required beyond the code comment at the state definition.

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
- **Crashes, timeouts and abnormal exits emit no marker** (the process dies
  before any error report). The runner falls back to the shape heuristic
  for those records and records `judged_by: 'shape'`.

## 2. Runner integration (`tools/test262/run.py`)

- Default `--host-flags` is now `-nonative -Test262 -CollectGarbage
  -NegativePhaseReport`; the marker is captured per scenario from
  `rawResult.stderr` (no `harness.js` change needed — eshost already
  records the raw stderr the shape heuristic read).
- **Every** negative scenario (controls and corpus) is re-judged by
  `negative_phase_judgement()`: marker phase must equal the declared
  `negative.phase` (plus no stdout for parse/resolution). The C1 shape
  heuristic remains **only** as the fallback when the marker is absent
  (`judged_by: 'shape'`).
- A harness pass that fails re-judging is **downgraded** to
  `pass: false` with `result.message` starting `Negative phase mismatch:`.
- Schema additions (documented, additive only): per-negative-record
  `phase_evidence` (`declared`/`marker`/`shape`/`executed`/`judged_by`),
  `summary.json` section `negative_phase` (`downgraded_pass_to_fail`,
  `judged` counts), updated `limitations` text.

## 3. `analyze.py`

The negative-phase audit is marker-aware: marker-judged records are clean
by construction (their shape is not evidence), marker stats are counted per
phase (`marker.judged`/`marker.judged_passed`), run.py's downgrades are
listed (`marker_downgraded_by_run_py`, from result messages), and legacy /
marker-less records keep the exact C1 shape-audit semantics. Verified: on
the recorded C1 baseline (no markers) every audit field, bucket and
aggregate matches the committed `c1-baseline-analysis.json` — the only
differences in a fresh run are the pre-existing edition-policy drift
(BigInt default-enablement re-buckets `disabled-flag` vs
`required-feature-missing`) plus the new zero-valued marker fields.

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

**Controls.** `run.py --controls-only`: **24/24 scenarios** (20 C1
controls + 4 marker controls), stable across repeated runs. The two
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
**2085/0**.

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

1. **Full pinned-corpus baseline rerun** — the next step, not this lane: a
   fresh `run.py` invocation over the default patterns into a new output
   dir, then `analyze.py`, giving the first edition-selected score whose
   negatives are phase-proven. Until then the §2 C1 headline numbers stand
   (with the 12-suspect sensitivity now measured concretely above).
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

