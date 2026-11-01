# C2-S12 follow-up: module phase fidelity for link errors and JSON modules

Recorded 2026-11-01 on branch `s12-module-phase` (base `f3ef8cdb0` = main;
no-JIT gates 2033 test / 2085 debug-static at base). The `ch
-NegativePhaseReport` marker (c1-negative-phase) exposed two engine gaps
where spec-declared **resolution**-phase errors were raised only at
**evaluation**, leaving 10 corpus scenarios wrong-phase
([c1-negative-phase.md](c1-negative-phase.md) §"honest limits", §5.2;
[c1-triage.md](c1-triage.md) §3 update). This lane closes both gaps at the
source. The JIT (`jit` profile merged on main) is out of scope; all
verification is no-JIT per the S12 stance.

## 1. Spec analysis

### 1.1 Child link errors must propagate to the root's instantiation

ES2025 16.2.1.5 *ModuleDeclarationInstantiation* (via *InnerModuleLink*):
when a module links its requested modules, a dependency's failed link **is
the requesting module's failed link** — for each required module the
instantiation recursion returns the dependency's abrupt completion, so the
root's `NotifyHostAboutModuleReady`-equivalent carries the exception and
**no module body of the graph evaluates**. `Module import/import-and-export
resolution` errors (`JSERR_ModuleResolveImport`/`JSERR_ModuleResolveExport`,
`rterrors.h:334-335`) are link errors wherever in the graph they originate.

### 1.2 JSON modules parse at load, not evaluation

With import-attributes JSON modules, the host parses the document when the
module is **loaded** (*ParseJSONModule*: `JSON.parse(source)` then
`CreateDefaultExportSyntheticModule(json)`), as test262 `json-invalid.js`
states (`phase: resolution, type: SyntaxError`). A malformed document is a
load/link failure of the graph; the synthetic module's body (evaluating the
already-parsed value) never runs.

## 2. Root causes (with anchors)

### 2.1 Gap 1 — the root's instantiation recursion discards child results

`lib/Runtime/Language/SourceTextModuleRecord.cpp`
`ModuleDeclarationInstantiation()` (:909-954 at base): after initializing
its own exports/imports, the root recurses `childModuleRecord->
ModuleDeclarationInstantiation()` for each child (:929-936) and **discards
the result**. A child's link error never throws:
`InitializeLocalImports` (:1451-1474) and `InitializeIndirectExports`
(:1581-1608) record the error on the child's own `errorObject`
(`JSERR_ModuleResolveImport`/`JSERR_ModuleResolveExport` SyntaxErrors) and
simply return. So the root reports ready with `errorObject == nullptr`
(`PrepareForModuleDeclarationInitialization` :371-412 →
`NotifyHostAboutModuleReady(this, nullptr)`), the host evaluates the graph,
and `ModuleEvaluationPrepass` (:1013-1076) rethrows the child's error at
:1058-1063 — an **evaluation**-channel failure (marker `runtime`).

Repro shapes (probe suite `out/s12phase/`, engine build at base):
- depth 2/3/N, multiple failing children, failing sibling of an evaluating
  sibling: marker `runtime`, no stdout (module bodies are lazy generators;
  `ModuleEvaluationPrepass` walks all children before any body resumes, so
  *in this engine* the wrong-phase failure had no evaluation side effects
  either — the observable phase distinction is the report channel/marker,
  not stdout);
- the exact corpus shape (root **side-effect**-imports a fixture that
  imports a non-existent binding from itself,
  `ensure-linking-error_FIXTURE.js`): `runtime` + `SyntaxError: Module
  import nonExistent cannot be resolved`;
- depth 1 with a *named* root import of the failing module was already
  `resolution` — the root's own `InitializeLocalImports` resolves the
  binding itself; the gap is precisely errors raised **below the root**.

### 2.2 Gap 2 — the host's synthetic JSON module parses at evaluation

`bin/ch/WScriptJsrt.cpp` `WrapJsonModuleSource` (:2544-2598 at base)
synthesizes `export default JSON.parse('<escaped file text>')`;
`ModuleMessage::Call`'s fetch arm (:2409-2436) parses that source when the
graph loads, so the module **links** fine and `JSON.parse` executes only
when the synthetic body is evaluated — a `SyntaxError` at evaluation
(marker `runtime`). `json-invalid.js` is the corpus case; BOM'd documents
(`p8`/`p8b`) fail the same way because this engine's `JSON.parse` rejects a
leading U+FEFF (ES2025 JSON whitespace does not include U+FEFF — an
engine-conformance detail, unchanged by this lane; only its *phase* moves).

## 3. Designs

### 3.1 Gap 1 — propagate the child's recorded link error in the recursion

In `ModuleDeclarationInstantiation`'s children loop, after each child's
instantiation, propagate the child's `errorObject` into `this->errorObject`
(first-wins, matching `OnChildModuleReady`'s existing propagation
:414-453). No new throw path, no new allocation: the child's error object
is a `Field(Var)` recycler value already rooted by the child record.

**Ownership record (doc/memory-management.md) — propagated exception:**

