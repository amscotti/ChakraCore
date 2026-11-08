# e13 — swarm-triage disposition lane (final quality gate)

Date: 2026-11-08 · Branch: `modernize/baseline` · Base: `301172d38`
(gates 2051 test / 2103 debug) · Lane commits: `a9ab2bb18` (R1),
`5427e858e` (mint-path), `74e9ec31d` (dylib digest), `1b1bfdd6e`
(nit batch), plus this receipt/cards commit.

Five-lens swarm review of the accumulated modernization tree
(concurrency, architecture, memory, compatibility, tests/docs).
**All five lenses ACCEPTed with zero `[bug]` findings.** This lane
landed the four dispositions the triage ordered — one real
concurrency fix, the must-fix-before-Linux mint-path fix, one tooling
fold, one mechanical nit batch — and carded everything else.

## 1. Verdicts and dispositions

| Lens | Verdict | Escalation | Disposition |
|---|---|---|---|
| concurrency | ACCEPT | R1: host global maps unsynchronized (below) | **landed** (`a9ab2bb18`) |
| architecture | ACCEPT | mint path writes untracked `Bytecode/` on Linux | **landed** (`5427e858e`, must-fix before B4) |
| memory | ACCEPT | BigInt count-reassign wart; reparse-guard⇒WB coupling | **noted in source** (nit batch; release-safe / by-design) |
| compatibility | ACCEPT | JSERR `NeedArrayBufferObject` message reuse | **carded** |
| tests/docs | ACCEPT | dylib digest absent from run summaries; receipt inaccuracies | digest **landed** (`74e9ec31d`); receipts **corrected** (nit batch) |

## 2. R1 — host global-map synchronization (`a9ab2bb18`)

The four process-global maps in `bin/ch/WScriptJsrt.cpp`
(`moduleRecordMap`, `moduleDirMap`, `moduleErrMap`, `scriptDirMap`) and
the `sourceContext` counter were mutated from crossthread agent threads
unsynchronized — reachable since S6e made 100-agent processes
supported. Fix:

- one host-globals `CRITICAL_SECTION` — the file's existing lock idiom,
  matching `RuntimeThreadData::csReportQ` — guarding **every** map
  read/write/clear site (find sites copy the value out under the lock);
  the `Uninitialize` teardown `clear()`s (already main-thread-after-join)
  take it too, for uniformity;
- `GetNextSourceContext()` increments interlocked
  (`InterlockedIncrement64`/`InterlockedIncrement` by pointer width),
  preserving post-increment semantics;
- the lock is created on first use (thread-safe local static, heap and
  deliberately leaked — no global-destructor-order dependence) with an
  RAII guard, and is **never held across `JsParseModuleSource` /
  `JsModuleEvaluation`**, which re-enter the host fetch callbacks: no
  recursive-lock reliance, and `JsInitializeModuleRecord` (held under the
  lock for find+create+register atomicity) calls no host callbacks;
- one consequential host repair the escalation exposed:
  `PushMessage` inserted module fetch/eval messages into the MAIN static
  queue from every thread, so a dynamic import inside an agent raced the
  main loop's queue and settled on the wrong thread — pre-fix probe
  (2 agents): `getReport()` returns `null` forever after
  `import("./dep.js")`; post-fix: `import-ok:42`. Now routed through
  `GetThreadMessageQueue()` per the S6e per-thread queue design.

Ownership/allocation discipline unchanged: the lock guards existing
`std::map`/counter state only; no JsRT handle is retained by it and no
new GC-visible state is introduced.

**Evidence** (`tools/host-map-sync/`, e1 convention):
`agents-loadsync.js` — N agents × {string-module `LoadScript`,
`LoadScript("…", "self")` whose loaded script dynamically imports a
per-agent module} + a concurrent main-thread dynamic import; every map
and the counter are hit cross-thread. `drive.sh` classifies
clean/assert/signal/other and requires the exact summary
`agents=N started=N importOk=N importErr=0`.

- post-fix, test profile: **10/10 clean** (agents=40)
  (`bin=zig-out/bin/ch runs=10 agents=40 clean=10 asserts=0 signals=0 other=0`);
- post-fix, debug-static profile: **3/3 clean** (asserts compiled in);
- existing agents100-style repro re-run: `tools/host-exit-join/drive.sh`
  **10/10 clean** (test) and **3/3 clean** (debug-static);
