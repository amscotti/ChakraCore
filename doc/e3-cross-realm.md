# E3 — Cross-realm engine-defect lane (B2 path-to-green)

Published 2026-11-03 by the `b2-cross-realm` implementation lane on
`modernize/baseline` (base `9f9850f9d`). Scope: the **cross-realm 185**
edition-selected failure family from the certified baseline
([c4-manifest.md](c4-manifest.md) §5(c)); full edition refresh is C4's, not
this lane's. Verdict summary: **185 → 55 edition-selected cross-realm
failures** (+130 fail→pass, 0 pass→fail), two bounded engine-defect families
fixed at their root, one same-realm bonus fix (+2), three families carded as
flag-substrate or other-lane overlaps, four small residuals carded.

## 1. Triage (fresh certified-equal baseline)

Raw c4-cert results are not retained (`out/` is ignored), so the 185 was
re-derived exactly: all 203 corpus files whose frontmatter `features:` include
`cross-realm` at the pinned revision `2e0a5676`, run through
`tools/test262/run.py` (controls 25/25). Under the c4 patterns
(`test/language|built-ins|annexB`) + edition policy the fresh baseline binary
reproduced the certified count **exactly: 149 passed / 185 failed** of 334
scenarios (`out/test262-b2-realm-before`, log `out/test262-b2-realm-before.log`).
Node v26.5.0 (`vm.createContext`) was the oracle for every family; spec
citations are ES2025.

| Family | Count | Shape / oracle verdict | Disposition |
|---|---:|---|---|
| A. `proto-from-ctor-realm` (44 files) | 88 | `newTarget.prototype` non-object ⇒ default proto must come from **GetFunctionRealm(newTarget)'s realm** (9.4.13 step 4b via 7.3.27; Node: true). Engine silently kept the executing builtin's realm. Covers all TypedArray ctors, Array/Date/String/Number/Boolean/Error/NativeErrors/AggregateError/Map/Set/WeakMap/WeakSet/Promise/RegExp/Function-family/ArrayBuffer/DataView paths. | **Fixed** (root: `OrdinaryCreateFromConstructor`) |
| D. Proxy/bind `*-realm` (18 files) | 36 | [[Call]]/[[Construct]] are internal methods — no new execution context — so validation TypeErrors and the trap args array (10.5.12/10.5.13 step 7 `CreateArrayFromList`) belong to the **caller's realm** (Node: `ctor=main`, `args-proto=main`). Engine used the proxy's realm. Sub-shapes: revoked-handler apply/construct/defineProperty (6), trap-not-callable (4), construct-trap non-object return (12), args-array realm (4), trap-undefined proto-from-newtarget (2), GetFunctionRealm through proxy chains (4) and bound functions (`bind/get-fn-realm{,-recursive}`, 4). | **Fixed** (root: `FunctionCallTrap` caller realm + IsConstructor/revoked semantics + CrossSite marshal gap) |
| A/D late residuals (3 files) | 6 | `ArraySetLength` RangeError realm (10.4.2.2, 2), JSON replacer IsArray-on-revoked-Proxy TypeError realm (7.2.2 step 3a, 2), `Function/internals/Construct/base-ctor-revoked-proxy-realm.js` missing throw (GetFunctionRealm step 4a via 9.4.13, 2). | **Fixed** |
| B. RegExp prototype surface `cross-realm`/splitter (10 files) | 20 | 18 scenarios: the getters (`source`/`flags`/`dotAll`/…) are absent in the default configuration — the **`-ES6RegExPrototypeProperties` flag decision** (c4-manifest §5(b), S9 sub-batch 2), not a realm defect. 2 scenarios (`Symbol.split/splitter-proto-from-ctor-realm`): `r[Symbol.split]` itself is gated by **`-ES6RegExSymbols`**. | **Carded** (flag substrate) |
| C. `TypedArrayConstructors/internals/*detached-buffer-realm` (10 files) | 20 | cross-realm wrapper around the known **detached-buffer semantics** cluster (c4-manifest §2 "detached-buffer TypedArray/DataView ~104"); the non-realm siblings fail identically. | **Carded** (other-lane overlap) |
| SAB-gated `proto-from-ctor-realm{,-sab}` (4 files) | 8 | `SharedArrayBuffer is not defined` — the **`-ESSharedArrayBuffer` flag decision** (c4-manifest §5(b)). With the flag on these exercise family A's code (the fallback table includes SAB). | **Carded** (flag substrate) |
| E1. derived-ctor completion errors (2 files) | 4 | `derived-return-val-realm` / `derived-this-uninitialized-realm`: the TypeErrors/ReferenceError are compiled into the **callee's bytecode** (`RuntimeTypeError` / `JSERR_BadSuperReference` emit sites) with the callee's realm; spec wants the [[Construct]]-level caller realm. Fixing needs caller-realm plumbing through the constructor-call epilogue. | **Carded** |
| E2. `eval-realm-indirect` (1 file) | 1 | direct-eval-position call of a **cross-realm eval** is treated as direct eval of the caller's frame (`args.IsDirectEvalCall()` trusts the compile-time marking; 13.3.6.1 requires the callee to be the caller realm's `%eval%`). | **Carded** |
| E3. `put-value-prop-base-primitive-realm` (1 file) | 2 | sloppy put on a primitive base does not consult a **Proxy in the prototype chain** (`(5).foo = 1` with a proxy set above `Number.prototype` never fires the set trap) — reproduces **same-realm**, so not a realm defect at root. | **Carded** (proxy-in-proto-chain set gap) |
| **Total** | **185** | 130 fixed / 55 carded | |

