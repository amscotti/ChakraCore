# C2-S13: bounded WeakRef slice

Status: `<pending>` — implemented in the working tree on Mac no-JIT (JIT
evidence is not claimed; B4 owns the JIT configuration claims). This slice
covers `WeakRef` only. `FinalizationRegistry` remains a separate C3 follow-up.

## Scope

1. `new WeakRef(target)` for an Object or an unregistered Symbol target
   (ES2023 `CanBeHeldWeakly`); primitives and `Symbol.for(...)` targets throw
   `TypeError`.
2. `WeakRef.prototype.deref()` with the WeakRef brand check; returns the exact
   live target or `undefined` after the target is cleared, and keeps a live
   result alive through the end of the current job.
3. Constructor/prototype shape: `length` 1, `name` `"WeakRef"`, non-writable
   non-configurable `WeakRef.prototype`, non-enumerable `deref` with `length`
   0, `prototype.constructor`, `Symbol.toStringTag`, subclassing and
   `Reflect.construct`/`NewTarget` prototype selection including a cross-realm
   fallback.
4. A recycled weak-reference handle that does not retain the target, plus the
   spec's constructor `AddToKeptObjects` step. No feature flag was added and no
   built-in bytecode property id changed.

Not in scope: `FinalizationRegistry`, `WeakMap.prototype.getOrInsert*`, and
cross-realm `deref` identity beyond the engine's existing `CrossSite` behavior.

## Ownership and lifecycle record

```text
Resource / creation call / source location:
  RecyclerWeakReference<RecyclableObject> entry created by
  Recycler::FindOrCreateWeakReferenceHandle (lib/Runtime/Library/
  JavascriptWeakRef.cpp::JavascriptLibrary::CreateWeakRef); the handle is
  allocated with AllocWeakReferenceEntry (WeakReferenceEntryBits = LeafBit).
Domain and owner:
  The ScriptContext's Recycler owns the object graph. JavascriptWeakRef owns
  the handle through Field(RecyclerWeakReference<RecyclableObject>*)
  targetWeakRef. The global Recycler::weakReferenceMap tracks the handle
  weakly for sweep; it is not a strong owner of the target.
Kept-object set:
  ScriptContext::keptObjects is a RecyclerRootPtr<JsUtil::List<
  RecyclableObject*, Recycler>>. The root keeps the list and its pointer array
  traceable; KeepDuringJob adds a target once, and ClearKeptObjects zeroes and
  clears the list at the next root job boundary. It is not a permanent strong
  target reference: the list is empty outside a job.
Physical allocation / release:
  Recycler-managed leaf entry plus the normal recycler block containing the
  WeakRef object. Nothing is allocated with malloc/HeapNew and there is no
  native free. The handle is reclaimed by the recycler after the WeakRef is no
  longer marked; target bytes belong to the target's normal owner.
Aliases / publication:
  The handle pointer is private to the WeakRef. deref returns a target pointer
  (marshalled through CrossSite when needed) but does not publish the handle.
Borrow invalidation:
  GC mark/sweep (Recycler::SweepWeakReference) clears handle->strongRef when
  the target's heap block is no longer marked and removes the map entry;
  runtime disposal nulls every weak reference. Target collection and
  WeakRef-instance collection are independent transitions.
User code/reentrancy:
  NewTarget.prototype lookup can run a getter/proxy trap after the target and
  WeakRef have been created; exceptions from that lookup propagate. The
  constructor calls KeepDuringJob(target) before that lookup, so a getter that
  runs an exhaustive GC cannot clear the still-live target. The target is
  validated before the lookup, per spec.
Thread/context:
  ScriptContext/Recycler and JS execution thread only. The kept list is
  cleared only at the outermost root call boundary; nested root entries and
  forced debugger entries do not clear it.
Failure/OOM/shutdown:
  Eligibility is checked before allocation. A target with no recycler
  allocation (host-dispatch/external surface) is rejected with TypeError
  instead of being retained strongly or published. OOM follows the recycler
  allocation path; no partial WeakRef is returned. Recycler shutdown clears
  and unroots the kept list before finalization.
Memory accounting:
  One recycler leaf handle per distinct weak target (shared by
  FindOrCreateWeakReferenceHandle), plus the WeakRef object. The kept list is
  capacity-bounded by the largest single job's distinct kept targets and is
  cleared/zeroed at each job boundary; it is not process-lifetime retention.
Tests:
  test/es7/weakref.js and the pinned test262 WeakRef corpus; see Evidence.
```

