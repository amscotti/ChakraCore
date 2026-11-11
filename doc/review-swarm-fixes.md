# Whole-branch swarm review — fix receipt (2026-11-10)

An 18-agent swarm reviewed `modernize/baseline` at `9cf54f48c` vs
`master 97e68c917` (767 files, 498 commits) across architecture, C, C++,
memory/GC, JS-engine performance, code quality, concurrency,
C/C++/Zig/JsRT boundaries, security, build/CI, and tests/docs, plus an
adversarial critic and gap follow-ups. Verdict: **CONDITIONAL** — push
as staged modernization only, never as B4/C4-complete, concurrent-GC,
OOM-safe, or perf-regression-free. This receipt records the fix pass
that followed: what changed, the ownership reasoning, the exact
verification actually exercised, and what stays explicitly unclaimed.
Open follow-ups live in [open-cards.md](open-cards.md) §9 (and §3 for
GCStress).

## Push scope (pinned)

Demonstrated green: Mac no-JIT interpreter gates, Mac ARM64 JIT
(test262 discharged per batch), Linux x64 native proof on the proof
host. Still owed and **not** claimed by this branch tip:

- post-D1–D4 Linux native handoff + hosted-CI first green run
  ([b4-d1-d4.md](b4-d1-d4.md) §5);
- concurrent-GC / full-tree and JIT-profile GCStress matrices;
- Recycler/C-library OOM rollback tests;
- any measured performance-regression verdict (no perf gate/run);
- external-ArrayBuffer finalizer/detach-transfer native tests;
- a standalone Phase-3 API/versioning decision for the additive JsRT
  surface.

## Fixes

### 1. GCStress startup SIGSEGV (engine, e12 card)