Out-of-scope bonus: `Function/internals/Construct/base-ctor-revoked-proxy.js`
(same-realm variant, +2) also flipped via fix 3.

## 2. Fixes (anchors at lane HEAD)

1. **GetFunctionRealm shared + realm fallback in `OrdinaryCreateFromConstructor`**
   — `lib/Runtime/Language/JavascriptOperators.cpp` (`JavascriptOperators::GetFunctionRealm`, `realmIntrinsicProtoGetters` table, `GetRealmIntrinsicPrototype`, and the step-4b fallback in `OrdinaryCreateFromConstructor`; declarations in `JavascriptOperators.h`). The object each builtin passes in already carries the current realm's intrinsic prototype, so the fallback matches that prototype against a table of realm-fallback intrinsics and takes the counterpart from the constructor realm's library. Unmatched intrinsics (e.g. subclass paths) keep the previous behavior. New getters for the 12 concrete TypedArray prototypes, ArrayBuffer/DataView, and SharedArrayBuffer in `JavascriptLibraryBase.h` / `JavascriptLibrary.h` (SAB lives with the other SAB fields).
   — `lib/Runtime/Library/JavascriptFinalizationRegistry.cpp` and `JavascriptWeakRef.cpp` now use the shared `GetFunctionRealm` (the S13 lane's local copy and WeakRef's direct `GetScriptContext()` were proxy/bound blind spots).
   — `lib/Runtime/Library/ArrayBuffer.cpp`: the S8 lane's early `newTarget.prototype` lookup gained the same step-4b fallback after allocation (OOM ordering preserved).
