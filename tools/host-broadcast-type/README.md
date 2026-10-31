# host-broadcast-type repro tooling — `WScript.Broadcast(non-SharedArrayBuffer)` SIGSEGV

Evidence tooling for the follow-up card filed by the host-spawn-check
adversarial review (`doc/c2-s6-cross-agent-waitasync.md` → "Review
outcome (host-spawn-check)" → "New pre-existing defect found (card
filed)"). Nothing here is wired into the build or test gates; these are
evidence-collection tools. The committed in-gate coverage lives in
`test/es7/s6-broadcast-type.js`.

## Defect (pre-fix, `bin/ch/WScriptJsrt.cpp` `WScriptJsrt::BroadcastCallback`)

`WScript.Broadcast(<non-SharedArrayBuffer>)` with live agent children
crashed the process (100% repro, rc=139 SIGSEGV, no output). The card's
reproducer is `repro.js` verbatim; run it with:

    <ch> -nonative -ESBigInt -ESSharedArrayBuffer -Test262 repro.js

Mechanism, from source:

1. `BroadcastCallback` called
   `ChakraRTInterface::JsGetSharedArrayBufferContent(arguments[1],
   &threadData->sharedContent)` with the return value **unchecked**.
2. On a non-SAB argument the engine returns `JsErrorInvalidArgument`
   **without writing the out parameter** (`lib/Jsrt/Core/JsrtCore.cpp`,
   the `VarIs<SharedArrayBuffer>` check returns before the store), so
   `threadData->sharedContent` keeps its prior value: `nullptr`
   (`RuntimeThreadData` constructor) on the first broadcast, or a
   stale already-released handle after an earlier valid broadcast
   (the parent releases the handle after the all-children handshake).
3. The callback then signaled every child anyway
   (`SetEvent(child->hevntReceivedBroadcast)`).
4. A live, non-leaving child consumes the broadcast:
   `JsCreateSharedArrayBufferWithSharedContent(parent->sharedContent,
   ...)` (`bin/ch/RuntimeThreadData.cpp`), which dereferences the
   handle (`library->CreateSharedArrayBuffer((SharedContents*)...)`
   then `->MarkCrossRuntime()`) — null/stale pointer → SIGSEGV in the
   child, before the parent's `try/catch` can print anything.
5. Without live children nothing is signaled and nothing crashes; the
   call silently returned `undefined` — also wrong (an invalid argument
   should surface), but non-fatal.

`$262.agent.broadcast` is `WScript.Broadcast` (`bin/ch/262.js`), so both
script entry points share this callback and one fix covers both.

Review repair (leak): the validation extraction AddRefs on success, so
an agentless valid broadcast (`threadData == nullptr`) must release the
staged handle itself — the agents-present handshake's release never
runs. `leakprobe.js` + `leakdrive.sh` isolate this by peak RSS (needs
`-Test262`, else `WScript.Broadcast` is not installed); see the receipt
follow-up in `doc/c2-s6-cross-agent-waitasync.md` for the A/B numbers.

## Fix

Validate the broadcast argument and check the API return **before
signaling any child**: stage the content handle in a local, and only on
`JsNoError` commit it to `threadData->sharedContent` and proceed with
the signal/wait/release handshake. On failure surface the sibling
callbacks' bad-argument shape — `JsErrorInvalidArgument` with a message
through `SetExceptionIf` (a catchable script error, like every other
`WScript.*` callback's argument rejection) — leaving `sharedContent`
and every child untouched. Host-side only; no engine change.

## Usage

    sh tools/host-broadcast-type/drive.sh <ch-binary> [runs]

Classifications: `throw` (post-fix), `crash` (pre-fix), `other`.

## Evidence

Pre-fix A leg (base `78376e684` = main; binaries SHA-256: test
`1a1b6fdf…5c64`, debug `519ee87a…f3a8`), Mac ARM64:

| Leg | Binary | Runs | Result |
|---|---|---|---|
| Pre-fix test | base `78376e684` | 10 (+5) | **15/15 SIGSEGV** (rc=139), 0 threw, no output |
| Pre-fix debug | base `78376e684` | 10 (+5) | **15/15 SIGSEGV** (rc=139), 0 threw, no output |

Post-fix B leg, agents100 regression, focused S6e suite and gate
numbers: see the follow-up receipt section in
`doc/c2-s6-cross-agent-waitasync.md` (the recorded numbers there are
authoritative).