Domain: native C++ (`JsUtil::ThreadService`, owned by `ThreadContext`
by value; `bin/GCStress` passes `nullptr` meaning "no host callback,
Recycler owns its background thread"). Root cause:
`ThreadService::HasCallback()` tested `this != nullptr` inside a
non-static member called through a null pointer — UB whose check
clang elides (Nov-5 crash report:
`HasCallback ← InitializeConcurrent ← EnableConcurrent`, addr 0x0).

- `lib/Common/Common/ThreadService.h`: `HasCallback()` no longer tests
  `this`; the null-pointer contract is documented as a caller
  precondition.
- `lib/Common/Memory/Recycler.cpp`: `InitializeConcurrent` guards
  `threadService == nullptr` before any member call;
  `EnableConcurrent` normalizes a null service to a process-lifetime
  no-callback instance, so `this->threadService` is never null while
  concurrent GC is enabled (every later `HasCallback()`/`Invoke()` site
  is then valid; `FinalizeConcurrent` still nulls it at teardown).
  Semantics unchanged: no callback ⇒ Recycler-owned threads.
- `bin/GCStress/GCStress.cpp`: comment pinning the nullptr contract.
- Regression pins: `GcStressStartupContractTests` (3 tests) in
  `tools/build/test_build_tools.py`, run by `check-build-tools` /
  `check-selftest` (suite 67 → 70 tests).

Behavioral result: startup crash gone (below). The fix also exposed a
deterministic **concurrent-mode** heap-op crash, now carded in
[open-cards.md](open-cards.md) §3 — that is the unrun concurrent-GC
matrix, not a startup regression.

### 2. Diagnostic-tool safety (tools only, engine unaffected)

`tools/e1-agent-startup-sigbus/cs-hammer.c`:
thread count validated `1..64` via `strtol` (was unchecked `atoi`
smashing `pthread_t t[64]`); `calloc` result checked; `max_in_cs`
updated with a CAS loop and `viol` set/loaded atomically (was racy
plain RMW/read); `pthread_create`/`pthread_join` returns checked and
only created threads joined; `g_cs` freed and handle `dlclose`d.

`tools/e1-agent-startup-sigbus/sigbus-trace.c`: prominent
async-signal-safety warning (file header + README) — the handler's
mutex/`fprintf`/`backtrace`/`dladdr`/`mach_vm_region` can self-deadlock
a faulting process holding `log_mtx`; best-effort diagnostic only,
never link/ship. Also: dead `record2`/`resolve_sym` removed,
`void (*)()` → `void (*)(void)`, LP64-truncating `long` distance
removed (covering test in `uintptr_t`), 700-thread probe wave checks
`pthread_create` and detaches instead of leaking joinable handles.

### 3. JsrtContextCore ownership record → literal §3

`lib/Jsrt/Core/JsrtContextCore.cpp`: the `FetchImportedModule` record
now states resource/creation, domain/allocator (Recycler, no native
allocation), owner/transfer (synchronous, host `JsAddRef` to retain, no
engine root, no AddRef taken here), borrow invalidation, **permitted
thread/context** (script thread bound to the runtime, current
ScriptContext; callers in `SourceTextModuleRecord` and dynamic import
under `BEGIN_LEAVE_SCRIPT`), and **every failure exit** (OOM throw
before host call; host error → `E_INVALIDARG`, out-param untouched;
attributes without capable host → `E_NOTIMPL` before allocation; no
callback → `E_INVALIDARG`), plus accounting (one Recycler string per
fetch). `FetchImportedModuleFromScript`, which had no record, now
cross-references the same contract. No behavior change.

### 4. ParserPch coupling correction (docs)

`doc/architecture.md` §3.1: Parser is coupled to Runtime at TU level —
26 of 27 Parser TUs include `ParserPch.h` (`ParserPch.h:21-29` pulls
`Runtime.h`, ByteCode and `Library/JavascriptFunction.h` headers). The
narrower per-file boundary (e.g. `RegexParser.cpp` takes only a
`ScriptContext*`) is the accurate claim; single-file greps understate
coupling.

### 5. owned-bytes gating (build)

`doc/examples/owned-bytes.zig` tests now run as a `check-selftest` leg
in both `build.zig` and `tools/build/linux.zig` (host target, mirroring
the existing Linux `gate_tests` pattern), so the repo's only
`checkAllAllocationFailures` coverage cannot rot ungated. Scope is
unchanged and restated in `doc/memory-management.md` §5: injected-
allocator domain only, not Recycler/C OOM.

### 6. Plan B4-status reconciliation (docs)

`doc/modernization-plan.md` header, milestone table, §1 and the
blocked-on-you/checklist sections still said B4 BLOCKED-with-green-build
and "zero behavior checks", contradicting the native proof recorded in
[b4-linux-run.md](b4-linux-run.md), the [b4-gate.md](b4-gate.md)
2026-11-09 update and the plan's own 2026-11-10 B4+D1–D4 update. They
now state: proof completed natively on the proof host (2841/0, 2305/0,
77,354/3,190); the D1–D4 handoff, pin re-record, certified refresh and
hosted-CI green remain owed before any default-rollout claim; the +230
delta stays tracked, not claimed identical.

### 7. WeakRef/FinalizationRegistry trace proof (docs, no code change)

The review could not sustain "no missing trace/barrier" past Field
existence. Verified mechanism now recorded in
[c2-s13-weakref.md](c2-s13-weakref.md) (GC contract): all four types are
`RecyclerNew` normal blocks; marking scans the full object extent
(`SmallNormalHeapBlock` → `ScanObjectInlineInterior(address,
objectSize)`), tracing subclass Fields exactly like `JavascriptMap`;
weak handles are `weakReferenceMap`-owned and sweep-cleared. No
per-type visitor exists or is missing. Concurrent-mark behavior under
load remains the GCStress card.

### 8. KeptObjectList capacity contract (code comment)

`lib/Runtime/Base/ThreadContext.cpp` `KeepDuringJob`: the bound is
spec-determined (every distinct target in one job; a hard cap would
break WeakRef semantics and is deliberately not imposed). Exhaustion is
a controlled Recycler OOM throw out of the deref/constructor, never a
silent drop; the kept prefix stays for the boundary clear. The `O(n²)`
linear dedupe note is retained.

### 9. ICU headers out of the namespace (header hygiene)

`lib/Parser/RegexUnicodeProperty.h`: `<unicode/uchar.h|uscript.h|uset.h>`
moved from inside `UnifiedRegex::UnicodeProperty` to global scope.
Including C headers inside a namespace nests ICU symbols and include
guards, breaking any second includer (today masked by the sole consumer,
`RegexParser.cpp`). Unqualified uses still resolve to the global ICU
names; verified by build + es7 tests below.

### 10. VScanAbort state snapshot completeness (parser)

`VClassPass0` already restored cursor, multi-units, scan limit,
speculative flag and three deferred-error slots on abort; the validate
pass never touches pass-1 build buffers (`vStringBuffer`,
`vScratch*`, named groups — verified by inspection of all `VValidate*`
bodies). The fourth deferred slot (`deferredIfNotUnicodeError`) and
`vClassSawNested` are now snapshotted/restored too, so the abort path
stays complete if a helper starts writing them. Comment records the
buffer-purity fact.

### 11. This receipt + open-cards

[open-cards.md](open-cards.md): GCStress startup card closed
(fixed-here), concurrent-mode GCStress crash card added (§3), §9 added
for the swarm residuals itemized above.

## Verification actually exercised

All commands run in the fix session against the edited tree
(`mise exec -- zig` 0.17.0, Mac ARM64):

| Check | Command | Result |
|---|---|---|
| C tools, strict | `cc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wstrict-prototypes -fsyntax-only` on both tools | 0 diagnostics (sigbus previously 4 warnings) |
| cs-hammer guards | built `/tmp/cs-hammer`; counts `65`/`0`/`abc` | usage + rc 1 each; `64` passes validation, fails cleanly at `dlopen` (no dylib) |
| sigbus dylib link | `clang -arch arm64 -O2 -dynamiclib` | links |
| Engine build | `mise exec -- zig build` | exit 0 (one pre-existing `__yield` warning in untouched `AtomicsObject.cpp`) |
| Full gate, test profile | `mise exec -- zig build check` | exit 0 — regression at pin **2053/0**, bytecode verify, native, smoke, metadata, selftest (incl. 70 build-tool tests and owned-bytes 4/4) |
| Selftest incl. owned-bytes | `mise exec -- zig build check-selftest` | exit 0; `owned-bytes.test.*` 4/4 OK visible in output |
| Contract pins | `python3 -m unittest test_build_tools.GcStressStartupContractTests` (in `tools/build`) | 3/3 OK |
| Focused JS | `runtests.py --test --variants=disable_jit` on `es7` + `es6module` | 140/0 (regex v/property-escapes, WeakRef/FR, module fetch) |
| GCStress non-concurrent | `./zig-out/bin/GCStress -js -off:ConcurrentCollect` (30 s bounded) | init complete, ≥2 full heap walks (8,697 → 33,782 objects), killed healthy |
| GCStress concurrent | `./zig-out/bin/GCStress` (default) | SIGSEGV ≤1 s at `GCStress.cpp:105` — expected open card, not the startup crash (Nov-5 vs Nov-10 crash reports compared) |

## Explicitly untested / unclaimed

- No ASan/TSan/UBSan run; no live `cs-hammer`/`sigbus-trace` execution
  against a dylib/process (compile + arg-validation only).
- No concurrent-GC, background-rescan or MAP_JIT stress execution
  beyond the GCStress repro above; no `-RecyclerTest` rerun
  (debug-static leg not built in this pass).
- No JIT-profile gate rerun in this pass (test profile only); no Linux
  execution (this host is Mac ARM64) — Linux legs of every fix ride the
  D1–D4 handoff.
- No performance measurement of any kind (see open card).
- `OperationAbortedException` catch-chain, host `exit(1)`/`callbackState`
  residuals, Recycler OOM and external-ArrayBuffer finalizer tests:
  tracked, not fixed here (open-cards §9).

## Follow-up pass (concurrent GCStress + pre-existing failures)

Requested after the receipt above: fix the newly exposed
concurrent-mode `GCStress` crash and the pre-existing failures noted
as out-of-scope (StyleChecks gates, the `__yield` warning,
`-RecyclerTest`). All items below are in the same uncommitted tree.

### Concurrent GCStress crash — fixed

Bisection (each a bounded 12 s probe of the rebuilt binary):
ConcurrentMark off → clean; ParallelMark off → crash; ConcurrentSweep
off → crash; ForceBlocking → clean; software-write-barrier off →
crash. Object-kind bisection: Scanned+Leaf graph crashes,
Barrier+Leaf clean, Scanned with barrier stores clean. Engine `ch`
under true concurrent GC (`-ConcurrentRuntime -CollectGarbage
-GCMemoryThreshold:1`, es7) ran 108/109 with a single baseline-text
diff and no crash — the engine thread model is healthy; the
standalone test model was not.

Root cause: GCStress heap-internal Scanned slots store with plain
writes (`FieldNoBarrier` + `Location::Scanned`). Only Windows
hardware write-watch (`RECYCLER_WRITE_WATCH`) makes plain stores
visible to a concurrent rescan; on software-write-barrier platforms a
reference stored into an already-scanned object is never revisited
and its target is swept (vptr-null in `GetRandomLocation`, background
thread observed in `BackgroundSweep` in both crash reports). Fix:
`Location::ScannedHeap` in `bin/GCStress/GCStress.h` — `Scanned`
under write-watch, `Barrier` otherwise — used by ScannedObject and
FinalizedObject heap slots (stack roots unchanged; `ScanStack`
rescans them). GCStress also now mirrors
`ThreadContext::EnsureRecycler` (`SetIsThreadBound` +
`RecyclerWriteBarrierManager::OnThreadInit`). The long-standing
`SWB-TODO: is this correct?` comments on those fields are answered.

Verified: concurrent soak alive at 60 s (678 full heap walks,
generation 679), repeated 12 s probes alive in both modes (143 walks
each). Pins: `GcStressConcurrentContractTests` (3 tests; suite 73).

### `-RecyclerTest` OBJALIGN assert — fixed

The assert was wrong, not the allocator: `StressTest.h` asserted
32-byte alignment while the Recycler guarantees
`HeapConstants::ObjectGranularity` = 16 (`HeapInfo::
ObjectAlignmentMask == 0xF`). `OBJALIGN` is now 16 with the guarantee
cited. Verified on rebuilt debug-static: `-RecyclerTest` (infinite
by design) alive at 45 s and 20 s bounded soaks with no assert
(previously <1 s, rc=133, 100% repro).

### StyleChecks gates — fixed, all four green

- `check_eol.sh`: 6 branch-added `tools/test262/*-triage.json`
  artifacts lacked a trailing newline (appended; JSON re-validated).
  The gate previously reported only 3 because of the truncated run
  class below.
- `check_copyright.sh` truncated mid-list on branch-sized diffs:
  BSD `xargs -I %` aborts ("command line cannot be assembled, too
  long") after ~60 files. Replaced with a while-read loop in
  `check_copyright.sh` and `check_eol.sh` — the gates now inspect
  the whole branch diff, which is how the full failure set (205
  files) became visible.
- Header normalization, per the gate's existing intent (touched
  files carry the combined Microsoft + Contributors notice): 150
  Microsoft-only files gained the contributors line; near-miss
  variants normalized (`(c)`→`(C)`, `MIT License`→`MIT license`,
  missing `file` in the license line, short dash rule);
  `check_copyright.py` combined-notice year generalized from
  `( 2022)?` to `( \d{4})?` (legacy files carry 2021; all three
  notice lines still required verbatim); PAL notice prepended to
  `pal/src/map/virtual.cpp`; project headers added to 35
  branch-authored files that had none (tools scripts/repros, test
  module helpers, evidence reproducers, Dockerfile); generated
  `.log`/`.time` run artifacts excluded (same class as the existing
  `.baseline`/`.json` exclusions).
- Result: copyright, EOL, ASCII and tabs scripts all exit 0 over
  the full branch diff. Caveat: on macOS, BSD `grep` lacks `-P`, so
  the tabs/ASCII inner greps are vacuous locally (pre-existing;
  CI runs them on Ubuntu). Source files changed in these passes
  were independently checked tab-free.

### `__yield` implicit declaration — fixed

`AtomicsObject::EntryPause` took the `_M_ARM64` branch under clang
(both `_M_ARM64` and `__aarch64__` defined), but only MSVC provides
`__yield` without `<arm_acle.h>`. Branch order swapped to prefer
the `__aarch64__` asm form. The warning is gone from a full build.

### Verification for this pass

`zig build -Dprofile=debug-static` exit 0; `zig build check`
(test profile) exit 0 including 73 build-tool tests; GCStress
probes and `-RecyclerTest` soaks as above; all four StyleChecks
scripts exit 0. Working-tree audit: every modified source file
passes `check_copyright.py` individually (the shell gate scopes to
committed diff, so this pre-checks the eventual commit).

Still explicitly untested after this pass: full concurrent-GC
matrices (full-tree, JIT-profile, WeakRef-specific), sanitizers,
Linux execution of every fix (Mac ARM64 host; Linux rides the
D1–D4 handoff), perf measurement. See
[open-cards.md](open-cards.md) §3/§9.
