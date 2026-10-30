# E1: agent-startup SIGBUS — diagnosis and fix receipt

Lane: `agent-startup-sigbus` (base `8d2d68ea8`). Defect: rare SIGBUS (~0.3% under
churn; sharp reproducer 16-agent no-Quit churn ~2/120) during concurrent child
`ScriptContext::Initialize` in the `ch` host's agent threads, before any Quit.

## Status: diagnosis in progress (doc updated at fix commit)

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
  faulting thread's pthread stack `[0x16ff80000, 0x170008000)` crosses the
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
`[0x16ff80000, 0x170000000)`; `segmentLastWritableAddress = 0x16FFFFFF` stays
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
   excluded the top 0x3000 bytes where the fault occurred.
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

