# B1 receipt: dynamic-import host loader (C4 path-to-green items 2/3)

Lane: **B1 dynamic-import** (first MAINLINE-ONLY lane). Base: `modernize/baseline`
@ `8ba87c918` (gates 2038/2090). Commits: `ec875f467` (diagnosis evidence),
`f6adb94f0` (fix + rlexe fixture), this receipt. No engine source changed:
the full diff is `bin/ch/WScriptJsrt.cpp` + the rlexe fixture + analysis
artifacts (`git diff 8ba87c918..HEAD --stat`: 4 files, nothing under
`lib/` or `pal/`).

## 1. Root cause (with anchors)

**Host bucket (419 → 1).** The test262 harness (`tools/test262/run.py`
`harness()` → `subprocess.Popen(cwd=corpus)`) runs `ch` with **CWD = corpus
root**, while eshost (`node_modules/eshost/lib/ConsoleAgent.js` +
`lib/dependencies.js`) materializes each test file and every discovered
fixture/import **side by side** in a per-agent temp dir and passes `ch` the
test file's absolute path. A test's own `import('./fixture.js')` therefore
must resolve against the **test file's directory**. The ch host instead
resolved script-issued dynamic imports against the process CWD:

- `bin/ch/WScriptJsrt.cpp` `FetchImportedModuleFromScript` (and the
  `...WithAttributes` variant registered at `WScriptJsrt.cpp:1369-1372`)
  discarded the `dwReferencingSourceContext` the engine passes
  (`lib/Runtime/Language/JavascriptOperators.cpp:11354-11412` — the engine
  already derives the referencing script's host source context, walking
  eval/callers) and called the fetch helper with no refdir → `_fullpath`
  resolved `./fixture.js` against CWD → corpus root → file not found.
- On load failure the host parses the record with `sourceText == nullptr`
  (`ModuleMessage::Call` load-failure branch), and the engine records
  `URIError(specifier)` on the record
  (`lib/Runtime/Language/SourceTextModuleRecord.cpp:112-129`); the dynamic
  import promise then rejects with that URIError — the manifest's
  "dynamic-import fixture loader URIError" family (419 scenarios), plus the
  "Expected SameValue(«URIError», «SyntaxError/TypeError»)" shapes (~96)
  where the fixture, once loadable, produces the real error.

The JsRT contract is satisfiable host-side: the callback receives the
referencing script's source-context cookie, and the host already maintains
`scriptDirMap` (source context → script directory) populated for **every**
source context passed to `JsRun` (`bin/ch/ch.cpp:450-451`,
`WScriptJsrt.cpp:843,890` via `RegisterScriptDir`). No engine change was
needed.

The lane brief's "hang" hint resolved as **not a load-failure hang**: a
missing file rejects with URIError and settles; the recorded
`catch/nested-async-function-await-eval-script-code-target.js`-shaped
"Test did not run to completion" cases are a *parser* gap (§4, family A)
plus handler-side exceptions being swallowed as unhandled promise
rejections (ch does not report those).

## 2. Fix design

`bin/ch/WScriptJsrt.cpp` only:

1. `FetchImportedModuleFromScript` / `FetchImportedModuleFromScriptWithAttributes`
   look up `scriptDirMap[dwReferencingSourceContext]`; on hit, pass the
   script's directory as `refdir` (same pattern as the existing
   `FetchImportedModule` module→module path); on miss keep the legacy CWD
   fallback (synthetic/unregistered sources).
