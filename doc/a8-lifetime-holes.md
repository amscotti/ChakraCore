# A8 lifetime holes — fix receipt

Lane A8 (Node-API lifetime holes) in `lib/NodeApi`. All edits are in the
isolated worktree; nothing is committed. Normative ownership rules are from
[doc/memory-management.md](memory-management.md) (§2 external-buffer domain,
§3 ownership record, §5 external/buffer finalizers, §7 async ownership).

## A8-1: orphaned external-ArrayBuffer finalizer records (medium-low)

Mechanism: `g_ext_queues` in `lib/NodeApi/NodeApiBinary.cpp` is keyed by
env generation, but `DrainExternalFinalizers` returned early when
`LookupLiveEnv` failed and had a single erase site for the calling live
env, while `chakra_nodeapi_destroy_env` never drained. Records
enqueued-but-undrained at destroy leaked, and buffers collected after
destroy enqueued under a dead generation no Drain could fetch — while the
ownership comment (`NodeApiBinary.cpp:71-79`) claimed dead-env records are
"deleted without invocation" with no such path.

Fix (`NodeApiBinary.cpp`, `NodeApiBinary.h`, `NodeApiEnv.cpp`):

- `EnqueueRecord` now checks `LookupLiveEnvWithGeneration(rec->env,
  rec->generation)` first (registry by value, no dereference of a dead
  env; lock order registry-then-ext, never the reverse). On miss the
  record is deleted inline without invoking the user finalizer — a dead
  generation never enters the queue, so no Drain can fetch it.
- Destroy step 2 calls `DrainExternalFinalizers(env_value)` (Closing env,
  context entered — same posture as wrap finalizers): records queued while
  Alive are invoked exactly once.
- Destroy step 7, after the live entry is erased, calls
  `NodeApiBinary_DestroyForEnv(env, generation)`, which deletes any
  stragglers for that dead generation without invocation (pure native, no
  context). Ordering (erase-then-discard) closes the enqueue window.
- Test introspection: `NodeApiBinary_ExternalLiveCount()` /
  `NodeApiBinary_ExternalQueuedCount()` (always-compiled atomics + locked
  scan; no behavior change).

Ownership record (memory-management.md §3):

```text
Resource / creation call: ExternalRecord (new) in
  napi_create_external_arraybuffer, transferred to
  JsCreateExternalArrayBuffer as callbackState.
Domain: provider native heap (C++ new/delete); backing lifetime == process.
Owner; transfer point: the JS ArrayBuffer's finalize slot after
  JsCreateExternalArrayBuffer; before transfer, creation-failure paths
  delete inline (now via DeleteExtRecord).
Borrow invalidation: GC finalization (trampoline) or detached-state
  transfer; destroy (Closing/Dead) or post-destroy collect.
Potential user-code/reentrancy: trampoline enqueues only (no user code);
  destroy step-2 drain invokes user finalizers with Closing env + entered
  context (queue moved out first, so reentrant drain sees empty).
Thread/context: trampoline on arbitrary GC thread (registry + ext mutex
  only); drain/destroy-tail on owner thread.
Release: invoke-then-delete (live drain, incl. destroy step 2) or
  delete-without-invocation (enqueue-time dead check, step-7 tail).
  Exactly once: live-counted (see tests).
```

## A8-2: stale TSFN drain/finalize host tasks validated by raw pointer (medium-low)

Mechanism: posted tasks held no native ref; re-validation was
`g_tsfns.find(st)` + pointer compare. After teardown finalized+freed the
state, a new TSFN reusing the same address passed validation, so a stale
task could operate on (drain/abort/finalize) the new TSFN.

Fix (`lib/NodeApi/NodeApiAsync.cpp`): per-state `tsfn_generation` from a
process-wide monotone counter, issued at create before map publication
(immutable after). Both posts (`TsfnDrainTramp` in
`napi_call_threadsafe_function` incl. retry, `TsfnFinalizeTramp` in
`ScheduleFinalize`) now post a nothrow `TsfnTaskArg{state, generation}`;
both tramps claim via `ClaimTsfnTaskArg` (validate map hit + pointer +
generation under the map mutex, take the native ref; always delete the
arg; never dereference a stale state — `it->second` is live on the match
path). Arg OOM behaves like a post failure (existing rollback).
Handle-value `LookupTsfn` API lookups are unchanged (pointer-only, same
irreducible ABA as the env registry A5; out of scope).

## A8-3: provider promise-continuation queue never unlinked (low-medium)

Mechanism: `g_queues` (`lib/NodeApi/NodeApiPromise.cpp`) entries were
never erased, so a `JsContextRef` address reuse aliased a stale queue and
skipped fresh installation; last-writer-wins across contexts unchanged.

Fix: `NodeApiPromise_DestroyForEnv` unlinks the entry by value after
releasing leftover tasks (erase only if the context still maps to the
same queue object). The queue object itself stays process-lifetime (not
deleted): the engine trampoline still references it and later teardown
steps may still trigger it. Continuation install/pump behavior otherwise
unchanged.

