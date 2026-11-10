# Open-cards register (consolidated)

The consolidated status of every residual card filed across the session's
receipts (`doc/e*.md`, `doc/c2-*.md` residual/review sections, B-lane
receipts), re-verified on 2026-11-05 against `zig-out/bin/ch`
(`45ed0e31c` lineage + this sweep's fixes; binary SHA in the sweep run
summaries) with Node v26.5.0 as the semantics oracle. Statuses:
**open-verified** (reproduces today; probe/corpus evidence inline),
**fixed-by-lane-X** (a later lane closed it incidentally), **fixed-here**
(closed by this sweep), **not-reproducible** (no longer reproduces; oracle
may have moved), **decision** (product/flag posture, not an engine defect).

Verification probes live in the sweep session log; the one-line evidence is
quoted per card. Corpus runs used `tools/test262/run.py` on the pinned
checkout (`out/test262`).

## Counts by status

| status | count |
|---|---:|
| open-verified (engine defect, owner lane pending) | 38 |
| decision (flag/feature posture, incl. SAB/RegExp/TCO/module-host families) | 21 |
| fixed-by-later-lane (verified this sweep) | 7 |
| fixed-here (this sweep) | 6 |
| not-reproducible (incl. oracle convergence) | 6 |
| record-only / no-action (differentials, awareness notes) | 9 |
| open, e12 GCStress campaign (tool crash + RecyclerTest assert, below) | 2 |
| open, e12 fulljit SIGSEGV family (deterministic 109 + load-only crasher, below) | 2 |
| record-only, e12 (dynapogo variant needs jit-profile binary) | 1 |
| open, e13 swarm-triage disposition (below) | 4 |

---

## 1. Decision-owned cards (flag/feature posture — not engine defects)

