# C2-S13 follow-up: FinalizationRegistry

Status: **pending integration** (branch `s13-fr`, base `1c983ef9c`). Mac
no-JIT only; JIT evidence is not claimed and B4 owns the JIT configuration
claims. `WeakRef` remains as recorded in [c2-s13-weakref.md](c2-s13-weakref.md);
this slice adds the separate `FinalizationRegistry` cleanup-job mechanism.

## Scope

1. `new FinalizationRegistry(cleanupCallback)` with a callable callback,
   `NewTarget`/`Reflect.construct` prototype selection, subclassing,
   `length` 1, `name`, non-writable/non-configurable `prototype`, and
   non-constructible `register`/`unregister` methods.
2. `register(target, heldValue, unregisterToken?)` with ES `CanBeHeldWeakly`
   targets and tokens (Object or unregistered Symbol; `Symbol.for(...)` and
   primitives rejected), duplicate registrations, a strong `heldValue` while
   the registration exists, weak target/token edges, and `undefined` return.
3. `unregister(token)` removes every matching registration, returns the
   boolean result, and never matches a target used as a held value.
4. Cleanup callbacks after target collection at an outermost host-call/job
   boundary, with the held value as the only argument and `undefined` as
   `this`; no callback after unregister, while the target is strongly
   reachable, or after the registry is collected.
5. Real `-CollectGarbage` and `WScript.SetTimeout` job-boundary tests. No fake
   GC or timing shim was added.

Not in scope: `WeakMap.prototype.getOrInsert*`, TTD snapshot support for the
new internal fields, JIT-specific behavior, and concurrent-GC/GCStress.

## Ownership record

```text
Resource / creation call / source location:
  JavascriptFinalizationRegistry is a recycler DynamicObject allocated by
  JavascriptLibrary::CreateFinalizationRegistry (JavascriptFinalizationRegistry.cpp).
  Each [[Cells]] entry is a FinalizationRegistryCell allocated in the
  registry-owned JsUtil::List<FinalizationRegistryCell*, Recycler>. Weak
  target/token edges are RecyclerWeakReference<RecyclableObject> handles from
  Recycler::FindOrCreateWeakReferenceHandle.
Domain and owner:
  The ScriptContext's Recycler owns the object graph. The registry owns its
  cell list; each cell owns its weak target handle, strong heldValue, and
  optional weak unregister-token handle. No malloc/HeapNew allocation and no
  native release authority is introduced.
Physical allocation / release:
  Normal recycler blocks (registry, list buffer, cells) and leaf weak-handle
  blocks (WeakReferenceEntryBits = LeafBit). The recycler reclaims them when
  unmarked. Recycler::SweepWeakReference clears a handle's strongRef when its
  target is no longer marked; the handle itself remains until its owner is
  reclaimed.
Aliases / publication:
  The registry pointer is published to ThreadContext only through a weak
  handle. Cells and held values are never published to the host. The cleanup
  task function stores a strong registry Field and is published through the
  host job queue until it runs.
Borrow invalidation:
  GC sweep clears target/token handles; unregister removes cells and releases
  their held values; registry collection releases the list/cells; runtime
  disposal nulls recycler weak references before recycler teardown.
User code / reentrancy:
  The constructor stores the callback, then performs the reentrant
  NewTarget.prototype lookup after registering the registry. register/unregister
  do no coercion. Cleanup invokes arbitrary JS; it removes the cell before the
  call, so a callback that unregisters/registers/collects sees a consistent
  list. Callback exceptions leave the host job.
Thread/context:
  JS execution thread; ThreadContext/Recycler. The registry list is
  agent/thread-wide so a same-thread child realm's registries are scanned at
  the active realm's outermost boundary. Cleanup tasks are created by the
  registry's own JavascriptLibrary and execute in that realm.
Failure / OOM / shutdown:
  Eligibility and recycler-heap presence are checked before either weak handle
  is added to the weak-reference map. OOM follows the recycler allocation path;
  no partial cell is published. ThreadContext's destructor clears and unroots
  the weak-handle list before deleting the recycler. No cleanup task is created
  when the host has no native promise-continuation/job callback.
Memory accounting:
  One registry block plus one thread-list slot per registry (until the next
  boundary removes collected registries), one list buffer per registry with
  cells, one cell per registration, and shared weak handles per distinct
  target/token. The registry list keeps only leaf handles, never registries.
  Held values are strong only while a cell exists; target/token edges are
  weak. A host that never drains the queue retains a scheduled registry and its
  cells until the task runs or the runtime is disposed.
```

## GC and cleanup-job contract

Targets use the existing recycler weak-reference map. There is no strong target
edge from the registry, cell, thread list, or task function. The registry is
also weak in the thread-wide list; when the registry is collected, the sweep
clears its handle and the next outermost boundary removes the list slot.

At `ScriptContext::OnScriptEnd` with `isRoot && callRootLevel == 1`, after
`ClearKeptObjects`, `ThreadContext::ScheduleFinalizationRegistryCleanupJobs`
walks the thread-wide weak-registry list. A live registry with at least one
cleared target handle schedules one cleanup task through
`JavascriptLibrary::EnqueueTask`, unless a task is already scheduled. The task
is a recycler `RuntimeFunction` with a strong `Field(registry)` and is rooted by
the host queue. This is the same host job queue used by Promise reactions:
`ch` installs `WScriptJsrt::PromiseContinuationCallback`, which queues the task
on its `MessageQueue`; timers and promise continuations are separate outermost
host calls/jobs. No callback runs synchronously inside `OnScriptEnd`.

