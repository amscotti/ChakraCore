# host-map-sync repro tooling — R1 host global-map synchronization

Evidence tooling for the swarm-triage R1 escalation (concurrency lens;
receipt `doc/e13-swarm-triage.md`): the host's process-global module/script
maps (`moduleRecordMap`, `moduleDirMap`, `moduleErrMap`, `scriptDirMap`) and
the `sourceContext` counter in `bin/ch/WScriptJsrt.cpp` are written from
crossthread agent threads concurrently with the main thread — S6e made
100-agent processes supported, so this is reachable, not theoretical.
Nothing here is wired into the build or test gates; these are
evidence-collection tools (same convention as `tools/host-exit-join/`).

Defects (pre-fix, both landed in the R1 commit):

1. **Unsynchronized shared mutation.** Every map access was a plain
   `std::map` read/write from whichever thread reached it:
   `GetNextSourceContext()`'s `sourceContext++`, agent-thread
   `LoadScript("…", "self"/"module")` inserts into `scriptDirMap` /
   `moduleRecordMap` / `moduleErrMap`, fetch-callback find/insert, and the
   main thread's own loads. Concurrent inserts corrupt the map nodes
   (heap corruption, debug-profile asserts). Fix: one host-globals
   `CRITICAL_SECTION` (the file's existing lock idiom, matching
   `RuntimeThreadData::csReportQ`) around every map read/write/clear
   site — including the teardown `clear()`s, which already run
   main-thread-after-join — plus an interlocked increment for the
   counter. The lock is never held across `JsParseModuleSource` /
   `JsModuleEvaluation` (the engine re-enters the fetch callbacks from
   inside those), so no recursive-lock dependence.
2. **Module fetch messages left the agent thread.** `PushMessage`
   inserted module fetch/eval messages into the MAIN-thread static queue
   unconditionally, so a dynamic import issued inside an agent raced the
   main loop's queue and its record was parsed/evaluated on the wrong
   thread — the agent's import promise silently never settled. Fix:
   route through `GetThreadMessageQueue()` (main queue on the main
   thread, the agent's own queue on an agent thread; the S6e per-thread
   queue design). Observed pre-fix with a 2-agent probe:
   `getReport()` returns `null` forever after `import("./dep.js")`
   inside an agent; post-fix it reports `import-ok:42`.

- `agents-loadsync.js` — the repro. Starts N agents (default 40; pass via
  `-args N -endargs`), each doing a string-module `LoadScript`, a
  `LoadScript("…", "self")` whose loaded script dynamically imports a
  per-agent module file, and `report("ok"/"err")` from the import
  continuations; the main thread concurrently fires its own dynamic
  import (`dep-main.js`, committed here) while spawning. Must run with
  CWD in a directory holding `dep-0.js … dep-(N-1).js` (`export const v =
  <i>;`) — agents resolve relative specifiers against CWD because agent
  sources carry no script directory.
- `drive.sh <ch-binary> [runs=10] [agents=40]` — builds those fixtures in
  a scratch workdir, runs the repro N times, and tallies
  `clean / asserts / signals / other` (see header comment for classes).
  Want: exit 0 with `clean=runs`, i.e. every run exits 0 and prints
  exactly `agents=N started=N importOk=N importErr=0`.

Not added as an rlexe/gate entry: N agent-runtime creations plus module
loads exceed what the debug regression gate can afford reliably (same
verdict as `tools/host-exit-join/`); the A/B evidence lives in
`doc/e13-swarm-triage.md`.
