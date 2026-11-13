# Node-API worktree — kickoff brief

Read this first if you are the orchestrator or a sub-agent starting
cold in this worktree. The full plan is
[node-api-plan.md](node-api-plan.md); this brief is the operational
layer on top of it.

## Where you are

- Worktree: this directory. Branch: **`node-api`**, created from
  `modernize/baseline` at `48b7db6d8` (WASM ARM64 tranche 5).
- The main checkout (`~/Documents/code/ChakraCore`) is on
  `modernize/baseline` with other agents actively working. **Do not
  work there. Do not commit to `modernize/baseline` from here.**
- This branch lives until the Node-API provider is done, then merges
  back. Expect to rebase (see below).

## Mission (one paragraph)

Implement the Node-API (N-API) v8 **engine provider** for ChakraCore
per `doc/node-api-plan.md`: a `libChakraNodeApi` library exporting the
`napi_*` C ABI on top of JsRT, plus a private host API and a test
host, so that Node-API addons can run on this engine. We are **not**
building a Node.js runtime (plan §0). Success is defined in plan
§0.3 — do not overclaim beyond it.

## First actions for the orchestrator (in order)

1. **Record the PR decisions.** Plan §2.2 lists PR-1…PR-12 with a
   recommendation for each. Accept the recommendations or amend them,
   and edit the §2.2 table to record the outcome + date. No task with
   undecided blocking PRs may start (plan §2.2, §9).
2. **Dispatch Wave 0** (plan §6):
   - Parallel: **N0.1** (vendor headers + inventory; needs PR-2),
     **N0.2** (Zig/C++ language spike; decides PR-4), **N0.4**
     (semantics spec).
   - Then: **N0.3** (host API + test host; needs PR-3/PR-5),
     **N0.5** (test assets; needs PR-11), **N0.6** (status mapping;
     needs N0.4 draft).
   - Wave 0 exits at gate **G0** (plan §5).
3. Give each sub-agent exactly one task card from plan §6 plus plan
   §0–§3. Require the report format in plan §9.

## Swarm worktree convention

- One worktree per lane/task when parallel work would collide:
  create with the opencode worktree tool, name pattern
  `nodeapi-<taskid>` (e.g. `nodeapi-n01`, `nodeapi-n24`), branched
  from this `node-api` branch — never from `modernize/baseline`.
- Lane branches merge back into `node-api` here (merge or rebase per
  the orchestrator's call; keep lane commits small and gated).
- File ownership is in plan §5.2. The `NodeEnv` layout (plan §3.3) is
  owned by task N1.1; other tasks propose changes in their reports
  instead of editing it concurrently.

## Rebasing onto the moving main branch

`modernize/baseline` will move while this work runs. Periodically
(and before any merge-back):

```sh
git fetch origin
git rebase origin/modernize/baseline   # from this node-api branch
```

Note: the main checkout may be ahead of `origin` at times (unpushed
work by the other swarm). If `origin/modernize/baseline` looks stale,
ask the owner before rebasing — do not rebase onto a guess. Expect
conflicts mainly in `doc/`; **never** resolve a conflict by moving
the regression pins in `tools/build/gate_counts.json` (Node-API work
must not touch them; provider tests use their own build step).

## Build and test discipline (machine-global)

- **Serialize all builds/gates** through the shared gate lock, which
  is global across every worktree on this machine (the main-branch
  swarm uses it too):
  `sh /tmp/opencode/cc-gate-lock.sh mise exec -- zig build ...`
  If the lock script is missing, recreate it per the header comment
  convention used previously — do not run unlocked parallel builds.
- Toolchain: `mise exec -- zig ...` only (pinned Zig 0.17.0). Never a
  system Zig.
- Each worktree has its own `.zig-cache`: the first build in a fresh
  lane worktree is slow. Budget for it; do not mistake it for a hang.
- Generated files (addon builds, test outputs) go to temp/build dirs,
  never into the source tree.

## Hard rules (from the plan; violations fail review)

- D6: shared-code edits (outside the new provider/test dirs) must be
  neutral for existing platforms or gated, and say which.
- D7: unsupported/host-less functions return `napi_generic_failure`
  with a last-error message. Missing symbols or fake success are
  defects.
- D8: no string `eval` workarounds for missing engine features; the
  approved intrinsic-call paths in plan §4.3 are the only exception.
- Any task that discovers it needs a new JsRT export stops and files
  the gap (plan §4.3 pattern) instead of hacking around it.
- Tasks marked "ownership record required" must include it
  (doc/memory-management.md §3) in their report.

## State at handoff

- Plan v2 (audited, 26 task cards, waves 0–5, gates G0–G5):
  `doc/node-api-plan.md` — committed on this branch.
- Implementation: **G0–G5 complete on macOS arm64** (2026-11-13).
  Provider `libChakraNodeApi` committed (f50b390d3) + conformance/
  probes/receipts (b18392821). js-native-api 43/26/0, gate 2053/0,
  probes 3117/profile, stress ALL PASS. Linux x64 WIRED
  (`tools/build/linux.zig` provider/host/check-nodeapi steps,
  platform-branched runners), NOT RUN — no native execution yet
  (testing deferred to the Linux host; not claimed).
- PR-1…PR-12 closed as recorded in plan §2.2 (C++ through v1 per
  PR-4; Zig re-evaluation only after G5 with fresh spike).
- **PR-4 decision 2026-11-11 (plan §2.1 D5): C++ through provider v1
  if G5 passes; Zig re-evaluation after G5.** Wave 1 provider is C++17
  (ABI-identical to Zig); do not port before G5 without a new decision
  record.
