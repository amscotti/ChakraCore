# E1: agent-startup SIGBUS — diagnosis and fix receipt

Status: **COMPLETE** — root cause proven, one-line fix landed (`944921636`),
gates 2031/2083 green (pins unedited), churn A/B conclusive.

Lane: `agent-startup-sigbus` (base `8d2d68ea8`). Defect: rare SIGBUS (~0.3% under
churn; sharp reproducer 16-agent no-Quit churn ~2/120) during concurrent child
`ScriptContext::Initialize` in the `ch` host's agent threads, before any Quit.
Review: general and adversarial reviewers both **ACCEPTED** at `58d02c90b`
(see "Review outcome" below); pending merge to main.

## Proven facts (instrumented interposer `out/sigbus/sigbus-trace.dylib`)

12+ captured faults across 4 churn rounds (pre-fix test-profile binary, S6e
`cur` SHA-256 `f13a52f1…f35f`), all byte-identical in shape:

- Faulting thread: agent thread (`ch+75268` = `RuntimeThreadData::ThreadProc`),
  in `JsCreateContext → ScriptContext::Initialize → InitializePrototypes →
  JavascriptStringObject ctor` — the reviewer's exact site, and identical
  `si_addr = 0x30005c000` (card table base + 0x5c000; reviewer's other sample
  `0xa8c05c000` = different process card-table base, same +0x5c000).
- Faulting PC: `RecyclerWriteBarrierManager::WriteBarrier(void*)`
  (`libChakraCore.dylib+0xfc19c`), instruction `ldrb w10, [x8, x9]` with
  `x8 = cardTable (0x300000000)`, `x9 = x0 >> 14`.
- Barrier argument `x0 = sp+8` of the faulting thread: a compiler-materialized
  *stack temporary* holding the return value of
  `JavascriptLibrary::GetTypesWithOnlyWritablePropertyProtoChainCache()`
  (disassembly at `JavascriptStringObject ctor+0x2c..0x40`, source line
  `JavascriptStringObject.cpp:36`). Returning `Field(T)` (= `WriteBarrierPtr`)
  by value constructs the wrapper on the stack, and every `WriteBarrierPtr`
  constructor calls `WriteBarrierSet` → `RecyclerWriteBarrierManager::
  WriteBarrier(this)` (RecyclerPointers.h:351-355, 396-409) — i.e. the write
  barrier is applied to a *thread-stack address*.
- The card table is a process-global 2GB `PROT_NONE` reservation
  (`X64WriteBarrierCardTableManager::Initialize`, static init). Card pages are
  committed lazily per 256MB "section" (`pageSize*pageSize`, 16K×16K). The
  faulting thread's pthread stack `[0x16ff80000, 0x170003000)` (the mach
  region around it extends to `0x170008000`, including guard/unmapped tail)
  crosses the
  section boundary `0x170000000`; the card page for the upper half
  (`cardTable+0x5c000`, section 23) was never committed (PAL PCMI bits, mach
  region walk and engine `committedSections` all agree), so the barrier's card
  byte read faults with `si_code 1` on a `PROT_NONE` page. Faults are rare only
  because a stack must both (a) land in a never-committed section and (b) be
  touched by a stack-slot barrier before any covering section is committed.
- `+0x5c000` signature: card index = `address >> 14`; `0x5c000 << 14 =
  0x170000000` — the deterministic first stack-region boundary above the
  pthread stack cluster in every run of a boot session.

## Root cause (complete)

`X64WriteBarrierCardTableManager::OnThreadInit`
(lib/Common/Memory/RecyclerWriteBarrierManager.cpp:83) computes the number of
card-table sections to commit for the calling thread's stack as

```c
size_t numPages = (stackBase - stackEnd) / AutoSystemInfo::PageSize;   // rounds DOWN
```

On macOS ARM64 the PAL's `GetCurrentThreadStackLimits` returns a
non-page-aligned stack top (pthread stackaddr; observed
`[0x16ff80000, 0x170003000)`, remainder `0x3000`). The division drops the
partial last page, so `OnSegmentAlloc` covers only
`[0x16ff80000, 0x170000000)`; `segmentLastWritableAddress = 0x16FFFFFFF` stays
in card section 22 (already committed for the main thread), the card page for
section 23 (`cardTable+0x5c000`) is never committed, and the stack's top
`0x3000` bytes are unprotected.

A write barrier on a *stack address* is legitimate engine behavior
(`WriteBarrierPtr` construction barriers the constructed slot —
RecyclerPointers.h:351-355,396-409 — and `JavascriptLibrary::
GetTypesWithOnlyWritablePropertyProtoChainCache()` returns `Field(...)` *by
value*, so the compiler materializes a barriered temporary at `sp+8` inside
the `JavascriptStringObject` ctor, JavascriptStringObject.cpp:36). The first
such barrier whose stack slot lands in the uncovered tail reads the
uncommitted card page → SIGBUS si_code 1 at `cardTable+0x5c000`.

