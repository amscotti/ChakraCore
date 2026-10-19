# C2-S13: bounded WeakRef slice

Status: **landed in `803331b51`** (feature fixes `a8267f451`, `b963bc9b1`,
`e94b2dd6a`); current integrated gates **2004/2056**; Mac no-JIT (JIT evidence is not claimed; B4 owns the JIT
configuration claims). WeakRef corpus **56/58**, **56 fail→pass / 0
pass→fail**; two residual scenarios require `FinalizationRegistry`. This
slice covers `WeakRef` only. `FinalizationRegistry` remains a separate C3
follow-up.

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
  ThreadContext::keptObjects is a RecyclerRootPtr<JsUtil::List<
  RecyclableObject*, Recycler>>, shared by every ScriptContext/realm on the
  agent/thread. The root keeps the list and its pointer array traceable;
  KeepDuringJob adds a target once, and ClearKeptObjects zeroes and clears the
  list at the next outermost root job boundary. It is not a permanent strong
  target reference: the list is empty outside a job. The list buffer uses
  Field(RecyclableObject*, Recycler), i.e. WriteBarrierPtr on barrier builds,
  so Add publishes through the recycler write barrier.
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
  ThreadContext/Recycler and the JS execution thread. The kept list is shared
  across same-thread ScriptContexts/realm loads, and is cleared only at the
  outermost root call boundary; nested root entries and forced debugger
  entries do not clear it.
Failure/OOM/shutdown:
  Eligibility is checked before allocation. A target with no recycler
  allocation (host-dispatch/external surface) is rejected with TypeError
  instead of being retained strongly or published. OOM follows the recycler
  allocation path; no partial WeakRef is returned. ThreadContext's destructor
  clears and unroots the kept list before deleting the recycler; Close does
  not unroot it.
Memory accounting:
  One recycler leaf handle per distinct weak target (shared by
  FindOrCreateWeakReferenceHandle), plus the WeakRef object. The kept list is
  capacity-bounded by the largest single job's distinct kept targets and is
  cleared/zeroed at each job boundary; it is not process-lifetime retention.
  Dedupe is a linear Contains, so a pathological job keeping N distinct
  targets is O(N^2); the common case is small.
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

Profiler/debug registration: `ProfilerRegisterWeakRef` registers the dynamic
`WeakRef` constructor and `deref` property ids, and `ProfilerRegisterBuiltIns`
includes WeakRef. `ScriptContext::DumpTypeCounts` now prints a WeakRef row
using `TypeIds_WeakRef`.

`AddToKeptObjects` is implemented by `ThreadContext::KeepDuringJob` and the
agent/thread-wide rooted `keptObjects` list. The constructor calls it before the
reentrant `NewTarget.prototype` lookup, and `deref` calls it before returning a
live target. `ScriptContext::OnScriptStart`/`OnScriptEnd` clear the thread list
when `isRoot && ThreadContext::GetCallRootLevel() == 1`, i.e. at the outermost
host call boundary. `JsRT` API entry (`BEGIN_ENTER_SCRIPT` with
`isCallRoot=true`) and `CallRootFunction(inScript=false)` both provide that
boundary; timer and promise-continuation callbacks are separate host calls. A
same-thread child realm created by `WScript.LoadScript(..., 'samethread')` gets
no independent root call, but its kept targets share this list and are cleared
by the outer realm's boundary. Nested root entries (level > 1) and forced
debugger entries do not clear the set. `ThreadContext`'s destructor clears and
unroots the list before deleting the recycler.

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
  job-1 constructor targets. In-job liveness is exact: 1000/1000 object,
  1000/1000 symbol, and 1000/1000 deref targets. Cross-job clearing allows
  conservative-stack remnants: at least 995/1000 object, 995/1000 symbol, and
  495/500 WeakMap-key targets cleared. A `WScript.LoadScript(..., 'samethread')`
  child realm creates 64 object and 64 symbol WeakRefs; all must be alive
  during the outer job, and at least 63/64 of each must be cleared after the
  outer job boundary. This child realm has no outermost root call of its own,
  so it is the regression for the thread-wide kept list.
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
`out/test262-s13-weakref-after5/results.json`. The controls gate passed 18/18
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
are fully alive in job 1, including 64 object and 64 symbol targets created in
a same-thread child realm; 1000/1000 `deref` targets remain alive through an
in-job GC after job 2 starts; and at least 995/1000 object, 995/1000 symbol,
495/500 WeakMap-key, 63/64 cross-realm object, and 63/64 cross-realm symbol
targets are cleared after the job-1 kept set is released. The allowance is for
conservative-stack remnants, not for retained kept-set entries. The callback
exits 1 on any threshold failure so the regression runner cannot pass on a
printed exception.

Commands (all builds/test262 runs serialized by
`/tmp/opencode/cc-gate-lock.sh`; cache and checkout are local):

```sh
mise exec -- zig build --cache-dir "$PWD/.zig-cache"
python3 tools/test262/run.py --binary "$PWD/out/s13-weakref-before-bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-before" \
  'test/built-ins/WeakRef/**/*.js'
python3 tools/test262/run.py --binary "$PWD/zig-out/bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-weakref-after5" \
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

Repeat stability (after the conservative allowance): test-profile
`weakref.js` 200/200 and `weakref-gc.js` 200/200 runs passed; debug-static
`weakref.js` 100/100 and `weakref-gc.js` 100/100 runs passed. The earlier exact
cross-job thresholds were flaky because 1--3 conservative-stack remnants could
survive.

Adding the global `WeakRef` intrinsic changed the debugger's global-property
view, so 66 shared `.dbg.baseline` files were regenerated from the test run.
All 66 add the `WeakRef` property; 24 of them also carry ordering-only changes
to `arguments`/`length`/`name` in function displays. This is why the baseline
diff is broad; it is not a runtime behavior change.

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
- Flake evidence: the cross-job thresholds were exercised repeatedly after the
  allowance change; see the final repeat-run report on the branch. The previous
  exact 1000/1000 thresholds were flaky because conservative-stack remnants
  can leave 1--3 targets.
- JIT/TTD/debugger evidence: no JIT claim; no TTD snapshot support was added
  for `[[Target]]` beyond the normal dynamic-object shape. B4 owns the JIT
  matrix. The kept-list `Add` stores through `JsUtil::List`'s
  `Field(RecyclableObject*, Recycler)` buffer, which is `WriteBarrierPtr` under
  `GLOBAL_ENABLE_WRITE_BARRIER`; the weak handle itself still relies on the
  existing JsRT `RecyclerWeakReference` primitive. No concurrent-GC/GCStress
  run was performed; B4/GCStress must cover both before any such claim.
- The repository's build-contract documents record their pinned 538/682 unit
  baselines; this new TU updates the build/test-table assertions to
  Mac 539 (537 static) and Linux 683 but does not rewrite those historical
  measurement sections.
- Regression gate pins move by two registered tests (`weakref.js`,
  `weakref-gc.js`): test 1982 -> 1984 and debug 2034 -> 2036. Both focused gates
  are green (1984/0 test, 2036/0 debug-static) in this branch.