- not gate-expressible (N agent-runtime creations + module loads exceed
  the debug gate's budget — same verdict as host-exit-join), so the
  committed repro + this A/B record is the receipt evidence.

Focused: S6e + es6module rlexe **36/0** (disable_jit); jit-variant
re-run on `out/jit/bin/ch` (es6module + S6 files) **Success**.

## 3. Mint-path casing (`5427e858e`) — must-fix before Linux

`tools/regenByteCode.py` minted the GUID header via lowercase
`lib/Runtime/Bytecode/` while git tracks `lib/Runtime/ByteCode/`: the
same file on case-insensitive macOS, an **untracked file + stale
tracked header** on case-sensitive Linux. Path constant fixed to the
tracked casing and pinned by `RegenGuidPathTests` (2 tests): a
functional dual-variant fixture mint (the tracked-path file is
overwritten in place; no lowercase sibling directory entry — catches
the split on case-sensitive filesystems) and an exact-cased constant +
`git ls-files` check (catches it everywhere). `check-build-tools`
**65 → 67 tests OK**.

## 4. Dylib-digest recording (`74e9ec31d`)

On the shared profiles the launcher and the engine are separate files
(~0.8 MB `ch` + ~23 MB `libChakraCore.dylib` on Mac), so
`binary_sha256` pinned only the launcher — an engine-side rebuild left a
run's primary identity unchanged. `tools/test262/run.py` now records
`engine_dylib_sha256` (dylib resolved beside the binary, first by name;
`None` on a static build; `shared_libraries` still itemizes all)
alongside `binary_sha256`; `refresh_baseline.py` prints it in the
report's run-identity section and carries it into register entries
(`.get`, so pre-recording runs still report).

Validation (small run: `test/built-ins/Array/from/*`, controls 25/25):
`engine_dylib_sha256 = ab8efd64…9326` equals the independently
recomputed dylib digest while `binary_sha256 = b8a8db8f…b2d4` stays the
launcher's; `refresh_baseline --skip-run` renders both lines; a
field-less legacy summary renders "none recorded" and registers
`engine_dylib_sha256: null`.

## 5. Nit batch (`1b1bfdd6e`) — mechanical

- `WScriptJsrt.cpp`: 3 double-blank-line introductions removed;
  `Uninitialize`'s children-list comment corrected (the failed-spawn
  path also `pop_back`s, the teardown `clear()` is the sole removal on
  the normal path); `"json-module-parse"` now uses the file's `strlen`
  idiom.
- Receipt corrections:
  - `doc/c1-negative-phase.md` (§2 spoofing defense, §5 residuals 6–7)
    and `run.py`'s `marker_evidence` docstring: "agents are separate
    processes whose stderr is not this record's" / "agents fail
    silently in separate processes" were wrong for S6e crossthread
    agents (threads sharing this process's stderr). Real bound:
    first-wins judging + the line-1 tripwire (+ declared-phase equality
    downgrading loudly); emit-once is per-thread, so a duplicate marker
    line remains possible — residual stands, justification corrected.
  - `doc/c2-s6-cross-agent-waitasync.md` host-exit-join residual (T4):
    "all `Error` exits precede `JsRun`" is wrong — `ThreadProc`'s
    wait-loop `break` (`Assert` arm) reaches the `Error` label after
    `JsRun` and skips the sub-agent join; practically unreachable
    (`WaitForMultipleObjects` over the two lifetime-valid handles
    cannot return `WAIT_FAILED` there).
  - `doc/e11-tco-timeouts.md` §2.B2/§4: one-line pointers to the
    corrected figures — the −2 host→disabled-flag reclassification is
    impossible under the bucket rules (c4-refresh: host 7→3, not 7→1;
    disabled-flag 1,734, not 1,736).
- Memory-lens notes: one-line comment on the inherited
  count-reassign-before-`HeapDeleteArray` wart in
  `JavascriptBigInt.cpp` (release-safe: the free is by pointer, only
  the debug fill is under-sized); shared comment in
  `CommonDefines.h` pinning the reparse-guard ⇒ write-barrier macro
  coupling (the guard's frame-count release runs from
  `JavascriptGenerator::Finalize`, compiled only under
  `GLOBAL_ENABLE_WRITE_BARRIER`).

## 6. Cards filed

Four new disposition cards in [open-cards.md](open-cards.md) §6: T3
broadcast-5s-overrun wrong-content pairing (bounded, warned,
memory-safe); WScriptJsrt.cpp split before further Linux/host
extension; JSERR dedicated-message follow-up
(`NeedArrayBufferObject` reuse); regen-guard create-on-refuse edge.
The b5-guid card's lowercase-path residue is marked fixed (e13).

## 7. Gates and verification (this lane's tree)

- `zig build check` **rc=0** — regression **2051/0**, bytecode verify +
  pure-verify rc 0, native (`test-shared-basic`) PASS, smoke PASS,
  metadata PASS, self-tests PASS.
- `zig build -Dprofile=debug-static check-regression` **rc=0 —
  2103/0**.
- `zig build -Dprofile=jit check` **rc=0** — check_jit PASS
  (NativeCodeGenerator symbol + hot-call/rest/apply probes), bytecode
  up to date, verify pure rc 0.
- `check-build-tools` **67 tests OK** (mint-path tests included).
- R1 repros and gates-exempt legs: §2 above. `git diff --check` clean.
- Gate pins (`tools/build/gate_counts.json`) untouched: 2051/2103
  (host/tooling/docs only; no new rlexe entries).
