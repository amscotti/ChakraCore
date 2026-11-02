# dbg-async-attach: suspended async resume after dynamic attach

Lane: `dbg-async-attach` (implementer subagent), base `710af1307`.
Defect: the second open engine issue in
[arm64-macos-jit.md](arm64-macos-jit.md) "Known open engine issues" —
*Suspended async-function resume after dynamic debugger attach aborts*.

Status: **fixed on this lane** — diagnosis (§1-2), ownership record (§3),
design (§4) and verification (§5) below; commits `0fc0dfbfd` (diagnosis),
`26055a0c2` (fix + test), and this receipt. The
[arm64-macos-jit.md](arm64-macos-jit.md) known-issues bullet is closed.

## 1. Reproduction

Minimal repro (the doc's), committed as
`test/DebuggerCommon/AsyncSuspendedResume.js` with a second-invasion
variant:

```js
var resolveIt;
var p = new Promise(function (r) { resolveIt = r; });
async function f() { print("before"); var v = await p; print("after " + v); }
f();
WScript.Attach(function () { resolveIt(41); });
```

`WScript.Attach` queues a host message (`bin/ch/WScriptJsrt.cpp`
`AttachCallback` → `QueueDebugOperation`). When the host message loop
services it, the handler runs `Debugger::StartDebugging` +
`SourceRunDown` and only then calls the callback — so at reparse time
`f` is guaranteed suspended at the `await` on the still-unresolved `p`,
and the resume is guaranteed to happen after the reparse.

Debug-static binary (`out/debug-static/bin/ch`, base `710af1307`):
**20/20 runs** exit 133 (SIGTRAP on a debug assert) with

```
before
done
after 41
ASSERTION 73841: (./lib/Runtime/Library/JavascriptAsyncFunction.cpp, line 192)
    JavascriptOperators::GetTypeId(result) == TypeIds_AwaitObject
```

The doc's harsher variant (`SIGABRT`, `FatalInternalError` in
`AsyncSpawnStep`, "resume receives a non-iterator value") is the same
defect under a collected/reused old bytecode buffer (§2); in this
no-GC-pressure repro the buffer survives and the failure mode is the
benign one.

## 2. Root cause: abort site and invalidation chain

LLDB on the failing `AsyncSpawnStep` call (breakpoint at
`JavascriptAsyncFunction.cpp:192`, second hit):

- `result` is `undefined` (`TypeIds_Undefined`), and `generator` is the
  same `JavascriptGenerator` as the initial run-to-await call.
- The frame's `ByteCodeReader`: `m_startLocation = 0x10d338000`,
  `m_currentLocation = 0x10d338059`, `m_endLocation = 0x10d33805a` —
  the reader consumed the **original** bytecode block to its final
  offset 89 of length 90.
- The body's *current* `byteCodeBlock` is a **different** allocation
  (`0x10d279300`) with `m_contentSize = 92`.

Chain:

1. `f()` runs to `await p`; the `InterpreterStackFrame` is stored on the
   `JavascriptGenerator` with `m_reader` pointing into `f`'s original
   `ByteBlock` (`FunctionBody::byteCodeBlock`, recycler memory), saved
   offset, cached `inlineCaches` pointer
   (`InterpreterStackFrame::Setup::InitializeAllocation` copies
   `executeFunction`'s IC array pointer and constant slots into the
   frame — constants are frame-local thereafter).
2. Attach: `ScriptContext::OnDebuggerAttachedDetached` calls
   `SetEntryToDeferParseForDebugger` on every body (re-defer + entry at
   `DefaultDeferredParsingThunk`), then
   `DebugContext::RundownSourcesAndReparse` calls
   `ParseableFunctionInfo::Parse` per top-level function info.