Evidence chain (all captured by `out/sigbus/sigbus-trace.dylib` on the S6e
test-profile binary):
1. 30+ identical faults: agent thread, PC `WriteBarrier` `ldrb [cardTable +
   (x0>>14)]`, `x0 = sp+8`, fault page `cardTable+0x5c000` PROT_NONE,
   PAL-PCMI/mach/engine BV all agree "never committed".
2. Caller disassembly: `JavascriptStringObject ctor` at
   JavascriptStringObject.cpp:36 materializes the `Field(...)` return
   temporary on the stack and barriers it (offsets `+0x50af40..0x50afbc`).
3. Probe thread experiment: calling exported `OnThreadInit` on 700 fresh
   threads commits the exact fault page `0x30005c000` when a probe stack
   crosses a section boundary — the mechanism works when the range is right.
4. Fault-time `X64WriteBarrierCardTableManager` diag (X64_WB_DIAG):
   `_stackbase=0x170003000`, `_stacklimit=0x16ff80000`,
   `_lastSegmentNumPages=32`, `_lastSectionIndexStart=Last=16` (stale heap
   call) — proving the agent's `OnThreadInit` ran with a 32-page count that
   excluded the top 0x3000 bytes where the fault occurred. (Caveat, per
   review: the `_stackbase/_stacklimit` fields are process-global and
   unsynchronized like the `_last*` fields and could in principle reflect a
   later thread's init; the proof rests on the values matching the faulting
   thread's actual stack bounds from the interposer's independent mach
   capture, not on field freshness.)
5. cs-hammer: the PAL `CriticalSection` passes a 480k-op mutual-exclusion
   hammer — no lock corruption involved; the bug is pure arithmetic.

Why rare and churn-shaped: the fault needs (a) a thread stack whose
non-page-aligned tail crosses a 256MB section boundary that no other segment
ever committed, and (b) a barriered stack slot in that tail during
`ScriptContext::Initialize`. More agent stacks → higher probability; the
pthread stack cluster sits directly below `0x170008000` on every run of a
boot session, giving the deterministic `+0x5c000` signature
(`0x5c000 << 14 = 0x170000000`).

## Fix

Round the page count up in `OnThreadInit` so the partial top page of the
stack is covered (engine-side one-liner; Windows stack limits are
page-aligned so behavior there is unchanged):

```c
// lib/Common/Memory/RecyclerWriteBarrierManager.cpp (OnThreadInit)
size_t numPages = (stackBase - stackEnd + AutoSystemInfo::PageSize - 1) / AutoSystemInfo::PageSize;
```

The round-up also cures a debug-only hazard the adversarial review
identified: `ToggleBarrier`'s unaligned byte length previously *wrote* the
top-partial-page card byte on a possibly-uncommitted card page under
`-StrictWriteBarrierCheck`; with stack tops covered, that page is committed.

### Ownership record (memory-management.md)

- Resource / creation call / source location: card-table card *pages*
  (RW commits inside the process-global 2GB card-table reservation created
  by `X64WriteBarrierCardTableManager::Initialize`, static init),
  committed via `X64WriteBarrierCardTableManager::OnSegmentAlloc` →
  `VirtualAlloc(MEM_COMMIT)`.
- Domain and allocator instance: process-global `x64CardTableManager`
  (one per process, shared by every runtime); commits serialized by
  `_cardTableInitCriticalSection`; mapped pages are released only at
  process exit (`~X64WriteBarrierCardTableManager`).
- Physical allocation base/extent: `[cardTable, cardTable+2GB)` PROT_NONE
  reservation; this change extends which *sub-ranges* get RW commits — it
  allocates no new arena and changes no release path.
- Logical views: `committedSections` BVSparse marks committed sections
  under the same lock (unchanged).
- Owner / transfer: none changed. The change only widens the committed
  range for a thread's stack from `[stackEnd, stackEnd+n*page)` to
  `[stackEnd, stackEnd+ceil(n)*page)`.
- Borrow invalidation: none (card pages are never decommitted;
  `OnSegmentFree` is a no-op).
- Reentrancy/user-code points: none new — `OnThreadInit` runs during
  `EnsureRecycler` (recycler creation) and first thread-context bind,
  before script runs.
- Thread/context: called on the initializing thread, same as before.
- Failure paths: unchanged — if the commit `VirtualAlloc` fails,
  `OnSegmentAlloc` returns false, `OnThreadInit` returns false, and
  `EnsureRecycler` throws OOM exactly as before.