| Card | Source | Status | Evidence / notes |
|---|---|---|---|
| SAB off (`-ESSharedArrayBuffer`): B8-3, e10-§4.1, e3-R1(SAB half), E11-3, e9-B8-4 flag-gated part | e3, e9, e10, e11 | decision | `typeof SharedArrayBuffer` → `undefined`; flag-on ceilings 10/10 (e10), 234/264 (e9) already recorded |
| RegExp prototype getters off (`-ES6RegExPrototypeProperties`): E8-3, e3-R1 (18), S9 sub-batch 2 | e3, e8 | decision | `getOwnPropertyDescriptor(RegExp.prototype,'flags'\|'source')` → `undefined`; e8 flag-on 84/0 |
| RegExp symbols off (`-ES6RegExSymbols`): E8-1 matchAll, E8-2 replaceAll, S7/S11 residuals | e8 | decision | `typeof String.prototype.matchAll` → `undefined`, `typeof Symbol.match` → `undefined`; e8 flag-on 50/0, 92/0 |
| E11-1 proper tail calls (34 scenarios) | e11 | decision | ES2015 27.2.4 never implemented; same posture as V8/Node (oracle also overflows); C3-class |
| E7-1 tagged-template cooked `undefined` (2 scenarios) | e7 | decision (engine work owed, distinct slice) | probe: `eval('tag`\\1`')` → scan-time SyntaxError in ch, evaluates with raw `"\1"` in Node; needs dead `m_fNextStringTemplateIsTagged` plumbing |
| B5-1 remainder: legacy `caller` throws uniformly vs Node `null` | e6 | decision (spec-letter) | probe: sloppy `F` called by strict `S` reads `F.caller` → TypeError in BOTH parse modes (the parse-mode split no longer reproduces — e6's own lane closed the asymmetry); Node returns `null`; ES2025 removed the accessor |
| B4-2 Node double-conversion | e5 | not-reproducible (oracle converged) | probe: `o[k()] *= 1` converts key **once** on Node v26.5.0 today (was 2 at e5 time); engine matches spec/test262 — do not read Node as buggy here in future comparisons |
| Module host surface: B1-1 module early dup decls; B1-3 instn-* TDZ (20); B1-4 namespace strict [[Set]]/[[Delete]] (30); B1-5 eval-gtbndng (37); B1-6 label-before-import (1); B1-7 anon-default-export name (8); B1-8 export-expname (10); B1-9 errored-cycle member (2); B1-10 namespace ToString | c2-s12 | open-verified (engine/module lane) | corpus run `t2-modules`: all named files still fail (e.g. `early-dup-top-function.js`, all `eval-export-dflt-*-anon`, all `export-expname-*`, `instn-*` family ~100 scenarios, `await-ns-set/delete-*-strict`); owner = module lane |
| e2 TLA dynamic-import back-edge never settles | e2 | open-verified (corpus witness found) | `top-level-await/pending-async-dep-from-cycle.js` + `fulfillment-order.js` → "Test did not run to completion" (hang); nearest passing siblings `dynamic-import-of-waiting-module` 3/3 |
| c2-s12-import-attributes residuals (B4 JIT lowering of `OP_ImportCallWithOptions`, `assert` keyword, non-JSON type rejection, JSON-relative resolution) | c2-s12 | open (per receipt; module/JIT owners) | no contradicting lane; es6module gate legs green |
| c2-s13-symbol-keys: symbol keys strongly retained (memory bound); registered-symbol message wording; getOrInsert out of slice | c2-s13 | open (design bound) | no surface change since; WeakRef/FR now exist (s13b) so the bound is observable |
| E8-5 vacuous passes at flag-off | e8 | open (flag-flip checklist) | re-witness at any flag flip; flag-on they pass (e8 §3) |
| E8-4 S7 `AdvanceStringIndex` +1/code-unit for `u`; S11 absent-`flags` internal bit | e8 | open (no witness) | no corpus or probe witness today |
| c2-jit-test262 timing flakes (waitAsync no-spurious-wakeup) | c2-jit-test262 | open (harness) | e12 re-measure (2026-11-05, current binary): flake is flag-on-campaign-only — 9-file `--jobs 1` selection flips 2→4 `lapse >= TIMEOUT` failures run-to-run, single file alone passes 4/4, default-flag shape 0/18 deterministic (`Atomics` undefined); certified refresh (flag-off) and every rlexe gate (in-tree `es7/s6-waitasync-*.js` only) are structurally exempt — load-robustness note in tools/test262/README.md; flag-on campaigns pin `--jobs 1` per-file or ignore the family |
| b5-guid: regen mint is dual-variant-only; engineering-scheme `__TIME__` granularity; ~~lowercase `Bytecode/` path on Linux~~; stale `out/{noJit,jit}/debug` copies | b5-guid-rotation | open (B5 package/floor) | the single-variant **overwrite guard** is fixed-here (§4); the lowercase `Bytecode/` GUID-header path is **fixed by e13** (mint-path commit: tracked casing + RegenGuidPathTests, build-tools 65→67); the rest ride B5 packaging |
| c1/c2-s10: error-text differences in Set methods (`Object expected` vs Node texts) | c2-s10 | record-only | error types match; corpus checks types only |

## 2. Engine-lane-owned cards (open-verified today)

| Card | Source | Status | Evidence |
|---|---|---|---|
| e3-R3 derived-ctor completion errors carry callee realm (4) | e3 | open-verified | corpus: `Function/internals/Construct/derived-return-val-realm.js` fails ("TypeError but different constructor with the same name") |
| e3-R4 cross-realm eval treated as direct at direct-eval positions (1) | e3 | open-verified | corpus: `expressions/call/eval-realm-indirect.js` fails ("inside" vs "outside") |
| e3-R5 sloppy put on primitive base skips Proxy in proto chain (2) | e3 | open-verified | corpus: `types/reference/put-value-prop-base-primitive-realm.js` fails (0 vs 1); same-realm root |
| e3-R6 debug-only cross-realm trap-args invariant | e3 | open (debug-static host not a gate) | unchanged; not relaxable unilaterally |
| e4-B1 Reflect.set invalid-index-at-entry skips value conversion | e4 | open-verified | probe: `Reflect.set(ta, 5, sideEffect)` → `r=false converted=0`; Node `r=true converted=1`; entry plumbing bypasses primitives |
| B9-8 Reflect.set conversion-only stores return false for canonical non-indices ("-0"/"1.1"/OOB); Proxy-set-delegation cascade | e9/e10 | open-verified | probe: `Reflect.set(ta,"-0",sideEffect)` → `r=false converted=false`; Node `true/true`; `"1.1"` → false vs true; the **delete half** of this family is fixed-here (§4) |
| e10-§4.4 `SetItem` early-out / `String`-object key no-convert | e10 | open-verified | probe: `ta[new String("-0")] = sideEffect` → `converted=false` (Node `true`); same typed-array-[[Set]] substrate as B9-8 |
| B8-2 typed-array internals family, remainder: canonical-non-index [[Set]] fall-through, prototype-setter bypass on valid-index stores, `configurable:true` descriptors (B9-7) | e9/e10 | open-verified (delete half closed here) | probe B9-7: `getOwnPropertyDescriptor(Uint8Array([7]),'0')` → `c=false` (Node `true`); corpus Delete selection now green (t5) |
| e10-§4.2 `options-maxbytelength` Reflect.construct order (2 scenarios) | e10 | open-verified | probe: ch reads `newTarget.prototype` twice and before the options coercion (`proto,max,proto` vs Node `max,proto`); needs Reflect.construct redesign |
| e10-§4.3 Array join separator length capture | e10 | open-verified | probe: `[1,2,3].join(evil)` → `"1"` vs Node `"1.."` (captured length); no corpus witness |
| e11-E11-2 unshift fast-path bypasses prototype accessors (6) | e11 | open-verified | probe: `unshift` on `[]` with `Array.prototype[0]` setter → `setters=0 own0=true`; Node `setters=1 own0=false throw=""`; contained in `UnshiftHelper`/`useNoSideEffectUnshift` |
| e7-E7-2 corpus DNE residue: strict `08`/`09` numerics (6), strict `\8`/`\9` strings (4), module-code shapes (8) | e7 | open-verified | probes: `eval("'use strict'; 08;")` accepted (Node SyntaxError); `eval("'use strict'; 'a\\8b';")` accepted; module shapes via B1-1 family run |
| e7-R1 template `\08`/`\09` compound spelling | e7 | open-verified | probe: `` eval('`\\08`') `` accepted; Node SyntaxError ("Octal escape sequences…"); fold into family C's rule |
| B1-2 residue — **now green** | e7 | fixed-by-e7-review-follow-ups | probe: `new Function("import('x') = 1;")` → SyntaxError ("Expected ';'"); for-head shape → SyntaxError; both ride the lane's parser rule as predicted |
| B4-4/B5-2 super receivers: element-form data-store receiver, dot/element compound store sink | e5/e6 | open-verified (corpus-green) | probes (review-corrected): `super[k]=5` on a data property → stores on the BASE, no own property (Node: own); `super[k]+=5`/`super.k+=5` setter sinks onto the home prototype (Node: the instance). Getter/setter `this` is the instance in BOTH engines (the earlier `this=undefined` evidence line was stale). **test262 `language/expressions/super/**` 100% green (184/184, 94 files)** — no corpus exposure; needs `LdElemI_A`/`StElemI_A` receiver plumbing (super-semantics lane). Dot-form plain get/store and compound **store** receivers were fixed by the e6 lane (probe: correct today) |
| e6-B5-3 `thisForSuperCall` allocated before args for invalid super ctors | e6 | open-verified (weaker shape) | probe: `Reflect.construct(class extends null…, [sideEffect], proxyNT)` → ch reads `prototype` twice, args never evaluated; Node throws without the read; untested by corpus |
| e6 fn-caller Reflect.set oracle split | e6 | open-verified | probe: `f.caller = 1` silent-ok; `Reflect.set(f,'caller',1)` throws TypeError (Node: `true`); three behaviors for one property |
| e6 function own-key order + anon-fn-expr `name` | e6 | open-verified | probes: `getOwnPropertyNames(function f(){})` → `prototype,length,name` (Node `length,name,prototype`); anon expr `name` absent (Node own `name`); owned by function lane |
| e5 inherited P5 destructuring residuals (25) + `GetPropertyObjectForElementAccess` debug-context gap | e5 | open (per receipt) | unchanged; wide-run clean |
| B8-6 `toLocaleString` locale/options forwarding (Array/%TypedArray%/BigInt) | e9 | open-verified | probes: element calls see `argcount=0` (Node 2); `123456789n.toLocaleString("de-DE")` → `"123,456.789"`-style **default-locale** grouping, Node `"123.456.789"`; threads args through `EntryToLocaleString` → `CallInfo(3)` |
| c2-s14-assignment-reference: extra proxy `getOwnPropertyDescriptor` in `with`-env set path (2 scenarios) | c2-s14 | open-verified | corpus `t4-proxy`: `set-mutable-binding-idref-*-with-proxy-env.js` 1 fails, get-binding 2/2 pass |
| c2-s14-annexb-var-skip P4 residue (17 scenarios: eval existing-global-init family, script-decl-lex-collision) | c2-s14 | open | declaration-instantiation batch |
| c2-s14-blockscope inherited head/case/catch rules (for-head lex-vs-var, switch caseblock, catch params, `with` fn decl, eval var conflicts) | c2-s14 | open | all pre-existing, Node rejects; probe basis unchanged |
| c2-s14-eval-arguments: `function g(p = 1) { eval("var p = 9"); }` ReferenceError residue | c2-s14 | open | P4 batch |
| c2-s12-module-phase: multi-error selection order (dict order vs requestedModules), NUL-truncated JSON, marker precedence (parse beats witness), host string-module quirks | c2-s12 | open (low, host/parse) | identical-at-base; no contradicting lane |
| c2-s13-finalization-registry: JsRT `EnqueuePromiseTask` E_NOTIMPL hosts get no cleanup; timing guarantees; callback-error text; host-dispatch rejection | c2-s13 | open (host contract) | recorded contract, not engine defect |
| c2-jit-private-opcodes: `LdPrivateBrand` eval-scope lowering; `LdEnvObj`/`InvalCachedScope` NoRegister; private-call inlining/int-specialization | c2-jit-private-opcodes | open (per standing record) | functions with these opcodes still abort whole-function codegen (backstop by design); JIT gates 646/646 green; no contradicting lane |
| c2-jit-eval-fidelity: eval bodies stay interpreted; split-param-region/loop-body direct-eval sites stay interpreted; `isParamScopeDone` bailout latent divergence | c2-jit-eval-fidelity | open (per standing record) | same basis; `arm64-macos-jit.md` status list current |
| c2-jit-finally-globopt: `MergePredBlocksValueMaps` abort backstop; synthetic loops optimization-free | c2-jit-finally-globopt | open (by design) | coverage-not-perf posture documented |
| c2-jit-immutable-store: TTD drops immutable bit; no OS page protection; `return-immutable.js` harness | c2-s8/c2-jit | open (TTD/S8 stream) | unchanged |
| B8-5 JIT BigInt surfaces, TTD BigInt recording, `JsGetValueType`=JsObject, giant-decimal quadratic ToString/parse (perf) | e9 | open (perf/TTD) | 0 corpus failures; restated still-true at e9 |
| e9-B8-4 Atomics multi-agent waiter-list ($262.agent) (30 flag-on) | e9 | open (host/agent lane) | non-BigInt analogs fail identically |
| e10-§4.5 JIT fulljit s8 diffs (length-tracking gap) | e10 | open (S8/B4) | inherited; per S8 receipt |
| e10-§4.6 isSealed/isFrozen non-extensible TA Node-quirk replication | e10 | not-reproducible | probe: `isSealed=true isFrozen=false` identical to Node on the plain shape; no divergence observed on the current oracle |
| e4 residuals: `fill` empty-range asymmetry (awareness), FindMinOrMax/CharArray detached throws, GetPropertyQuery fixed-PId proto walk | e4 | open (low/no witness) | awareness notes; proto-walk probe: detached-view read through fixed-PropertyId path returns undefined without proto `get` firing on this build (shape needs exotic defineProperty witness) |
| e6 inherited eval-super runtime-vs-SyntaxError; nested-class-key `PropertyAllowed` hole | c2-s1 | open (parity-kept) | unchanged |
| e5-B4-1 `for (let \n of/in)` | e5 | not-reproducible (already retracted at review) | probes: `let \n of` rejected by both; `let \n in` accepted by both (matching oracle) |
| e5-B4-3 destructuring memoization upstream unprobed | e5 | open (watch) | probe: converts once on both engines; re-check on corpus revisions |
| B5-5 `(async function await(){})` accepted | e6 | fixed-by-e6 (own Parse.cpp await rule) | probe: SyntaxError on current binary ("Unexpected invalid identifier 'await' after 'function'"); Node also rejects |

## 3. Hardware/B4-owned cards

| Card | Source | Status | Evidence |
|---|---|---|---|
| Linux JIT evidence for the c2-jit campaigns (19/19 Mac-only) | c2-jit-test262, c2-jit-reentrancy-fix | open (B4) | discharge requires Linux runs |
| Windows/x64 backend proofs for shared `Lower.cpp` guards | c2-jit-immutable-store | open (B4) | ARM64 macOS only exercised |
| FRR cleanup-callback arming unexercisable in host | c2-jit-reentrancy-fix | open (coverage) | no script-exposed GC in ch; 200-reg churn never fired cleanup |
| B1-B3 review items: hosted-CI clause conditional on first green Actions run | b1-b3-review | open (CI) | process item |
| dbg-async-attach / GCStress / concurrent-GC matrices (s13 weakref/FR, enumeration-order) | c2-s13, c2-s14 | partially run (e12) | the bounded GCStress campaign ran (e12 §2): rlexe core 582/0 identical under `-CollectGarbage -GCMemoryThreshold:1`, concurrent axes and rescan-protect; the classic `-ForceGCTimeout`/`-GcStress` host flags never existed in this tree; two new cards below. Full-tree and JIT-profile GCStress matrices still unrun |
| bin/GCStress standalone startup SIGSEGV (null ThreadService → `InitializeConcurrent` → `HasCallback()`) | e12 | open (tooling) | 100% repro: `zig-out/bin/GCStress` → rc=139 in ~0.02 s (crash report `SimpleRecyclerTest → EnableConcurrent → InitializeConcurrent → HasCallback`, addr 0x0); `GCStress.cpp` passes `Initialize(false, nullptr)` while the ENABLE_CONCURRENT_GC + debug-config default branch calls `EnableConcurrent(nullptr)` (Recycler.cpp:823-855, 4818). Workaround (verified): `GCStress -js -off:ConcurrentCollect` runs (10-min bounded run clean) |
| `ch -RecyclerTest` StressTester OBJALIGN assert (debug-static) | e12 | open (GC owner) | 100% repro: `out/debug-static/bin/ch -RecyclerTest test/Basics/hello.js` (any script) → `ASSERTION 41943/41946 (lib/Common/Memory/StressTest.cpp:48)`, rc=133, same seed each run; `-RecyclerTest` is `#if DBG` (ThreadContext.cpp:803) so no gate leg reaches it |
| fulljit deterministic SIGSEGV family, 109 non-WASM rlexe tests (dynapogo flags) incl. es7 matchall/bigint64array/resizable-typedarray | e12 (supersedes c2-jit-eval-fidelity's "5 load-flakes" note) | **fixed-here (e14)** | root cause: b5 `9b9ef5d50` changed `LowerNewClassConstructor`'s prototype store to `IndirOpnd::New(aux, 2, ...)` — the int32 is a BYTE offset, not a slot index; the misaligned store clobbered length/name and left the prototype slot recycler-zeroed (null Var) → `NewScObjectCommon → GetTypeId(0x0)` faulted at address 0x8. Fix: `2 * sizeof(Js::Var)` (Lower.cpp, one line). Full receipts: doc/e14-fulljit-sigsegv.md — dynapogo after 2019/286 `-j4` (failures exactly at the 286 receipt base) and 2019/286 `-j10` (immutable-binary rerun), both with ZERO signal exits (before: 1907/398, 115 SIGSEGV + 3 SIGBUS); standalone repro 20/20; interpreted 20 class-crashes → 0 (3 pre-existing unrelated crashers remain, identical before/after); gates 2051/2103 + jit check PASS with an extended check_jit class smoke (negative control: broken build fails the gate with -11) |
| matchall #15 `lastIndex` sloppy-[[Set]] tiering flake | e14 (adversarial-verified independent) | open (regex lane) | `matchall.js:309` sloppy `r2.lastIndex = 5` on a non-writable data property goes through under one `-j10` tiering order (IC/[[Set]]-bypass-after-transition hazard); no dataflow to the e14 ctor-slot corruption (disjoint objects); 40/40 standalone-clean incl. exact profile-input recipe; not standalone-reproducible |
| NamedEvaluation `o[expr] = class{}` yields bracket SOURCE TEXT, not the key VALUE | e14 (adversarial, pre-existing) | open (conformance lane) | `var o={}; o[q()] = class{}` → ctor name is `"q"` (or `""` folded) where spec wants the key value `"xy"`; numeric keys pinned correct; identical Nov-3 pre-b5 binary + byte-identical all three tiers |
| fulljit load-only crasher (JIT-race candidate): es6/blockscope-functionbinding.js passes standalone, SIGSEGV at `-j10` | e12 | **fixed-here (e14)** | same defect as the family above (zero signal exits in the e14 `-j10` rerun, blockscope-functionbinding clean); the load-only shape was tiering-order dependence of which functions get fulljit-compiled, not a race |
| dynapogo/interpreted `profile.dpl` note: runtests' interpreted/dynapogo variants require a JIT-profile binary | e12 | record-only | the no-JIT test profile compiles out `-dynamicprofileinput`/`-dynamicprofilecache` (`DYNAMIC_PROFILE_STORAGE` ← `ENABLE_PROFILE_INFO`, CommonDefines.h:273-284, 386-388): running those variants against `zig-out/bin/ch` fails every test instantly with `Invalid Flag`; use `out/jit/bin/ch` (fresh `zig build -Dprofile=jit`) |

## 4. Tooling-owned cards

| Card | Source | Status | Evidence |
|---|---|---|---|
| regen single-variant `--binary` overwrite hazard (guard required before B5 package/floor) | b5-guid-rotation | **fixed-here** | `tools/regenByteCode.py` now refuses a single-variant run whose content differs from the checked-in headers (exit 1, headers restored byte-identical); `--allow-single-variant-write` escape hatch for scratch trees; 3 negative/positive controls in `tools/build/test_build_tools.py` (`RegenSingleVariantGuardTests`), gate `check-build-tools` (now 67/67: e13 added `RegenGuidPathTests`, next row); the lowercase `Bytecode/` path half of the b5-guid card is **fixed by e13** (next row) |
| GUID mint path casing: `regenByteCode.py` wrote `lib/Runtime/Bytecode/` while git tracks `lib/Runtime/ByteCode/` — same file on case-insensitive macOS, an UNTRACKED file + stale tracked header on case-sensitive Linux (must-fix before B4) | e13 swarm-triage (architecture lens; the b5-guid card's lowercase-path residue) | **fixed-here (e13)** | path constant now uses the tracked casing; `RegenGuidPathTests` pins it functionally (fixture dual-variant mint: the tracked-path file is overwritten in place and no `Bytecode` sibling directory entry exists — catches the split on case-sensitive filesystems) and by exact-cased constant + `git ls-files` (catches it everywhere) |
| `regenByteCode.py --verify` rewrites headers / self-heals | b2 | closed by wrapper (pre-existing) | gate runs through `verify_bytecode_pure.py` (restore + rc); guard above now also covers raw single-variant runs |
| Map-groupBy wrapper-arm asymmetry (cosmetic) | c1-triage/c2-jit-reentrancy-fix | **fixed-here** | `MapGroupByIteratorStepAndValue` now mirrors Object.groupBy's `BEGIN_SAFE_REENTRANT_CALL` defense-in-depth arm; no behavior change by construction (transitions self-arm via `ExecuteImplicitCall` today); warmed coverage stays `setmethods-groupby-warmed.js` |
| `difference` keys-walk branch comment | c2-jit-reentrancy-fix review | **fixed-here** | inline comment at `EntryDifference`'s keys-walk documenting the no-`has`/no-live-receiver consult (vs `intersection`'s live-receiver check), per the ES2025 algorithm |
| B8-7 BigInt default-locale grouping | e9 | **fixed-here** | `BigInt.prototype.toLocaleString` groups through `NumberToDefaultLocaleString` (same non-Intl path as Number); probe: `[123456789n].toLocaleString()` → `"123,456,789"` (Node-identical incl. negative and >bufSize heap path); focused test `typedarray-oob-delete-and-bigint-locale.js`; B8-6 (locale-arg forwarding) stays open |
| e4-B3 RAB-shrunk-to-OOB `delete` returns false/throws; e4 review nit attached-OOB `delete` returns false | e4 | **fixed-here** | `TypedArrayBase::DeleteItem` returns true when `index >= GetLength()` (ES2025 IsValidIntegerIndex half); probes: `delete=true` for shrunk + attached-OOB incl. strict; corpus `internals/Delete/**` + `ArrayBuffer/prototype/resizable/**` now 68/74 with only 6 SAB-flag-gated failures left (was failing the OOB scenarios); in-range cant-delete pinned by the new test and existing `test/TypedArray/delete.js` |

## 5. Cards closed by later lanes (verified this sweep)

| Card | Closed by | Evidence |
|---|---|---|
| e3-R2 TA internals `detached-buffer-realm` (20 scenarios) | e4 (detach lane) | corpus: Get/Delete/GetOwnProperty/HasProperty/Set detached-buffer-realm incl. BigInt variants **all pass** |
| e4-B6 `Object.freeze` non-empty TA no-throw | e10 (B9 seal/freeze work) | probe: both engines throw TypeError today |
| B1-2 residue import() update-shapes | e7 lane + follow-ups | both remaining shapes are parse errors (see §2) |
| B5-5 async-await-name | e6 lane | both reject (see §2) |
| B5-1 parse-mode split | e6 lane | uniform throw in default and deferparse runs (see §1) |
| B4-4 store half (dot + compound stores) | e6 lane | probes: correct receivers today (see §2) |
| B4-2 Node double-conversion | oracle (V8 fix) | Node now converts once (see §1) |

## 6. Swarm-triage disposition cards (e13)

Filed by the five-lens swarm triage (doc/e13-swarm-triage.md) — accepted
findings whose fixes are deliberately deferred, with their bounds.

| Card | Source | Status | Evidence / notes |
|---|---|---|---|
| T3 broadcast 5s-overrun wrong-content pairing | e13 (concurrency lens T3) | open (host/agent lane) | `WScriptJsrt.cpp` BroadcastCallback: after the bounded 5 s per-child consumed-ack wait the parent proceeds with only the `live agent did not acknowledge broadcast` stderr warning; a child that acknowledges late (or a callback still pending when the NEXT broadcast overwrites `parent->sharedContent`) can pair broadcast N's wakeup with broadcast N+1's content. Bounded (test262 agent semantics only), warned on stderr, memory-safe (each consumer's wrapper AddRef's the handle it actually reads) — a code-quality risk, not a crash path |
| WScriptJsrt.cpp split before further Linux/host extension | e13 (architecture lens) | open (refactor suggestion) | ~3.1k lines mixing host-object installation, the global module/script registries, agent lifecycle (S6e) and negative-phase classification; R1 added the host-globals lock across all of them. Split into cohesive units (module registry / agent runtime / WScript surface) before the Linux host work extends it further |
| JSERR dedicated-message follow-up (NeedArrayBufferObject reuse) | e13 (compat lens) | open (low) | the ES2025 ArrayBuffer accessor entries (resizable/maxByteLength/immutable/… receiver checks, `ArrayBuffer.cpp` ~18 sites + `AtomicsObject.cpp:94`) all throw `JSERR_NeedArrayBufferObject` ("ArrayBuffer object expected"); type-correct but the shared text loses which accessor rejected. Dedicated JSERR strings when the message table is next touched |
| regen-guard create-on-refuse edge | e13 (tests/docs lens) | open (tooling, low) | `regenByteCode.py` restores only headers that EXISTED before the run (`written_files` is appended only when `os.path.exists`); a refused single-variant run in a tree missing one checked-in header still leaves that header newly created (untracked). Refuse-on-create or delete-on-restore when the guard is next touched |

## Gate deltas

`tools/build/gate_counts.json`: **2050/2102 → 2051/2103** (+1 = the focused
regression `test/es7/typedarray-oob-delete-and-bigint-locale.js`). Gates run
after the fixes: `zig build check` **2051/0** (bytecode verify, native,
smoke, metadata, self-tests PASS), `-Dprofile=debug-static check-regression`
**2103/0**, `-Dprofile=jit check` **646/646** (check_jit PASS, bytecode up to
date), `check-build-tools` **65 tests OK** (3 new guard controls included).

## 7. B4 Linux x64 residuals (doc/b4-linux-run.md)

| Card | Source | Status | Evidence / notes |
|---|---|---|---|
| Dynamic debugger attach + suspended async resume hangs on Linux (`test/DebuggerCommon/AsyncSuspendedResume.js`) | B4 Linux run | open (Linux debugger) | Native x64: prints `before/done/before/after 41/after 41` then stays in the `WScript.Attach` / `CallbackMessage::CallFunction` message loop indefinitely (no exit at 150 s; gdb interrupt shows `Debugger::SourceRunDown` path). Tagged `exclude_linux` (new platform tag in `test/runtests.py`) pending the Linux debugger dynamic-attach implementation; not an engine-semantics defect outside the debugger |
| `test/Function/FuncBody.bug232281.js` O(depth²) stack walk through `eval` | B4 Linux run | open (performance, low) | Full-JIT recursion with `eval("  ")` per frame completes correctly (caught "Out of stack space") in 4m27s on the 4 vCPU proof host vs the legacy 60 s deadline. Cause: each `VEval` walks the whole stack (`JavascriptStackWalker::GetCaller` → `IsNativeAddress`), so overflow depth D costs O(D²). Linux gate widens the deadline to 480 s without changing selection |

e13 swarm-triage lane (doc/e13-swarm-triage.md): gate counts unchanged
**2051/2103** (host/tooling/docs only, no new rlexe entries);
`check-build-tools` **65 → 67** (+2 `RegenGuidPathTests` for the mint-path
casing fix).

## 8. B4 swarm review residuals (2026-11-10)

Filed by the ten-lens review swarm on `4779e73bc`. Fixed-in-tree items are
listed in the commit receipt; these are the deliberately deferred ones.

| Card | Source | Status | Evidence / notes |
|---|---|---|---|
| ES2018 100-digit `toFixed`/`toPrecision`/`toExponential` | RL-02/RL-04, SEC-04 | **fixed in tree (2026-11-10)**; certified rerun pending | Exact `to_chars`-based decimal expansion (767 significant digits) now handles every requested digit count 0..100, with magnitude half-up rounding; API caps are 100/100 on exact-capable toolchains and remain 20/21 otherwise. Targeted test262 Number/prototype `{toFixed,toPrecision,toExponential,toString}` is **276/276**; full certified rerun is still owed. [b4-d1-d4.md](b4-d1-d4.md) |
| `Number.prototype.toString(radix)` low digits for large integers | RL-06 | **fixed in tree (2026-11-10)** | The integer part is converted exactly through `BigUInt`/`DivRemSmall`; 13,600 differential conversions (bases 2..36, 1e308 and random large integers) match exact Python integers. Fractional digits keep the implementation-defined legacy loop. [b4-d1-d4.md](b4-d1-d4.md) |
| Async debugger attach + suspended coroutine reparse family | CA-01…CA-05 | **fixed in tree (Mac, 2026-11-10)**; Linux native verification pending | The guard is now gated on `GLOBAL_ENABLE_WRITE_BARRIER` (the finalizer compile condition), enabling Linux x64 with the same release pairing. New `AsyncSuspendedDetach.js` covers attach+detach with a generator suspended across both plus a collected suspended generator; `exclude_linux` removed from both D1 tests. Mac gates green; Linux message-loop exit and pin re-record are the handoff. [doc/b4-d1-d4.md](b4-d1-d4.md) §5 |
| ARM/ARM64 CIE-vs-FDE registration | MEM/portability findings | open (portability, medium) | `ChakraEhFrameFdeStart` is x86-64 gated; `arm/` and `arm64/` `XDataAllocator` still pass the CIE start to `__register_frame`. Non-Apple ARM64 also emits Windows `.xdata`, so the fix must select by buffer format, not architecture. |
| `_LIBCPP_VERSION` as unwinder contract proxy | ARCH-04, PORT-01 | open (build-contract, medium) | The FDE-vs-CIE choice is inferred from a stdlib header macro, not from the linked unwinder. A libstdc++-headers + LLVM-libunwind build would register nothing. Replace with a build-contract define when a second unwinder profile exists. |
| FullJit without dynamic profile is ungated | B4-JIT-05 | open (coverage, low) | Both Linux regression variants either disable simple JIT or supply `-dynamicprofileinput`; a no-profile FullJit run (287/0 in the reviewer probe, 35 s) is a distinct path worth adding as a third variant. |
| `check-jit` proves output, not execution | BT-03 | open (tooling, medium) | The gate checks an `nm` symbol and identical stdout from `-mic`/`-forceNative`; a build that ignores the JIT flags still passes. Add a JIT-only observable (e.g. a compiled-function counter) as a control. |
| Deep legacy regex class nesting hard-fails at ~36k | pr-02 | open (parser, low) | VValidateClass's stack probe raises the stack-overflow ParseError even though Node accepts 40k nested `[`; the VScanAbort bound does not cover the probe path. |
| `WebAssembly.Module` vs `WebAssembly.validate` deferred gap | SEC-02 | **fixed in tree (2026-11-10)**; Linux native verification pending | Deferred bodies are validated at construction. `ValidateFunction` no longer publishes `firstTmpReg` (`SetFirstTmpReg` is guarded by `!IsValidating()`), so the later deferred compile still owns the one-shot frame layout. New test covers invalid-body construction, exported-function first call, and an import/instance call-through; WASM is x64-only and cannot run on this Mac ARM64 host. [doc/b4-d1-d4.md](b4-d1-d4.md) §5 |
