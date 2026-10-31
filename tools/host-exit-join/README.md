# host-exit-join repro tooling — end-of-main join over >64 agents

Evidence tooling for the S6e iteration-3 host-teardown defect recorded in
`doc/c2-s6-cross-agent-waitasync.md` ("Pre-existing exit assert under
very-high agent churn" residual → fixed by the host-exit-join repair; see the
receipt section there for A/B tables and gates). Nothing here is wired into
the build or test gates; these are evidence-collection tools.

Defect (pre-fix): `WScriptJsrt::Uninitialize`'s end-of-main join called
`WaitForMultipleObjects(count, ...)` once over every handle in
`threadData->children`. Children are never unlinked before that point
(`leaving()` only sets the child's own flag; the sole removal is the clear in
the same join block), so a process that starts more than
`MAXIMUM_WAIT_OBJECTS` (64) agents passes `nCount > 64`, which the PAL
rejects (`pal/src/synchmgr/wait.cpp`). Debug profiles assert at the `waitRet`
check; release/test profiles fall through to the delete loop and free
`RuntimeThreadData` objects without any thread having been observed exiting.

- `agents100.js` — deterministic repro. Starts 100 agents (50 report-then-
  `leaving()`, 50 just complete their initial script; both classes park in
  the child wait loop and remain in `children`), then lets main end. Run:
  `<ch> -Test262 tools/host-exit-join/agents100.js`. Want exit 0, stdout
  exactly `agents-started=100`, no `ASSERTION` lines. Pre-fix this asserts
  on debug profiles on every run.
- `drive.sh <ch-binary> [runs=50]` — runs the repro N times and tallies
  `clean / asserts / signals / other` (see header comment for classes).
  Used for the pre/post A/B legs on the test and debug-static profiles.

Not added as an rlexe entry: 100 agent-runtime creations exceed what the
debug regression gate can afford reliably; the gates' pins stayed
2031/2083 through this repair (see the receipt in
`doc/c2-s6-cross-agent-waitasync.md`). They were later bumped to
2032/2084 by the host-broadcast-type lane's rlexe entry, unrelated to
this tooling.
