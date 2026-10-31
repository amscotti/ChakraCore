# host-spawn-check repro tooling — unchecked CreateThread at the agent spawn site

Evidence tooling for the follow-up card filed in
`doc/c2-s6-cross-agent-waitasync.md` ("S6e host teardown repair" →
residual: "`CreateThread`'s return value at the spawn site is unchecked").
Nothing here is wired into the build or test gates; these are
evidence-collection tools.

Defect (pre-fix, `bin/ch/WScriptJsrt.cpp` crossthread branch): the
`child->hThread = ::CreateThread(...)` result is never checked. When the
OS refuses thread creation (resource exhaustion — realistic under agent
churn; the S6e adversarial campaigns created 100-400 runtimes per
process), `hThread == NULL` and two hazards follow, in order:

1. the immediately following
   `WaitForSingleObject(threadData->hevntInitialScriptCompleted, INFINITE)`
   waits forever: the only setter of the parent's event is the child's
   `ThreadProc` after its initial script (`RuntimeThreadData.cpp:107`),
   and that thread never started. Sampled stack of a hung run (test
   profile, base `4f564ea04`): `main → RunScript → ... →
   WScriptJsrt::LoadScript (WScriptJsrt.cpp:879) → WaitForSingleObject →
   CPalSynchronizationManager::BlockThread → _pthread_cond_wait` —
   1532/1534 samples parked there.
2. the dead child stays in `threadData->children` with a NULL handle, so
   the end-of-main per-child join (`WaitForSingleObject(NULL)`) trips its
   Assert on debug profiles or deletes a never-started child on release —
   reachable only if something else breaks the wait, because hazard 1
   hangs first.

## Why an interposer instead of `ulimit -u`

The card suggested a `ulimit -u`-restricted run. Measured on the evidence
machine (macOS ARM64): **RLIMIT_NPROC does not gate thread creation on
macOS** — it is enforced per-uid at fork()/posix_spawn() only. 40 node
`worker_threads` spawn fine under `ulimit -u 5`, and ch itself spawns 20
agents under `ulimit -u 5` cleanly. The per-process thread budget
(`kern.num_taskthreads`, 16384 here) is a system-wide boot sysctl an
unprivileged driver cannot lower. So `failthreads.c` simulates the
exhaustion at the exact boundary the real one crosses: one
`pthread_create` returns `EAGAIN` (the resource-exhaustion errno) inside
a bounded window; every other call passes through untouched. Because the
injection is at the libc boundary, the PAL takes its genuine failure path
end to end (`pal/src/thread/pal_thread.cpp`: `pthread_create` error →
`ERROR_NOT_ENOUGH_MEMORY` → cleanup → `CreateThread` returns `NULL`) —
the same path real thread exhaustion takes. No engine or host source is
touched; the product binaries are byte-identical with or without the
shim (control legs below).

What is and is not exercised, honestly:

- Exercised: the real PAL `CreateThread` failure path on the exact spawn
  site, on the process's main thread, mid-script, with 5 live parked
  agents already in `threadData->children`; the post-fix error surface
  (catchable JS exception), state consistency (the next spawn succeeds)
  and the end-of-main join over the surviving children.
- Not exercised: a genuine kernel-level EAGAIN (the errno and the PAL
  path it drives are identical, but the kernel is not the one refusing);
  Windows `CreateThread` failure; debug-profile PAL internals beyond the
  shared failure path. Not deterministically testable in-gate: no gate
  can afford to push a process to its thread limit — hence no new rlexe
  entry, matching the host-exit-join precedent.

## Files

- `spawnfail.js` — repro. 5 agents spawn (phase 1), the 6th spawn is the
  one the driver refuses, a 7th (phase 2) proves the host state is
  consistent again post-fix; main then ends and the end-of-main join runs
  over every successfully started agent.
- `failthreads.c` — `DYLD_INSERT_LIBRARIES` shim (`__DATA,__interpose`
  section; see its header comment for why symbol shadowing and
  `dlsym(RTLD_NEXT)` both fail on modern dyld, with the measurements).
  Env knobs: `FAILTHREADS_AT=N` (deny creates strictly after N
  successes), `FAILTHREADS_DENY=D` (stop after D denials; default deny
  forever), `FAILTHREADS_LOG=1`.
- `drive.sh <ch-binary> [runs=10]` — builds the shim, runs the repro N
  times and tallies `caught / hangs / other` (class definitions in the
  header comment). Requires `cc` and coreutils `timeout`.

The budget used by the driver: 3 startup creates (measured, test and
debug profiles — stable across repeated runs; agents create exactly one
thread each, and neither profile showed lazy engine thread creation
during the churn window) + the 5 phase-1 agents.

## Pre-fix evidence (this repair's A leg)

Test-profile binary at base `4f564ea04`
(`zig-out/bin/ch`, SHA-256 `beea7257…32af`), driver
`drive.sh <bin> 12`:

| Leg | Result |
|---|---|
| `drive.sh` (12 runs, injected refusal) | **12 hang / 0 caught / 0 other** |
| Control: no shim (bare `ch -Test262 spawnfail.js`) | all spawns succeed, `ERROR: spawn unexpectedly succeeded`, exit 0 |
| Control: shim loaded but disarmed (no `FAILTHREADS_AT`) | identical to bare run, exit 0 |

A hung run prints `phase1=5` (per-line flush survives the kill) and
nothing else — it never returns from the 6th `$262.agent.start`; the
sampled stack above is from exactly this configuration.