`CleanupDeadCells` removes one cell before invoking the callback with
`CallInfo(CallFlags_Value, 2)` and `this = undefined`. If the callback throws,
the exception propagates to the host job (the `ch` host prints it and continues
its message loop); the remaining dead cells stay registered and are scheduled
at a later boundary. Cells whose targets are still marked are skipped. A
callback that unregisters more cells or registers new ones observes the normal
list mutation rules.

The cleanup contract is deliberately conservative: if the host has not
installed a native promise-continuation/job callback, no cleanup task is
created. Registrations remain and may be cleaned at a later boundary if a host
callback becomes available. Finalization timing is implementation-defined; the
engine does not force a collection merely to run a callback, and a GC performed
outside a job is observed at the next job boundary.

## Evidence

Pinned test262 `2e0a5676...`; Mac test profile, no JIT. The before binary was
copied from the base commit to `out/s13-fr-before-bin/` before implementation.

| Corpus | Before | After | Fail→pass | Pass→fail |
|---|---:|---:|---:|---:|
| `built-ins/FinalizationRegistry` (47 files / 94 scenarios) | 0 | **94** | 94 | 0 |
| `built-ins/WeakRef` (29 files / 58 scenarios; residual) | 56 | **58** | 2 | 0 |

Exact result sets: `out/test262-s13-fr-before/results.json`,
`out/test262-s13-fr-after3/results.json`, and
`out/test262-s13-fr-weakref-before/results.json` /
`out/test262-s13-fr-weakref-after-final/results.json`. Controls passed 18/18 on
each run. The debug-static binary also passed the FinalizationRegistry corpus
94/94 (`out/test262-s13-fr-after-debug`).

Focused `test/es7/finalization-registry.js` covers constructor/call state,
descriptors, non-constructible methods, target/token eligibility, duplicate
registrations, all-match unregister, target vs held value, object/primitive
held values, heldValue strong retention (including a live cell across a job
boundary), weak token lifetime, no cleanup while a target is strongly
reachable, cleanup after it becomes unreachable, multiple registries, a
throwing cleanup callback (expected host error is in the test baseline), and
target/token non-retention using real collections. It passed 50/50 direct
baseline-mode runs on the test profile, and the `es7` folder passed 70/70 on
both test and debug-static. The test is registered once with
`-CollectGarbage -args summary -endargs`.

Adding the global intrinsic changed the debugger's global-property view. All
66 affected `.dbg.baseline` files were regenerated from the test run; the diff
is the added `FinalizationRegistry` entry (the tests had no other change).
`test/Debugger` + `test/DebuggerCommon` then passed 227/227 (19 + 208).

Commands (builds/test262 serialized by `/tmp/opencode/cc-gate-lock.sh`; cache
and checkout local):

```sh
mise exec -- zig build --cache-dir "$PWD/.zig-cache"
mise exec -- zig build -Dprofile=debug-static --cache-dir "$PWD/.zig-cache-debug"
python3 tools/test262/run.py --binary "$PWD/out/s13-fr-before-bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-fr-before" \
  'test/built-ins/FinalizationRegistry/**/*.js'
python3 tools/test262/run.py --binary "$PWD/zig-out/bin/ch" \
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-fr-after3" \
  'test/built-ins/FinalizationRegistry/**/*.js'
python3 test/runtests.py --test --variants=disable_jit \
  -b "$PWD/zig-out/bin/ch" test/es7
python3 test/runtests.py --debug --variants=disable_jit \
  -b "$PWD/out/debug-static/bin/ch" test/es7
```

Profiler registration uses tracked dynamic property ids for
`FinalizationRegistry`, `register`, and `unregister`, so the checked-in built-in
bytecode property-id table is unchanged. `TypeIds_FinalizationRegistry` sits
inside `TypeIds_LastBuiltinDynamicObject` and below
`TypeIds_LastTrueJavascriptObjectType`; the later internal type ids shift by
one. `JsrDebugUtils`, `IsJsNativeType`, and `DumpTypeCounts` include the new
type.

## Residual and risks

- **B4:** no JIT, TTD snapshot, concurrent-GC, or GCStress claim. The new type
  id and task function are compiled but not behavior-proved with JIT. B4 owns
  those matrices.
- **Gate pins:** per the implementer instruction, this branch does not update
  `tools/build/check_regression.py` pins. Registering the new test makes the
  expected counts test **2005** and debug-static **2057**; integration must move
  the pins. No full gate was run here.
- **Host job contract:** cleanup requires a native promise-continuation/job
  callback. The checked-in JsRT context returns `E_NOTIMPL` from
  `EnqueuePromiseTask` and JsRT embedders without `JsSetPromiseContinuationCallback`
  get no cleanup jobs at all. This is recorded rather than replaced with a
  synchronous callback at `OnScriptEnd`.
- **Retention when the host never drains:** a scheduled task and its registry's
  held values stay rooted by the queue. A thread-list weak-handle slot for a
  collected registry is retained until the next outermost boundary.
- **Timing:** callbacks are not guaranteed at a particular collection; a target
  that remains on the conservative C++ stack may survive. The focused test uses
  allowances for such remnants rather than exact counts.
- **Callback errors:** the expected-error baseline is host-specific and embeds
  the test line number; changing the throwing line requires regenerating
  `finalization-registry.js.baseline`. Other hosts may report the job error
  differently.
- **No `$262.gc`:** test262 GC-timing tests cannot run; focused tests use the
  `ch` `-CollectGarbage` intrinsic and real timer jobs.
- **Reentrancy coverage:** callbacks that unregister/register/GC are handled by
  the loop and covered in focused tests, but cross-realm and nested-host-call
  combinations are not exhaustively stress-tested.