## Verification

All commands run from the worktree root. Engine/profile builds via
`/tmp/opencode/cc-gate-lock.sh` (serialized gate) with `mise exec -- zig`.

Custom drivers (throwaway, under `/tmp`, not committed) compile the
provider TUs from source against the engine dylib with the probes'
flags (`-O3`, `CHAKRA_NODEAPI_TEST_HOOKS`):

- A8-1 `/tmp/a8_ext_driver.cpp` — destroy-before-drain (V1) and
  destroy-then-GC (V2), helper-frame creation + 30 rounds of stack churn
  and forced GC so the collection premise is non-vacuous:
  - patched tree: `setup: sacrificial collect+drain calls=1`,
    `V1 ... before=1 after=2 (expect +1) / V1 PASS`, `V2 ... before=2
    after=2 / V2 PASS`, `A8EXT: PASS` (exit 0; -O3, test profile).
  - negative control (provider fixes stashed, `git stash push -- lib/`,
    rebuilt, rerun): `V1 ... before=1 after=1 / V1 FAIL: finalizer did
    not run (record orphaned)`, `A8EXT: FAIL` (exit 1). Fixes restored
    with `git stash pop`. This proves the driver detects the bug and the
    fix resolves it on the real code paths.
  - robustness note: an earlier driver shape at -O3 showed `queued=0`
    post-GC (conservative stack residue pinned the buffer), making V1
    vacuous; the helper-frame + churn + sacrificial-proof pattern fixed
    the premise, and the committed probe case additionally asserts the
    premise via the live/queued counters (poll-until-queued/collected,
    bounded 30 attempts).
- A8-2 `/tmp/a8_tsfn_driver.cpp` — FakeHost-held drain task, destroy env1
  with the task pending, minimal-gap realloc churn on env2 until the
  freed `TsfnState` address is reused, queue a call on the new TSFN, pump
  exactly the one stale task and count deliveries:
  - patched tree: `address reuse: YES`, `delivered=0 / STALE-DROPPED`
    (2 of 3 runs reused; 3rd run no-reuse weak pass, no crash).
  - negative control (fixes stashed): `address reuse: YES`,
    `delivered=1 / STALE-DELIVERED: stale task drained the new TSFN
    (BUG)` (2 of 3 runs reused). Bug reproduced end-to-end; fix
    verified on the same sequence.

Maintained probes (test profile, `SKIP_BUILD=1`, engine
`zig-out/bin/libChakraCore.dylib` built via `zig build ch-nodeapi`):

- binary: `BINARY_PROBE: PASS (242/242)` — includes the new
  `external-destroy-before-drain` case (17 assertions: queued-proof,
  finalizer-at-destroy with data/hint match, live/queued back to
  baseline, post-destroy collect dropped without invocation, root
  baseline).
- async: `async_probe: 280 checks, 0 failures`.
- gate: `gate_probe: 51 checks, 0 failures`.
- teardown: `teardown_probe: 92 checks, 0 failures` (destroy ordering
  markers unchanged — the step-2 external drain emits no marker).
- promise_script: `promise_script_probe: 195 checks, 0 failures`.
- asyncctx: `asyncctx_probe: 203 checks, 0 failures`.
- env_addr_reuse: `env_addr_reuse_probe: 618 checks, 0 failures`.

JIT profile (engine `out/jit/bin/libChakraCore.dylib` via
`zig build -Dprofile=jit`, exit 0; one transient libtool static-archive
flake printed a "failed command" line on the first attempt but the
rebuild was fully up-to-date and green — engine-side, unrelated to this
lane; same runners, `SKIP_BUILD=1`, both legs each):

- binary: `PASS (242/242)` test + jit.
- async: `280 checks, 0 failures` test + jit.
- gate: `51 checks, 0 failures` test + jit.
- teardown: `92 checks, 0 failures` test + jit.
- promise_script: `195 checks, 0 failures` test + jit.
- asyncctx: `203 checks, 0 failures` test + jit.
- env_addr_reuse: `618 checks, 0 failures` test + jit.

All runners report `ALL PASS (test + jit)`.

## Files changed

- `lib/NodeApi/NodeApiBinary.cpp`, `lib/NodeApi/NodeApiBinary.h`
- `lib/NodeApi/NodeApiAsync.cpp`
- `lib/NodeApi/NodeApiPromise.cpp`
- `lib/NodeApi/NodeApiEnv.cpp`
- `test/nodeapi/probes/binary_probe.cpp` (destroy-before-drain case)
- `doc/a8-lifetime-holes.md` (this file)

## Unresolved / residual

- Detached-external finalizer JsRT behavior (pre-existing, recorded in
  the ownership comment) is out of scope.
- A8-2 natural address reuse in the repro driver is probabilistic; the
  driver reports whether reuse was observed.
