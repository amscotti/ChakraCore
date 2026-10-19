# C2-S13 follow-up: FinalizationRegistry

Status: **pending integration** (branch `s13-fr`, base `1c983ef9c`; initial
implementation `3d9012a8d`, review fixes `63d77e9a4`). Mac no-JIT only; JIT
evidence is not claimed and B4 owns the JIT configuration claims. `WeakRef` remains as recorded in [c2-s13-weakref.md](c2-s13-weakref.md);
this slice adds the separate `FinalizationRegistry` cleanup-job mechanism.

## Scope

1. `new FinalizationRegistry(cleanupCallback)` with a callable callback,
   `NewTarget`/`Reflect.construct` prototype selection (including
   `GetFunctionRealm` through Proxy and bound-function target chains),
   subclassing, `length` 1, `name`, non-writable/non-configurable `prototype`,
   and non-constructible `register`/`unregister` methods.
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
5. Real `-CollectGarbage` and `WScript.SetTimeout` job-boundary tests, plus
   focused Proxy/bound NewTarget, callback-retry/remaining-cell, and expected
   callback-error tests. No fake GC or timing shim was added.

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
  when the host has no native promise-continuation/job callback. Task
  creation/enqueue failure resets cleanupJobScheduled before propagating;
  closed ScriptContexts are not scheduled.
Memory accounting:
  One registry block plus one thread-list slot per registry (until the next
  boundary removes collected registries), one list buffer per registry with
  cells, one cell per registration, and shared weak handles per distinct
  target/token. The registry list keeps only leaf handles, never registries.
  Held values are strong only while a cell exists; target/token edges are
  weak. A host that never drains the queue retains scheduled task functions and
  their registries/cells until the tasks run or the runtime is disposed; each
  new outermost boundary may enqueue one idempotent retry task.
```

## GC and cleanup-job contract

Targets use the existing recycler weak-reference map. There is no strong target
edge from the registry, cell, thread list, or task function. The registry is
also weak in the thread-wide list; when the registry is collected, the sweep
clears its handle and the next outermost boundary removes the list slot.

At `ScriptContext::OnScriptEnd` with `isRoot && callRootLevel == 1`, after
`ClearKeptObjects`, `ThreadContext::ScheduleFinalizationRegistryCleanupJobs`
walks the thread-wide weak-registry list. A live, open registry with at least
one cleared target handle schedules one cleanup task through
`JavascriptLibrary::EnqueueTask`. The task is a recycler `RuntimeFunction` with
a strong `Field(registry)` and is rooted by the host queue. This is the same
host job queue used by Promise reactions: `ch` installs
`WScriptJsrt::PromiseContinuationCallback`, which queues the task on its
`MessageQueue`; timers and promise continuations are separate outermost host
calls/jobs. No callback runs synchronously inside `OnScriptEnd`.

`cleanupJobScheduled` is advisory within one scheduling attempt. If a task is
still pending at the next outermost boundary, the flag is cleared and an
idempotent retry task is enqueued. This recovers a dropped/late host task, at
the cost of duplicate no-op tasks when the host merely delays it; a host that
never drains the queue can accumulate one retry per job. Task creation or
`EnqueueTask` failure clears the flag before rethrowing so the next boundary can
retry. `cleanupJobActive` prevents nested/reentrant cleanup, and a closed
ScriptContext is not scheduled.

`CleanupDeadCells` removes one cell before invoking the callback with
`CallInfo(CallFlags_Value, 2)` and `this = undefined`. If the callback throws,
the exception propagates to the host job (the `ch` host prints it and continues
its message loop); the active guard is cleared, the remaining dead cells stay
registered, and the next boundary schedules a retry. Cells whose targets are
still marked are skipped. A callback that unregisters more cells or registers
new ones observes the normal list mutation rules.

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
`out/test262-s13-fr-after4/results.json`, and
`out/test262-s13-fr-weakref-before/results.json` /
`out/test262-s13-fr-weakref-after-fix/results.json`. Controls passed 18/18 on
each run. The debug-static binary also passed the FinalizationRegistry corpus
94/94 (`out/test262-s13-fr-after-debug2`).

Focused `test/es7/finalization-registry.js` covers constructor/call state,
descriptors, non-constructible methods, target/token eligibility, duplicate
registrations, all-match unregister, target vs held value, object/primitive
held values, heldValue strong retention (including a live cell across a job
boundary), weak token lifetime, no cleanup while a target is strongly
reachable, cleanup after it becomes unreachable, multiple registries, and
target/token non-retention using real collections. It passed 100/100 direct
runs on the test profile and the `es7` folder passed 72/72 on both test and
debug-static.

`test/es7/finalization-registry-realm.js` covers `GetFunctionRealm` through a
same-thread child realm for a normal function, bound function, Proxy, nested
Proxy, and bound Proxy whose target function has an undefined `prototype`; an
object-valued `NewTarget.prototype` still wins. `test/es7/
finalization-registry-errors.js` covers a callback that throws on its first
cell and requires the remaining cells to be retried by a later cleanup task;
the expected host error is in `finalization-registry-errors.js.baseline`. Both
focus tests passed 20/20 direct runs on test and debug-static.

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
  --checkout "$PWD/out/test262" --output "$PWD/out/test262-s13-fr-after4" \
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
  `tools/build/check_regression.py` pins. The three registered focus tests make
  the expected counts test **2007** and debug-static **2059** (the prior commit
  already accounted for `finalization-registry.js` at 2005/2057; the realm and
  errors tests add two more). Integration must move the pins. No full gate was
  run here.
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
  `finalization-registry-errors.js.baseline`. Other hosts may report the job
  error differently. The callback-error text is still the generic
  `JSERR_FunctionArgument_NeedObject` for target/token validation nits.
- **Cross-realm Symbol identity:** symbol targets/tokens use the recycler weak
  handle, but a cross-realm Symbol clone is a distinct primitive identity. This
  is inherited from the existing Symbol cloning/`WeakRef` behavior and is not
  fixed in this batch.
- **Host-dispatch/recycler restriction:** objects without a recycler allocation
  (host-dispatch/external surfaces) are rejected with TypeError rather than
  retained strongly; no test262 scenario exercises that host surface.
- **Dropped tasks:** a delayed task is intentionally retried at the next
  boundary, so duplicates are possible; a host that never drains accumulates
  one idempotent task per job until the runtime is disposed or a task runs.
- **No `$262.gc`:** test262 GC-timing tests cannot run; focused tests use the
  `ch` `-CollectGarbage` intrinsic and real timer jobs.
- **Reentrancy coverage:** callbacks that unregister/register/GC are handled by
  the loop and covered in focused tests, but cross-realm and nested-host-call
  combinations are not exhaustively stress-tested.