## GC and cleanup contract

`JavascriptWeakRef` is a normal recycler block, so the conservative normal-block
scan reaches its `targetWeakRef` field and marks the handle. The handle is a
leaf block and is not scanned, so its `strongRef` does not trace the target.
During the collection's weak sweep, `Recycler::SweepWeakReference` keeps the
handle only while both the handle and its target are marked; a dead target
turns `deref()` into `undefined`, and a dead WeakRef lets the handle be
removed and reclaimed. The same sweep is used by the existing weak-key regions,
so object targets shared with `WeakMap`/`WeakSet` clear on the same collection.

A handle may be shared by several WeakRefs for the same target. Sweep keeps it
only while the handle itself is marked by at least one owner; all WeakRef owners
for that target must be unmarked before the entry is removed and reclaimed. If
the target dies first, the still-marked handle stays with `strongRef == null`
until its WeakRef owners die, so deref correctly returns undefined.

Unregistered symbol targets use the same typed handle. The library's symbol
cache is already a weak dictionary, so an unregistered symbol with no strong
root can be cleared. Registered `Symbol.for` symbols are rejected before a
handle is created.

`TypeIds_WeakRef` is `80`, inside `TypeIds_LastBuiltinDynamicObject` (now
`TypeIds_WeakRef`) and below `TypeIds_LastTrueJavascriptObjectType` (now
`TypeIds_ModuleRoot = 82`), so `IsSpecialObjectType`/`IsExposedType` and the
JIT builtin-object range treat it as a normal builtin JavaScript object. The
subsequent internal type ids are shifted; no serialized property id changed.

`AddToKeptObjects` is implemented by `ScriptContext::KeepDuringJob` and the
rooted `keptObjects` list. The constructor calls it before the reentrant
`NewTarget.prototype` lookup, and `deref` calls it before returning a live
target. `ScriptContext::OnScriptStart`/`OnScriptEnd` clear the list when
`isRoot && ThreadContext::GetCallRootLevel() == 1`, i.e. at the outermost host
call boundary. `JsRT` API entry (`BEGIN_ENTER_SCRIPT` with `isCallRoot=true`)
and `CallRootFunction(inScript=false)` both provide that boundary; timer and
promise-continuation callbacks are separate host calls. Nested root entries
(level > 1) and forced debugger entries do not clear the set. `Close` clears and
unroots the list before recycler shutdown.

## Host/test262 GC behavior

- `$262.gc` is not provided by this host. `tools/test262/harness.js` adds only
  `detachArrayBuffer`; test262 GC-timing tests cannot use `$262.gc`.
- The `ch` host provides `-CollectGarbage`, which installs the
  `CollectGarbage()` global (`GlobalObject::EntryCollectGarbage`). The focused
  tests use that flag and perform real exhaustive collections; they do not
  simulate or time a fake collection.
- `test/es7/weakref-gc.js` runs its first checks in the top-level job, then
  schedules a `WScript.SetTimeout` callback. The timer runs through
  `JsCallFunction`/`ContextAPIWrapper` as a separate outermost host call/job.
  The callback verifies deref's KeepDuringJob with in-job GC, then collects the
  job-1 constructor targets. The thresholds require 1000/1000 object,
  1000/1000 symbol, and 500/500 WeakMap-key targets cleared, and 1000/1000
  deref targets retained in-job.
- The pinned `test/built-ins/WeakRef/**/*.js` corpus contains 29 files and 58
  strict/default scenarios and has no GC-timing scenario. One file
  (`prototype/deref/this-does-not-have-internal-target-throws.js`) also requires
  `FinalizationRegistry`, which is intentionally absent and remains a residual
  failure.

## Evidence

Pinned test262 `2e0a5676…`; Mac test profile, no JIT. The before run used the
base-commit binary copied to `out/s13-weakref-before-bin/` so the measured
later build could not replace it.

| Corpus | Before | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| `built-ins/WeakRef` (29 files / 58 scenarios) | 0 | **56** | 56 | 0 |

The exact before/after per-file result sets are in
`out/test262-s13-weakref-before/results.json` and
`out/test262-s13-weakref-after3/results.json`. The controls gate passed 18/18
scenarios on both runs. The only remaining failures are the two scenarios of
`prototype/deref/this-does-not-have-internal-target-throws.js`, both
`ReferenceError: 'FinalizationRegistry' is not defined` (the file's
FinalizationRegistry assertion is outside this slice).