2. **Proxy [[Call]]/[[Construct]] caller realm** — `lib/Runtime/Library/JavascriptProxy.cpp` `FunctionCallTrap`: derives `requestContext` via the new `ThreadContext::GetCallerScriptContextForCallee` (`lib/Runtime/Base/ThreadContext.{h,cpp}`) — the engine's host-script-context idiom, stepping over the `CrossSite::CommonThunk` callee push — and uses it for every validation throw, the trap lookup, and the trap arguments array (`CreateArrayFromList`). Argument values are `CrossSite::MarshalVar`-ed into the caller realm before `DirectSetItemAt` to honor the debug array-storage invariant (identity for same-realm values; in-place x-site wrap otherwise).
3. **Revoked proxies are still constructors** — `JavascriptOperators::IsConstructor(JavascriptProxy*)` no longer walks `GetTarget()` (which threw, from the proxy's realm); ES2025 10.5.15 fixes [[Construct]] at creation, so a revoked proxy reports constructable and its [[Construct]] throws the caller-realm revoked TypeError. `FunctionCallTrap`'s no-trap path also surfaces the revoked-during-`prototype`-lookup TypeError (spec 9.4.13 step 4a via GetFunctionRealm) instead of silently defaulting.
4. **Marshal must not invoke revoked-proxy internal methods** — `lib/Runtime/Base/CrossSite.cpp` `MarshalDynamicObject`: a revoked proxy has no target to consult; reading `revocable().proxy` after revoke is an opaque pass-through, so the entry point is left alone (calls still reach `FunctionCallTrap` and throw caller-realm). This was the actual blocker behind D1.
5. **Caller-realm errors at two internal-method sites** — `lib/Runtime/Library/JavascriptArray.cpp` `SetLength(Var)` (`ArraySetLengthErrorScriptContext`: ArraySetLength RangeError in the caller's realm) and `lib/Runtime/Library/JSONStringifier.cpp` `ReadReplacer` (IsArray-on-revoked-Proxy TypeError in the stringifier's realm).
6. **Focused gate test** — `test/es6/crossRealmSemantics.js` (+ `test/es6/rlexe.xml` entry): six scenarios covering families A/D and fixes 3/5, written against `WScript.LoadScript('', 'samethread')` realms; passes 6/6 on both no-JIT profiles and fails 6/6 on the pre-change baseline. Gate pins bumped `2039/2091 → 2040/2092` (`tools/build/gate_counts.json`, lineage updated).

### Ownership record (doc/memory-management.md §3)

No new retained allocations, roots, or transfer points. Changed sites only
select *which existing recycler-owned intrinsic/error object* is referenced:
the realm-fallback prototypes come from the constructor realm's
`JavascriptLibrary` (deferred-initialized `Field(DynamicObject*)` members,
stable identity, same runtime recycler — `$262.createRealm()` contexts share
one runtime); error objects are `JavascriptError::Throw*` products as before;
the trap args array is `library->CreateArray` as before, from the caller's
library. Argument values entering the array pass through the pre-existing
`CrossSite::MarshalVar` (in-place, identity-preserving; same-realm calls are
identity by construction). Reentrancy: the `newTarget.prototype` read was
already a user-code point (`GetPropertyNoCache`) and its ordering is
unchanged; `GetFunctionRealm` may throw `JSERR_ErrorOnRevokedProxy` from
`GetTarget()` for a revoked proxy newTarget (realm of that specific error is
the proxy's — recorded residual R3). Threads: all changes execute on the
script thread under the existing rental-threading rules; no cross-thread
surface touched.

## 3. Before/after (same binary provenance per side)

| Selection | Before | After | Δ | pass→fail |
|---|---:|---:|---:|---:|
| cross-realm 185 set (203 files, edition-selected under c4 patterns) | **185 fail** | **55 fail** | **+130 f→p** | **0** |
| Affected surfaces (Proxy/Reflect/WeakRef-family/JSON stringify/Function/Iterator/Symbol + touched ctor paths; 4,293 scenarios) | 770 fail | 672 fail | +98 f→p | 0 |
| Bonus: `Function/internals/Construct/base-ctor-revoked-proxy.js` (same-realm, in affected batch) | 2 fail | 0 fail | +2 f→p | 0 |

Commands (all through the shared gate lock): fresh baseline and after runs via
`tools/test262/run.py --binary "$PWD/zig-out/bin/ch" --output out/test262-b2-realm-{before,after3}` with the 203-file pattern list; the affected-surfaces
pair used a baseline binary rebuilt from clean `9f9850f9d` into
`out/b2-baseline-prefix` (`out/test262-b2-affected{,-base}`).

## 4. Verification

- `zig build check` — **rc=0, regression 2040/0** (pin bumped; was 2039/0 pre-lane).
- `zig build -Dprofile=debug-static check-regression` — **rc=0, 2092/0** (pin bumped; was 2091/0). An intermediate debug-only `VerifyNotNeedMarshal` assert in `test/es6/proxybugs.js` (cross-site values entering the caller-realm args array) was resolved by marshaling argument values (fix 2); the gate returned to green before the test landed.
- `zig build -Dprofile=jit check` — **rc=0** (smoke PASS, `-mic:1 -bgjit-` and `-forceNative -off:simpleJit` snippets PASS, bytecode verify rc 0). Engine shared surfaces (JavascriptOperators/Proxy/CrossSite/ThreadContext) were touched, hence this check.
- test262 controls 25/25 on every run; negative-phase marking unaffected (no negative scenarios in the selections flipped).

## 5. Residual cards

- **R1 (28 edition failures)**: flag substrates — B: RegExp prototype getters (20, `-ES6RegExPrototypeProperties` decision), SAB-gated (8, `-ESSharedArrayBuffer` decision). Expected to clear only if/when those flags flip default; the family-A machinery already resolves them when on.
- **R2 (20)**: TA internals `detached-buffer-realm` — owned by the detached-buffer semantics lane (non-realm siblings fail identically).
- **R3 (4)**: derived-constructor completion errors carry the callee realm (bytecode `RuntimeTypeError` / `JSERR_BadSuperReference`); caller-realm plumbing through the ctor epilogue owed. Same card notes the nested-realm limit of the host-script-context idiom (a caller frame entered via another realm's `eval` is not distinguished; no corpus scenario exercises it today).
- **R4 (1)**: direct-eval-position call of a cross-realm `eval` treated as direct eval (var leaks into the caller's frame).
- **R5 (2)**: sloppy put on a primitive base skips a Proxy in the prototype chain (same-realm repro; not realm-specific).
- **R6 (debug-only invariant)**: the spec-mandated caller-realm trap arguments array that flows into an other-realm trap frame trips `InterpreterStackFrame::ValidateRegValue`'s `!NeedMarshalVar` assert on DBG builds (release/test profiles unaffected). The corpus exposure is the cross-realm test262 selection on a debug-static host, which is not a gate. Recorded as an engine-wide invariant/spec tension; not relaxed unilaterally.

## 6. Expected C4 delta (recorded, not yet applied)

Engine-defect edition-selected failures: **−132 expected** (130 cross-realm +
2 same-realm `base-ctor-revoked-proxy.js`), i.e. the edition-selected total
4,080 → ~3,948 and cross-realm 185 → 55. [c4-gate.md](c4-gate.md)'s
engine-defect figure should be updated to the C4 refresh's measured numbers
after review acceptance of this lane; the disabled-flag (B/SAB) and
detached-buffer (C) families do not move (they are bucketed elsewhere).
