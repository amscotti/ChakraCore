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

- `spawnfail.js` — repro. 5 agents spawn (phase 1), then spawns continue
  in a try/catch loop until one is refused (the driver denies exactly one
  create somewhere in that window; the loop absorbs the per-profile
  startup-count variance — 3 test / 2 debug), then one more (phase 2)
  proves the host state is consistent again post-fix; main then ends and
  the end-of-main join runs over every successfully started agent.
- `failthreads.c` — `DYLD_INSERT_LIBRARIES` shim (`__DATA,__interpose`
  section; see its header comment for why symbol shadowing and
  `dlsym(RTLD_NEXT)` both fail on modern dyld, with the measurements).
  Env knobs: `FAILTHREADS_AT=N` (deny creates strictly after N
  successes), `FAILTHREADS_DENY=D` (stop after D denials; default deny
  forever), `FAILTHREADS_LOG=1`.
- `drive.sh <ch-binary> [runs=10]` — builds the shim, runs the repro N
  times and tallies `caught / hangs / other` (class definitions in the
  header comment). Requires `cc` and coreutils `timeout`.

The budget used by the driver: `FAILTHREADS_AT=8` (deny the 9th create:
3 startup + 5 phase-1 agents on test, 2 + 5 + the first loop spawn on
debug) with `FAILTHREADS_DENY=1` (exactly one refusal, then creates
succeed again — the consistency probe needs one). Startup counts were
measured per profile (stable across repeated runs; agents create exactly
one thread each, and neither profile showed lazy engine thread creation
during the churn window).

## Pre-fix evidence (this repair's A leg)

Driver `drive.sh <bin> 12`; final `spawnfail.js` (loop version) for all
four legs. Binaries (SHA-256): pre-fix test `ae1a0b89…185`
(`zig-out/bin/ch`, base `4f564ea04`), pre-fix debug `2dfb264e…831`
(`out/debug-static/bin/ch`), post-fix test `44a8980e…783`, post-fix
debug `99d7ae86…bf5`. Each rebuild at the same source produces a fresh
link instance (Mach-O UUID), so identities are pinned per leg; each
pre-fix binary was string-verified to lack the fix's message and each
post-fix binary to contain it. Startup pthread_creates before the script
spawns agents: 3 on the test profile, 2 on debug (measured, stable).

| Leg | Binary | Runs | Result |
|---|---|---|---|
| Pre-fix test (base `4f564ea04`) | `ae1a0b89…185` | 12 | **12 hang / 0 caught / 0 other** |
| Pre-fix debug (base `4f564ea04`) | `2dfb264e…831` | 12 | **12 hang / 0 caught / 0 other** |
| Post-fix test (fix commit) | `44a8980e…783` | 12 | **12 caught / 0 hang / 0 other** |
| Post-fix debug (fix commit) | `99d7ae86…bf5` | 12 | **12 caught / 0 hang / 0 other** |

A hung pre-fix run prints `phase1=5` and nothing more (per-line flush
survives the kill) — it never returns from the refused
`$262.agent.start`; the sampled stack in the defect section above is
from exactly this configuration. A post-fix run prints
`phase1=5`, `caught=Failed to create the agent thread.`, `phase2=1` and
exits 0: the exception is catchable, the next spawn succeeds, and the
end-of-main join collects every successfully started agent.

Controls (base binary): no shim — all spawns succeed, `ERROR: no spawn
refused (no thread budget injected?)`, exit 0; shim loaded but disarmed
(no `FAILTHREADS_AT`) — identical to the bare run. Earlier preliminary
legs with the first script revision (fixed try-block spawn index)
reproduced the same 12/12 hangs on both pre-fix profiles.

Notes: the test-profile `ch` dlopens `libChakraCore.dylib` from its own
directory — when copying binaries for legs, copy the dylib beside them
or a copied launcher exits rc=5 (a loud `other`, never a silent
misclassification). `failthreads.c` counters are non-atomic; safe under
the driver's single-spawning-thread serialization assumption (noted in
the source).

Spawn-success path unchanged: `tools/host-exit-join/drive.sh` on the
post-fix binaries — agents100 test 20/20 clean, debug 10/10 clean.
Focused S6e suite (`runtests.py --test|--debug --variants=disable_jit`,
s6-waitasync-cross-agent, s6-waitasync-pause, s6-bigint-atomics,
s6-growable-nonshared, atomics_test, sharedarraybuffer, bigint64array):
**7/7 test, 7/7 debug**. Gates: `check_regression.py --select test
--expected-passed 2031` → **PASS 2031/0**; `--select debug
--expected-passed 2083` → **PASS 2083/0** (one first attempt measured
2082/1: `DebuggerCommon/globalFuncVars.js`, a debugger-baseline test
whose output gained a PAL `InternalSetThreadPriority` warning under
machine load — no agents, unrelated path; 5/5 clean in isolation and
0 failures in the logged full rerun). Pins in
`tools/build/gate_counts.json` untouched: thread-exhaustion is not
deterministically reachable in-gate, so no new rlexe entry (same
reasoning as the host-exit-join repro tooling).