- Accounting: at most one extra 16K card page per thread whose stack top
  is unaligned; bounded by thread count.
- Tests: see "Verification" — interposer card-commit observation (fix
  engages), interposed churn A/B (pooled 0/2450+ post vs 55/11200 pre; see
  Verification and Review outcome), clean
  churn A/B, focused S6 suites, both regression gates. No new rlexe entry
  (no deterministic JS-level reproducer; pins untouched).

## Verification (completed legs)

### Binaries

| Label | Path (lane worktree) | SHA-256 |
|---|---|---|
| pre-fix (S6e `cur` test profile) | `out/sigbus/bin-pre/ch` | `f13a52f1d0f0819b5ce1349e30cef0fc06c6579a83f9e0198488b8e01737f35f` |
| pre-fix dylib | `out/sigbus/bin-pre/libChakraCore.dylib` | `cef46d17e30d755487bd57280cf16593916a7096ccf217b7d226e7f3deb0673a` |
| post-fix (test profile, fix commit) | `out/sigbus/bin-post/ch` | `9e53be3d24f2b5a1b58beac5611854a77504173e0e15a658b6fb8a58242648e5` |
| post-fix dylib | `out/sigbus/bin-post/libChakraCore.dylib` | `fe1fda69903365ca5801f9c4157be70807552e077cd3f2b190b9174b125783e4` |

Post-fix disassembly proof the fix is compiled in (`OnThreadInit`):
`sub x8,x8,x1; add x8,x8,#16383; lsr x2,x8,#14; bl OnSegmentAlloc`.

### Churn A/B (rvs.mjs driver; nq_*.js; SIGKILL at killms; all legs 16/8 agents, 60 ms)

Pre-fix, fault-catching interposer inserted (pooled):

| Round | Leg | Faults/runs |
|---|---|---|
| r3 | 16/60 interp | 4/1200 |
| r4 | 16/60 interp | 8/1600 |
| r5 | 16/60 interp | 10/1600 |
| r6 | 16/60 interp | 8/1600 |
| r7 | 16/60 interp | 4/1600 |
| r8 | 16/60 interp | 10/1600 |
| r9 | 16/60 interp | 11/2000 |
| **pooled** | | **55/11200 = 0.49%** |

Pre-fix, clean (no interposer): 16/60 ms **2/1000**; 8/60 ms **2/700**
(total 4/1700 = 0.24%; matches the ~0.3% churn rate and the reviewer's
8-agent 1/80 leg). The interposed pre-fix rate is ~2× the clean rate — the
interposer perturbs timing/layout enough to push more stacks across
uncovered boundaries; every A/B above is like-for-like (both sides equally
instrumented), and the clean A/B (0.24% pre → 0/1500 post) corroborates
independently of instrumentation.

Post-fix: clean 16/60 ms **0/1500** (0/600 + 0/600 + 0/300 across three
rounds) and 8/60 ms **0/300**; interposed 16/60 ms **0/2000**. If the
pre-fix interposed rate (0.49%) still applied, P(0 faults in 2000) ≈ 5.5e-5.
Zero other failures (all non-fault runs killed by the driver as designed).

### Fix-engages evidence (interposer SIGTERM dump, nq_16, post-fix)

```
[263] mmap-fixed-rw range=0x30005c000..0x300060000 size=0x4000 tid=3587
```

An agent thread (tid ≠ main) now commits the exact card page
(`cardTable+0x5c000`) that was PROT_NONE at every pre-fix fault; pre-fix
dumps never showed any agent-tid card commit. Manager diag also shows
unaligned stack tops on agents (e.g. `_stackbase=0x170793000`), the
condition the round-up now covers.

### Gates (pins unedited — no gate_counts.json changes)

| Gate | Command | Result |
|---|---|---|
| test profile | `zig build check-regression` | **PASS 2031/0** |
| debug-static | `zig build -Dprofile=debug-static check-regression` | **PASS 2083/0** |

Both gates ran through the shared gate lock. (Operational note for the
integrator: `cc-gate-lock.sh` `exec`s the command, dropping its EXIT trap,
so the lock dir leaks after every gate command and needs a `rmdir` reclaim;
observed repeatedly during this lane.)

### Focused S6e/S6 tests

Direct `ch` invocations of the four `test/es7/s6-*.js` tests with their
rlexe flags behave **identically on the pre-fix and post-fix binaries**
(growable 3×/3× exit 0; the other three fail the direct invocation the same
way on both binaries — a harness-context artifact, since the same tests are
included and pass in both regression gates above). No behavioral change
from the fix. Churn datapoint on the post-fix debug-static binary
(`out/sigbus/bin-post-dbg/ch`, SHA `8f0a245b…ff85`): 16 agents/900 ms →
**0/120**.

### Coverage actually exercised