```text
Resource / creation call / source location:
  the child module's link-error Var (JavascriptError from
  InitializeLocalImports/InitializeIndirectExports,
  SourceTextModuleRecord.cpp:1466-1468 / 1601-1603), re-stored on the
  parent record at ModuleDeclarationInstantiation's children loop.
Domain and allocator instance:
  Recycler (JS value) — scriptContext->GetRecycler() via
  scriptContext->GetLibrary()->CreateSyntaxError()/CreateReferenceError().
Physical allocation base/extent/release:
  ordinary recycler object; no native free; released by GC.
Logical length / borrowed views: none (opaque Var).
Owner; transfer/publication point:
  child SourceTextModuleRecord.errorObject (Field(Var)) → parent
  SourceTextModuleRecord.errorObject (same GC domain); published only by
  plain Var assignment, identical to OnChildModuleReady's propagation of
  parse/fetch errors (:421-424) — no new rooting mechanism.
Borrow invalidation: GC only; both endpoints are traced fields of
  recycler-allocated module records (records themselves rooted via the
  library's ModuleRecordList, SourceTextModuleRecord::Create:49-62).
Potential user-code/reentrancy points: none new — the assignment happens
  in the same synchronous instantiation walk that already sets errorObject.
Thread/context allowed: the script thread/context of the module graph
  (unchanged; MDI runs on the parsing thread).
Release on success/failure/OOM/shutdown: nothing to release (GC-owned);
  on failure the value is surfaced through NotifyHostAboutModuleReady /
  ReportModuleCompletion as before.
Memory accounting: none (no native allocation).
Tests: probe battery §5 + corpus flips §6; debug-static gate exercises the
  path under assertions.
```

### 3.2 Gap 2 — validate the JSON document in the fetch/load arm

`WrapJsonModuleSource` is refactored into a shared
`BuildJsonParseSource(text, prefix, suffix)` (identical escaping). In
`ModuleMessage::Call`'s `isJsonModule` arm, before parsing the synthesized
module, the host builds `JSON.parse('<same escaped text>')` and runs it
(`JsRun`) in the current context:

- **valid** → proceed unchanged (the synthetic body re-runs `JSON.parse` at
  evaluation — pure, so behavior for valid documents is byte-identical);
- **invalid** → the engine's own `SyntaxError` from `JSON.parse` is
  captured (`JsGetAndClearException`) and recorded on the still-unparsed
  record via `JsSetModuleHostInfo(module, JsModuleHostInfo_Exception, …)`
  → `SourceTextModuleRecord::OnHostException` (:1319-1333) sets
  `errorObject`; `ParseModuleSourceOnRecord(module, nullptr, 0)` then
  drives the existing dependency-failure notification, and
  `ParseSource`'s failure path keeps the pre-recorded error object
  (:183-211 — the recorded `errorObject` wins over the synthesized
  URIError/compile error) and propagates it through `NotifyParentsAsNeeded`
  → the root's `NotifyHostAboutModuleReady(exception)` — phase
  `resolution`, identity preserved.

**Ownership record — JSON validation exception:**

```text
Resource / creation call / source location:
  the SyntaxError Var thrown by the engine's JSON.parse during the host's
  validation JsRun (ModuleMessage::Call fetch arm), transferred with
  JsSetModuleHostInfo(JsModuleHostInfo_Exception) → OnHostException →
  SourceTextModuleRecord.errorObject.
Domain and allocator: Recycler (JS value) — created inside the engine's
  JSON.parse; never natively freed.
Owner; transfer/publication point: engine exception state → host local
  (JsValueRef on the C++ stack between JsGetAndClearException and
  JsSetModuleHostInfo — conservatively scanned, no intervening engine
  call) → module record's traced Field(Var).
Borrow invalidation: GC only; the record roots the value exactly as it
  roots parse-error errorObjects today.
User-code/reentrancy: the validation run executes JSON.parse (pure). It
  resolves `JSON` from the global — the same object the synthesized body
  would use at evaluation, so validation and evaluation cannot disagree;
  a host application that replaces global JSON changes both identically
  (a known limit of the synthetic-module design, not newly introduced).
Failure: if the host cannot run validation (JsCreateString/JsRun API
  failure), the legacy evaluation-time behavior stands (honest fallback);
  if recording fails, the empty-parse fallback surfaces a URIError —
  still a resolution-stage load failure, degraded identity only.
Thread/context: the host message-pump thread with the runtime context
  current — the same state in which the arm already calls
  JsParseModuleSource.
Tests: §5 json probes + json-invalid corpus flip §6.
```

## 4. Probe battery

`out/s12phase/` (scripts listed in appendix A; run as
`ch -NegativePhaseReport -Module <p*.js>`).

## 5. Results (to be completed after the fix build)

## 6. Corpus (to be completed)

## 7. Gates (to be completed)

## 8. Residuals

- `import-defer-of-syntax-error-fails.js` stays downgraded: the parser
  rejects `import defer` syntax at parse (post-ES2025 syntax, out of
  scope) — marker `parse` ≠ declared `resolution`, correctly.
- Static-import fetch failures that occur during the **root's own parse**
  (e.g. unsupported attribute `type`) surface through
  `JsParseModuleSource` → `JsErrorScriptCompile`, so the marker reports
  `parse` although the witness was set (observed `p9b`: `Error: fetch
  import module failed`, marker `parse`). Pre-existing marker precedence
  (parse beats the witness), unchanged by this lane.
- JSON BOM: this engine's `JSON.parse` rejects a leading U+FEFF, so BOM'd
  JSON modules fail (now at resolution, `SyntaxError`), matching the
  engine's own JSON conformance; the phase moved, the accept/reject
  behavior did not.
- JIT (B4) verification remains out of scope; TTD unchanged (module
  records/error paths are not TTD-instrumented by this lane).

## Appendix A — probe sources

See `out/s12phase/` in the lane worktree (uncommitted scratch by repo
convention); the fixtures are: `f-self.js`/`f-self2.js` (self link error,
the `ensure-linking-error_FIXTURE.js` shape), `mid1.js`/`mid2.js` (depth),
`f-ok.js`, `f-side.js` (evaluating sibling), `good.json`/`bad.json`/
`bom-good.json`/`bom-bad.json`, `cyc-a.js`/`cyc-b.js` (cycle), and roots
`p1*`–`p15` as described in §5's table.