2. Both fetch helpers skip the `refdir` prefix for absolute specifiers
   (leading `/`, `\`, or a Windows drive letter) — previously
   `dir + absolutePath` concatenated into a wrong path for every caller
   passing `refdir`.

Contract change: upstream `5fc588778` (2018) made *module* imports
module-relative but deliberately kept *script* dynamic imports CWD-relative;
`test/es6module/bug_issue_3257/script/script0.js` pinned that with the
comment "relative to CWD not the script". This lane updates the pinned
contract to `HostImportModuleDynamically` script-relative semantics; the
fixture's two specifiers became `../mod1.js`, `../mod2/mod2.js` and its
comment now states the script-relative rule. `bug_issue_3257.baseline`
(expected output `script0, mod2, mod1, mod0`) is **unchanged**.

**Ownership record** (per doc/memory-management.md): `scriptDirMap` lookup
is a read of an existing static `std::map` (cleared only at teardown,
`WScriptJsrt.cpp` Uninitialize). The directory `std::string` is copied into
a local before the helper call (identical to `FetchImportedModule`'s use of
`moduleDirMap`), then only its `c_str()` crosses the call. No new
allocation, root, callback state or lifetime. The absolute-specifier guard
is pure. Untracked-context fallback preserves prior behavior for any
unregistered caller.

## 3. Verification

Selections `test/language/expressions/dynamic-import/**` +
`test/language/module-code/**` + `test/language/import/**`
(`run.py --binary zig-out/bin/ch --jobs 8`, pinned corpus `2e0a5676`,
host flags `-nonative -Test262 -CollectGarbage -NegativePhaseReport`;
evidence: `tools/test262/b1-before-analysis.json`,
`tools/test262/b1-after-analysis.json`):

| selection | before | after | delta |
|---|---|---|---|
| dynamic-import (broad) | 1066/840 (1906) | **1539/367** | **+473/−473** |
| module-code (broad) | 530/66 (596) | 530/66 | 0 |
| import (broad) | 29/106 (135) | 29/106 | 0 |
| **edition-selected total** | **1170/608 (1778)** | **1643/135** | +473/−473 |

Scenario-level diff (file × attrs × scenario): **473 fail→pass, 0 pass→fail**.
Edition failure buckets: host **420 → 1**; engine-defect 178 → 124 (the −54
are the URIError-mismatch shapes that were loader artifacts);
required-feature-missing 10 → 10. The residual host:1 is
`module-code/parse-err-decl-pos-import-labeled.js` — a parser gap (a label
before an `import` declaration is accepted), surfacing through the loader as
URIError; card B1-6.

**s12-modules campaign batch** (`tools/jit-test262/batches.json`, globs
`test/language/import/**` + `test/language/module-code/**`): before 559/172
of 731, after 559/172 of 731 — **zero-diff**.

**Focused rlexe (S12)**: `test/es6module/module-syntax.js` 11/11,
`dynamic-module-functionality.js` 22/22, `dynamic-module-import-specifier.js`
3/3 — each in normal, `-forcedeferparse`, `-nodeferparse` modes
(`-nonative -ES6Module -ESDynamicImport -MuteHostErrorMsg -BaselineMode`);
`bug_issue_3257.js` green with the unchanged baseline. The pre-existing
module-syntax.js foreign-CWD quirk (string-loaded modules; s12 receipt §6)
reproduces identically — untouched path.

**Gates** (both through the shared lock): `zig build check` (test profile)
= **2038/0 regression** — pin unchanged, the only suite delta is
bug_issue_3257 passing again under its updated fixture;
`zig build -Dprofile=debug-static check-regression` = **2090/0** — pin
unchanged. Full rlexe sweep (`runtests.py -b zig-out/bin/ch -t --variants
disable_jit`): 2038/0. `zig build -Dprofile=jit check`: green, no
bytecode/GUID deltas — nothing engine-side changed; the host fix and fixture
are profile-independent.

## 4. The 124 engine-defect triage (post-fix, edition-selected)

| family | # | verdict |
|---|---|---|
| A. `catch/*eval-script-code-target` — module early error missing: `var x; function x(){}`, `function x(){} function x(){}` at module top level parse cleanly (must be SyntaxError: module LexicallyDeclaredNames vs VarDeclaredNames / duplicates). Repros: `ch -module` accepts `var x; function x(){}` (m2/m3) and `function x(){} function x(){}` (m7), while `let x; var x` etc. already error. Fix = parser module-scope name tracking (~60-80 lines, module-mode only). **Card B1-1** | 13+2 | card |
| B. `dynamic-import/syntax/invalid/*` — `$DONOTEVALUATE` executed: `++import('')` parses (UpdateExpression on ImportCall must be a parse-phase SyntaxError). **Card B1-2** | 13 | card |
| C. module-code `instn-*` TDZ: "binding is created but not initialized" ReferenceError missing. Pre-existing S1-adjacent cluster. **Card B1-3** | 20 | card |
| D. `namespace/await-ns-*-strict` — module namespace object [[Set]]/[[Delete]] must throw TypeError in strict code. **Card B1-4** | 20+10 | card |
| E. `usage/*eval-gtbndng-*` — indirect-eval binding-update asserts (fn source text in message). **Card B1-5** | 37 | card |
| F. `eval-export-dflt-*-anon` — anonymous default export `name` should be `"default"`. **Card B1-7** | 8 | card |
| G. `export-expname-*` (rfm bucket) — `export {x as "str"}` string export names rejected at parse. **Card B1-8** | 9+1 | card |
| H. `import-fulfilled-member-of-errored-cycle` — cycle error propagation assert. **Card B1-9** | 2 | card |
| I. `parse-err-decl-pos-import-labeled` — label before `import` accepted (also the host-bucket residual). **Card B1-6** | 1 | card |
| namespace ToString: `"x" + namespace` throws `TypeError: Object doesn't support this property or method` instead of "[object Module]" — found while diagnosing; amplifies several of the above into silent unhandled rejections. **Card B1-10** | (cross-cutting) | card |

Not fixed here: each is a bounded but distinct engine/parser feature; this
lane's scope was the loader (done) and the loader-fix must stay small and
reversible. rfm 10 ≈ G (9) + let-ASI tail (1) — matches the manifest's rfm
accounting for these selections.

## 5. C4 path-to-green expected delta (for reviewers to apply after accept)

- Manifest §2 engine-defect-on-existing: 1,700 → ~1,646 (−54 loader-artifact
  shapes reclassified as fixed; the 124 non-host dynamic-import defects
  remain, now precisely triaged above).
- Manifest §2 host: 426 → 8 (419 dynamic-import loader fixed + 1
  parse-err-decl-pos reclassified to engine/parser as card B1-6; remaining:
  4 Array integer-limit, 2 Atomics-wait, 1 module-code → that module-code 1
  is B1-6).
- c4-gate §3 items 2/3 update accordingly; no other clause moves.

## 6. Residuals

- Cards B1-1 … B1-10 (§4) with repros inline above.
- The unhandled-promise-rejection silence (no host report) is host behavior,
  not a regression; noted for a future host lane.
- `module-syntax.js` foreign-CWD quirk: unchanged (string-module path,
  `WScript.LoadModule`, deliberately untouched).

### Review outcome (dual ACCEPT, corrections folded)

Both reviewers ACCEPTed with zero `[bug]`. The general review
reproduced the full delta from raw artifacts (key-join: 473 fail→pass,
0 pass→fail, module-code/import bit-unmoved) plus an independent full
rerun (one opposite-direction flake only); verified the 3257 contract
flip is the only script-in-subdir shape in the rlexe tree with the
baseline unchanged; and confirmed all three card repros. The
adversarial review ran a script-vs-module convergence matrix over 16
specifier shapes (byte-identical resolution everywhere; bare/`./`
converge; `_MAX_PATH` guarded) and constructed the registration
completeness set (eval/new Function/timer/async/LoadScriptFile all
resolve correctly; unregistered contexts are synthetic-source ones
where CWD is the only candidate — pre-fix behavior).

**Folded corrections:** (1) **[risk]** module-issued ABSOLUTE
specifiers now load any readable file (before: dir-concatenated
mangling → URIError). This matches the script path and the unsandboxed
test host's existing `LoadScriptFile('/any/path')` reach — not a
regression in kind, but a widening on the module side that this
receipt now states explicitly. (2) The host-arithmetic in the C4 delta
was wrong: host 426 → **7** (419 loader fixed; B1-6 stays host; 4
Array + 2 Atomics timeouts unchanged), engine-defect 1,700 → **1,646**.
(3) The "every JsRun context" claim is overstated: registration sits
in the TTD-enabled branch of `ch.cpp` and misses
crossthread/serialized/bg-parse/parser-state paths — all safely
CWD-fallback (legacy behavior); a non-TTD build would revert main
-script registration (these binaries are TTD-enabled). (4) B1-2's
family count is 4 by name filter (13 was family-overlap counting).
(5) `c:foo.js`-shaped relative names skip the script dir on POSIX
(Windows-drive heuristic — identical both paths; arcane, documented).
(6) No percent-decode/query-strip — consistent with the module path
and LoadScriptFile; documented, not fixed. (7) Stray whitespace-only
hunk noted. (8) The adversarial reviewer's jit-profile default-sweep
observation (WasmSpec/AsmJs/jsapi categories outside the disable_jit
pin) is the known tagged gap plus out-of-pin categories — the
`-Dprofile=jit check` gate remains the authoritative jit gate.