Focused `test/es7/weakref.js` covers constructor/call behavior, target identity,
unregistered/registered symbols, descriptors, brand checks, subclassing,
`Reflect.construct`, abrupt `NewTarget.prototype` lookup, coexistence with
WeakMap/WeakSet keys, and constructor KeepDuringJob: 1000/1000 object and
1000/1000 symbol targets remain alive through two in-job collections. It is
registered with `-CollectGarbage -args summary -endargs` and passes on the
test-profile binary.

`test/es7/weakref-gc.js` adds the job-boundary evidence: constructor targets
are fully alive in job 1, then 1000/1000 `deref` targets remain alive through an
in-job GC after job 2 starts, and 1000/1000 object targets, 1000/1000 symbol
targets, and 500/500 WeakMap key targets are cleared after the job-1 kept set is
released. The callback exits 1 on any threshold failure so the regression
runner cannot pass on a printed exception.

Commands (all builds/test262 runs serialized by
`/tmp/opencode/cc-gate-lock.sh`; cache and checkout are local):

```sh
mise exec -- zig build --cache-dir "$PWD/.zig-cache"
python3 tools/test262/run.py --binary "$PWD/out/s13-weakref-before-bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-before" \
  'test/built-ins/WeakRef/**/*.js'
python3 tools/test262/run.py --binary "$PWD/zig-out/bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-after3" \
  'test/built-ins/WeakRef/**/*.js'
(cd test/es7 && ../../zig-out/bin/ch -CollectGarbage -args summary -endargs weakref.js)
(cd test/es7 && ../../zig-out/bin/ch -CollectGarbage weakref-gc.js)
```

The test262 host adds `-nonative -Test262 -CollectGarbage` by default; the
WeakRef corpus itself does not call `CollectGarbage`.

Gate evidence (both runs serialized by the shared gate lock):

- `mise exec -- zig build check-regression --summary line --cache-dir "$PWD/.zig-cache"`:
  **1984 passed / 0 failed**, 569/569 steps.
- `mise exec -- zig build check-regression -Dprofile=debug-static --summary line
  --cache-dir "$PWD/.zig-cache"`: **2036 passed / 0 failed**, 546/546 steps.
- `python3 tools/build/check_regression.py --self-test`: all negative controls
  pass with the 1984/2036 profile pins.

Adding the global `WeakRef` intrinsic changed the debugger's global-property
view, so 66 shared `.dbg.baseline` files were regenerated from the test run
(they add the `WeakRef` property and current function-property order). This is
why the baseline diff is broad; it is not a runtime behavior change.

## Residual

- `FinalizationRegistry`: not implemented; its cleanup-job contract and
  host-observable finalization remain a separate C3 project. The one WeakRef
  corpus scenario that constructs a FinalizationRegistry stays failed.
- S13's symbol entries in `WeakMap`/`WeakSet` remain strongly held by their
  per-collection symbol dictionaries. A symbol that is both a WeakMap key and a
  WeakRef target is therefore kept alive by the WeakMap; this is the
  already-recorded S13 memory-bound residual, now observable through WeakRef.
  Object keys retain the existing weak path.
- Host-dispatch/external JS objects without a recycler allocation are rejected
  rather than weakly retained (TypeError), matching the existing WeakSet
  HostDispatch limitation. No test262 scenario exercises that host surface.
- JIT/TTD/debugger evidence: no JIT claim; no TTD snapshot support was added
  for `[[Target]]` beyond the normal dynamic-object shape. B4 owns the JIT
  matrix. No concurrent-GC/GCStress run was performed; the handle uses the
  existing JsRT `RecyclerWeakReference` primitive, but its concurrent-GC
  behavior must be covered by the B4/GCStress matrix before any such claim.
- The repository's build-contract documents record their pinned 538/682 unit
  baselines; this new TU updates the build/test-table assertions to
  Mac 539 (537 static) and Linux 683 but does not rewrite those historical
  measurement sections.
- Regression gate pins move by two registered tests (`weakref.js`,
  `weakref-gc.js`): test 1982 -> 1984 and debug 2034 -> 2036. Both focused gates
  are green (1984/0 test, 2036/0 debug-static) in this branch.