3. `Parse` (reparse branch) calls `funcBody->CleanupToReparse()`, which
   drops `byteCodeBlock`, const table, aux data, statement maps, ICs
   (unregistered from the thread-context invalidation lists and returned
   to the arena) — then codegen repopulates the **same FunctionBody
   object** with fresh debug-mode bytecode (here: length 92 ≠ 90 at a
   different address). The suspended frame's raw references now describe
   state that no longer exists:
   - **lifetime**: the original `ByteBlock` is unreferenced → collectable;
     once collected/reused the reader interprets arbitrary bytes (the
     doc's bare-value/`FatalInternalError` abort in
     `AsyncSpawnStep`'s `VarTo<RecyclableObject>`), and the frame's
     cached IC pointers point at recycled arena memory;
   - **consistency**: even with the old buffer still mapped (this repro),
     the body's replaced metadata no longer describes the code the frame
     executes. `GeneratorStateHelper::IsDone`
     (`JavascriptGenerator.cpp:62-66`) compares the frame's reader
     offset (old-buffer offset 89) against
     `frame->GetFunctionBody()->GetByteCode()->GetLength()-1` — now the
     *new* bytecode's 91. Mismatch → the completed generator is left
     `Suspended`, `CallGenerator` returns the raw return value
     (`undefined`) instead of an iterator-result object, and
     `AsyncSpawnStep` takes the not-completed else-branch → assert at
     line 192 (debug) / undefined treated as an `AwaitObject` (release).

So the defect is not only a UAF: the suspended frame must keep a
**self-consistent** body (reader buffer + lengths + aux + ICs + register
layout), which an in-place reparse of the same `FunctionBody` cannot
provide. The generator-only reparse guard sidesteps this by refusing to
reparse a body with live generator frames — but for plain async bodies
that would skip installing debug bytecode, losing the async
locals/stepping baseline (`AsyncDynamicAttach.js`: a *new* invocation of
an async function must hit breakpoints/`locals()` while an instance of
the same function is mid-flight).

Upstream context: CVE-2019-0923 (`972584709`) made coroutine
inner-scope numbering and the yield register stable across debug
reparse and added a fail-fast asserting the yield register is unchanged
(`DebugContext.cpp` `yieldFunctions`) — i.e. upstream's model is that
suspended coroutine frames resume *on reparsed* bodies. That guarantee
covers only the yield register/inner-scope layout; bytecode offsets and
lengths do drift (90 → 92 here), which is what breaks completion
detection, and the dropped-buffer lifetime hazard is exactly what the
port's generator guard exists for. A plain async body therefore needs
its *original* body kept whole for its suspended frames, plus a
*separate* debug body for the debugger/new calls.

## 3. Ownership record (per doc/memory-management.md)

| Resource | Domain / allocator | Owner; invalidation | Notes for this lane |
|---|---|---|---|
| `FunctionBody` (old, suspended-frame body) | Recycler (`RecyclerNewWithBarrierFinalized`), finalizable | Normally: `FunctionInfo`→proxy chain; after the fix: each live `JavascriptGenerator`'s `liveCoroutineFunctionBody` `Field` | Kept whole (bytecode, aux, ICs, statement maps, profile info, entry points) so the frame's raw reader/IC pointers stay valid and self-consistent. Released at generator completion or `Finalize`; collected body runs the normal `FunctionBody::Finalize`→`Cleanup` (IC arena return + invalidation-list removal — the established collected-body path, same as `Redefer`'s replaced bodies). |
| Fresh debug `FunctionBody` | Recycler, finalizable | `FunctionInfo`→proxy swap (the `UpdateFunctionBodyImpl`/first-parse pattern) + `Utf8SourceInfo::SetFunctionBody` registration (replaces the old body's dictionary entry; `SetFunctionBody` is explicitly designed for id→body replacement) | Receives the debug reparse's bytecode; all ScriptFunctions resolve to it via the proxy. |
| `ByteBlock`s (old + new) | Recycler (`RecyclerNewArrayLeaf` contents) | Owning `FunctionBody` | Two blocks alive during the overlap; old freed by GC when the last live generator releases the old body. |
| Frame `inlineCaches` pointer | Recycler array of arena-allocated `InlineCache` | Old body's `inlineCaches` (kept un-cleaned while live) | Old body's ICs stay registered in the thread-context invalidation lists and are returned by its `Finalize`→`CleanUpInlineCaches` — no early cleanup, no dangling registration. |
| Live-frame count (`liveCoroutineFrameCount`) | Plain `uint` on `FunctionBody` | `JavascriptGenerator::SetFrame` / completion / `Finalize` | Extended from generators to all coroutines; still a count, not a GC reference. Script thread + GC-finalizer thread as in the generator guard (single decrement, no allocation). |

Threads/contexts: everything happens on the script thread during the
attach reparse walk (no concurrent JS); the only GC-thread touchpoint is
the pre-existing `RemoveLiveCoroutineFrame` in `Finalize`. No new
cross-context publication; the fresh body belongs to the same
scriptContext/sourceInfo.

## 4. Fix

Direction (b) from the issue text — *a separate debug FunctionBody for
the suspended case* — inverted: keep the **original** body whole as the
suspended frames' execution body, and give the **debug reparse** a fresh
`FunctionBody`:

- `JavascriptGenerator::SetFrame` counts/retains the body for **all**
  coroutine frames (generators already did; plain async frames now do
  too) — the count is what tells the reparse path a body is mid-flight.
- `ParseableFunctionInfo::CleanupToReparse`'s pin branch is restricted
  to `IsGenerator()` bodies (unchanged behavior for sync/async
  generators).
- `ParseableFunctionInfo::Parse` (debug-reparse branch): when the body
  is a plain async (`IsAsync() && !IsGenerator()`), not the global
  function, has bytecode, and has live coroutine frames, allocate
  `FunctionBody::NewFromParseableFunctionInfo(this)` and retarget the
  `FunctionInfo` at it (`SetFunctionProxy`/`SetFunctionInfo`/attribute
  clear — the `UpdateFunctionBodyImpl` recipe) **without** cleaning the
  original body; the reparse/codegen below then populates the fresh
  body, which registers itself in `Utf8SourceInfo` (replacing the old
  entry). Debugger registration (`RegisterFunction` after `Parse`) and
  entry-point updates (`UpdateEntryPointsOnDebugReparse`,
  `UpdateRecyclerFunctionEntryPointsForDebugger`) all see the fresh
  debug body; new invocations and breakpoints/locals/stepping work on
  it.
- The resume path reads the **frame's** body where frame layout is
  involved: the coroutine resume prologue writes the resume value to
  `newInstance->GetFunctionBody()->GetYieldRegister()` (was
  `executeFunction`'s; identical in the normal case, correct after a
  fresh-body swap), and `JavascriptGenerator::CallGenerator`'s
  inner-scope re-materialization uses the frame's body. Upstream's
  yield-register/inner-scope stability (CVE-2019-0923 fail-fast) makes
  these cross-body-safe, but the frame's body is the authority for the
  frame's layout, so the reads are frame-local now.
- Completion detection fixes itself: `IsDone` compares the old reader
  offset against the **old** body's length (both retained together), so
  the generator completes, `CallGenerator` returns a proper iterator
  result, and `AsyncSpawnStep` resolves the promise.

Why not the alternatives:

- *Pin plain async bodies like generators* (extend the existing guard
  unchanged): loses the async debugger locals/stepping baseline — no
  debug bytecode is installed for the function while any instance is
  suspended, so `AsyncDynamicAttach.js`-style `bp:locals()` on a new
  call of the same function stops working. Explicitly forbidden by the
  lane constraints.
- *Retain the old bytecode block and keep reparsing in place*: the frame
  reads more than the bytecode buffer through `m_functionBody` — aux
  data (`m_reader.ReadAuxArray(..., GetFunctionBody())`), statement
  maps, and (via the frame's cached pointer) the IC array; and the
  in-place reparse *replaces* those with new-layout contents while the
  frame's saved offsets still address the old layout. Retaining every
  replaced field side-by-side on one body (design (a) "retain/rebase")
  amounts to a second body's worth of state with aliasing hazards; a
  distinct `FunctionBody` object gets the same guarantee by
  construction, using existing body-replacement machinery
  (`NewFromParseableFunctionInfo`, `SetFunctionBody`, `Redefer`).
- *Rebase the frame onto the debug bytecode*: debug-reparse bytecode is
  not offset-compatible (length 90 → 92 measured), so a rebased reader
  would misexecute; there is no offset-mapping table.

## 5. Verification

| Check | Binary / command | Result |
|---|---|---|
| Doc repro (`AsyncSuspendedResume.js` shape, one await) | `out/debug-static/bin/ch` (debug-static, asserts) | 25/25 exit 0, `before/done/after 41` (base `710af1307`: 20/20 abort, exit 133) |
| Same | `zig-out/bin/ch` (test profile, release) | 25/25 exit 0 |
| Same | `out/jit/bin/ch` (JIT profile) | 20/20 exit 0 |
| Committed test (adds a second invocation on the fresh debug body while the first frame is suspended) | all three profiles | 20/20 each, exact baseline order `before/done/before/after 41/after 41` |
| Nested (closure) plain async suspended at attach (scratch) | debug-static | exit 0, both frames complete correctly |
| Re-attach while a post-attach invocation is suspended (scratch) | debug-static | exit 0, both frames complete correctly |
| Generator guard still intact: yield/attach/resume | debug-static `-nonative -off:DynamicProfile` and `-ForceSoftwareWriteBarrier-` | iterator results `{"value":2,"done":false}`, `{"done":true}`, exit 0 |
| Async locals/stepping baseline | `DebuggerCommon/AsyncDynamicAttach.js` on all three profiles | `PASSED` (its `bp:locals()` dump fires on a new invocation) |
| Debugger + async suites | `runtests.py --debug ... test/Debugger test/DebuggerCommon test/es7 test/es6` | **567/0** |
| Same | `runtests.py --test ...` (release) | **562/0** |
| Same + `es6GeneratorJit` | `runtests.py --test` on the **JIT** binary | **406/0** and **479/0** (both variant groups) |
| Gate: no-JIT test profile | `zig build check-regression` | **2037/0** (pin bumped 2036→2037) |
| Gate: debug-static | `zig build check-regression -Dprofile=debug-static` | **2089/0** (pin bumped 2088→2089) |
| JIT-profile focused diff vs base | `runtests.py --test` on jit binary, `test/es7` | failing set identical to the base build (`matchall`, `s6-growable-nonshared`, `bigint64array`, `resizable-typedarray` — pre-existing default-variant failures); one `class-private-methods.js` blip did not reproduce in 5 direct runs + 2 full dir runs (load flake) |

Pin decision: one new rlexe entry (`test/DebuggerCommon/AsyncSuspendedResume.js`,
`exclude_dynapogo`, like its attach-test sibling) appears in both gate
selections → **2036→2037 test / 2088→2089 debug**, lineage recorded in
`tools/build/gate_counts.json`.

Coverage of the changed failure/reentrancy paths actually exercised:
pending-await resume across reparse (the defect), fresh-body execution of
a second invocation, nested-closure async bodies, generator pin
(non-regression), attach with an async-generator (via the Debugger suite's
async-generator tests and `DebuggerCommon`), and detach/attach ordering via
the suite's attach/detach tests. Not exercised: OOM during the fresh-body
reparse (rollback path is analysis-only), TTD replay under dynamic attach.

## 6. Residuals

- Non-Apple targets: `ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD` is 0, so
  plain async (and generator) suspended bodies keep the original
  in-place reparse lifetime problem there, as before this lane.
- Breakpoints/stepping *inside the already-suspended frame* remain
  unavailable until it completes (the original body has no debug probes)
  — same tradeoff as the generator guard, scoped to the live frame; the
  function itself is fully debuggable via the fresh body from the moment
  the attach completes.
- Bailout-only paths (`BailOut.cpp` reads the yield register via the
  script function's proxy body) were left as-is: post-attach execution is
  interpretation-only, and the coroutine yield-register stability
  (CVE-2019-0923 fail-fast) covers them by construction; they are not
  reachable in the attach scenario this lane fixes.
- OOM/SOE during the fresh-body reparse rolls the proxy back to the
  original body (AutoRestoreFunctionInfo; its end-of-scope assert now
  accepts a whole, bytecode-bearing FunctionBody as a rollback target).
  The original body's entry points may then still be the re-defer thunks
  from the attach pre-pass, so a failure that late remains
  fatal-by-policy (`DEBUGGER_ATTACHDETACH_FATAL_ERROR_IF_FAILED`), as
  before. This rollback path is analysis-only (not covered by a test).
- TTD stance: unchanged — TTD forces debug mode from the start; dynamic
  attach mid-TTD is out of scope for this lane, same as the generator
  guard.
- The retired body (and its bytecode/ICs) stays alive until the last
  suspended frame completes or its generator is collected — the same
  bounded retention the generator pin introduces; its inline caches stay
  registered until the body's normal `Finalize`→`Cleanup`.

### Review outcome (dual ACCEPT, nits folded; new pre-existing defect filed)

Both reviewers ACCEPTed. The general review verified the invalidation
chain and both failure layers from source, every frame-body read site,
the identity-takeover mechanism (deferred-parse promotion; the
CVE-2019-0923 fail-fast now checks the FRESH body — cross-body
stability), ownership/rooting (plain-async spawns ARE
JavascriptGenerators retained by promise-reaction closures;
`liveCoroutineFunctionBody` rooted; RecyclerNewFinalized release
exactly-once), and reproduced all evidence (repro 34/34 across
profiles, DebuggerCommon 210/0, gates 2037/0 + 2089/0). The adversarial
review passed multi-await/re-attach/generator-mix/closure/TLA/eval/
`new Function`/deferred shapes and deep-after-await EH shapes; verified
the debugger boundary behaves exactly as disclosed (breakpoint fires
once, on the fresh-body invocation; the suspended frame silently
misses it); JIT-profile es7 failures empirically identical to base.

Folded nits: (1) TLA/global-body asymmetry — `Parse` excludes
`GetIsGlobalFunc()` from the fresh-body path while `CleanupToReparse`'s
keep-whole branch does not: a suspended TLA module body is kept whole
with NO debug bytecode until completion (benign; completion identical
to base; align or comment in future debugger work); (2)
`UpdateFunctionBodyImpl`'s relaxed assert keys on
`HasLiveSuspendedAsyncFrames` state, not the construction site — a
future unintended replacement would pass silently (comment exists);
consider a debug-build caller assert; (3) OOM rollback can leave the
Utf8SourceInfo dictionary pointing at the half-populated fresh body
(analysis-only, fatal-by-policy, disclosed); (4) retired-body
boundedness under REAL GC pressure is unverified — blocked by the
defect below; light evidence (10 attach cycles, max RSS 86 MB) only.

**New pre-existing engine defect (adversarial review; HIGH priority,
own lane required):** GC pressure while async frames are suspended
**silently drops promise continuations** — no debugger involved,
exit 0, base-identical (verified against the 710af1307 base binary):
churn (15×12000 objects) between suspension and resolve leaves the
awaiting function never resumed, and after the churn even a NEW async
function awaiting an ALREADY-RESOLVED promise never resumes. This
silently loses user continuations and currently blocks true
GC-pressure validation of coroutine lifetime work (including this
lane's pin). Filed as the next lane. **Closed by the gc-async-continuation
lane (2026-11-02): GC exonerated — the drop was `ch` host message ordering
(promise microtasks stamped "now" among absolute-due timers); fix, evidence
and the exposed latent TLA/SortedList bugs in
[e2-gc-async-continuation.md](e2-gc-async-continuation.md).**