- Changed line: `X64WriteBarrierCardTableManager::OnThreadInit` page-count
  rounding (single expression). Exercised by every thread that creates a
  recycler or binds a context — in the churn, 17 threads per run across
  thousands of runs on both profiles; the interposer's card-range op log
  directly observes the changed commit sets (agent-tid commit of
  `cardTable+0x5c000` appears post-fix, never pre-fix).
- No deterministic JS-level reproducer exists (the fault needs a specific
  stack/section layout), so no new rlexe entry was added and no pin was
  bumped. The A/B churn, interposer evidence and gates are the coverage.

## Review outcome

Both reviewers **ACCEPTED** with zero `[bug]` findings.

- **General review**: causal chain re-verified from source end-to-end —
  card-table sizing/section math (`GetSectionIndex` = `addr/(pageSize²)` →
  256MB sections on 16K pages), `OnThreadInit`/`OnSegmentAlloc` arithmetic
  including unaligned low ends (macOS rounds the PAL low limit up to 16K;
  card-down rounding covers the partial low card conservatively),
  `committedSections`-guarded idempotent commits (no overcommit/leak), all
  four `OnThreadInit` call sites (static init, `EnsureRecycler`, first TLS
  bind, background jobs), and Windows/Linux page-aligned limits making the
  round-up an identity there. Ownership record judged compliant with
  `doc/memory-management.md`. Re-ran the test-profile regression gate on
  hash-verified binaries: **PASS 2031/0**. Its nits are folded into this
  doc (mach-region extent label, missing hex digit, diag-field staleness
  caveat, interposer amplification note).
- **Adversarial review**: post-fix campaigns on the existing binaries —
  pooled interposed **0/2450** (16/60ms ×1200, 8/60ms ×500, 2/60ms ×500,
  16/600ms ×250), clean 16/60ms **0/500**, debug-static 16/900ms **0/120**,
  zero anomalous exits; if the pre-fix interposed rate (0.49%) still
  applied, P(0/2450) ≈ 5.8e-6. Root-cause attacks (guard-page touch, commit
  race, GC/thread suspension, misattributed PC, interposer circularity)
  all refuted; independent non-interposer corroboration from a
  CrashReporter `.ips` capture with the byte-identical `+0x5c000` signature
  at the same `WriteBarrier` PC via a second site in the same class
  (`WriteBarrierPtr<char16_t>` in `CharacterBuffer →
  ThreadContext::FindPropertyRecord` during `EnsureRecycler`); no `.ips`
  captures post-fix. Its gate rerun was deferred by a no-rebuild
  constraint (provenance note: the general review's 2031/0 rerun plus the
  post-merge main gates are the authoritative evidence). Its findings are
  folded: the low-end PAL round-up residual and the `rvs.mjs` dbg
  symbolication pin (both below), plus the `ToggleBarrier` note in Fix.

## Residuals

1. The barriered-stack-temporary pattern (functions returning `Field(...)`
   by value, e.g. `GetTypesWithOnlyWritablePropertyProtoChainCache`)
   remains; with stacks fully covered it is benign (sets a card bit for a
   stack address — the same behavior this code has on Windows).
2. A `WriteBarrierPtr` constructed in non-recycler *malloc* memory could
   still touch an uncommitted card section. No such site is known; not
   addressed by this fix.
3. The PAL still reports unaligned stack tops (`GetInternalStackLimit`
   returns raw `pthread_get_stackaddr_np`); the engine now rounds up, so
   PAL consumers that need alignment must still round themselves.
4. Statistical bounds: post-fix clean 16-agent 0/1500 and 8-agent 0/300
   (pre-fix 0.24% pooled → rule-of-three upper bound ~0.20%/0.99%
   respectively); the powered leg is interposed 0/2000 vs pre-fix 0.49%
   (P≈5.5e-5 if unfixed; upper bound ~0.15%).
5. Debug-profile churn is a 0/120 datapoint only (low single-run
   reproduction probability on debug).
6. PAL low-end up-round (adversarial review): `GetInternalStackLimit`
   rounds the stack's low limit **up** to 16K (pal_thread.cpp), excluding
   up to 16K-1 of the true deepest stack from the reported range. A
   barriered slot below the reported low limit, in an otherwise
   uncommitted section, could still fault — the same class as the fixed
   bug, but it requires a barriered temporary within ~16K of the absolute
   stack bottom (deepest recursion near the guard page), far rarer than
   the top-page case where execution begins. Not addressed; documented.
7. `rvs.mjs` maps any `*dbg` label to the **pre-fix** debug image's
   symbolication offsets for fault reporting. Fault counting is
   unaffected; an interposed `postdbg` leg would symbolicate against the
   wrong image — all dbg legs in the recorded evidence ran without the
   interposer.



