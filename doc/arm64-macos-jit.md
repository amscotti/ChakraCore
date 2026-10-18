# ARM64 macOS JIT

Status of the native JIT port for Apple ARM64, as of 2026-10-18. This is a
worktree effort. It is not a C2 language batch, and it is not milestone B4.
B4 in [modernization-plan.md](modernization-plan.md) is the still-blocked
Linux x64 JIT proof. The language work stays on the modernization branch,
interpreted, with `-DDISABLE_JIT=1`. This document is the record of what the
Mac JIT port has proved, how the Apple calling convention differs from the
Windows ARM64 frame the backend was written for, and the steps still required
before the branch can merge.

Read [Current state and handoff](#current-state-and-handoff) before editing.
The port is committed on `jit/arm64-macos`: `07d6965e8` (`jit: native ARM64
macOS port`), `2372fcc81` (JIT build/runtime/bytecode checks), `9ccc535a8`
(XData lifecycle + installer symlink hardening), and `fcbc434c9` (generator
write-barrier and empty-page leaks). The branch tip is `fcbc434c9`; it is not
merged. The installed binary is `out/jit/bin/libChakraCore.dylib`, 26238584
bytes, timestamp 2026-10-18 13:58. `out/jit/bin/ch` is the host (788320 bytes); it
dlopens the sibling dylib, so trust the dylib timestamp. The suite logs named
below are ephemeral local artifacts from the original runs; they are not
checked in.

Path convention: `<worktree>` is this checkout, and
`<modernization-checkout>` is the sibling ChakraCore checkout on
`modernize/baseline`. Bare `.js`, `.log`, and `.lldb` names refer to scratch
files that may no longer exist. In commands, `$TMPDIR` is a scratch directory
outside the repository.

## Current state and handoff

The port compiles and runs JavaScript with the JIT on. Throw/catch works.
An 8-argument `apply(arguments)` with default inlining works. `new.target`
works for the argument counts that were measured. Generator JIT
(`-JitES6Generators`) now runs all measured yield/resume/return/throw and
try/finally shapes. The three suite runs below were rerun on the 13:08 binary
(`jit-*-inline.log`); the 18:13 and 19:09 builds are not the same object set.

- `disable_jit` is **1973/0** (`jit-disable-inline.log`).
  Wall time varies with host load: the prior 19:27 run was 42.51 s
  (`jit-disable-final.log`) and the 19:09 run was 54.5 s
  (`jit-disable-repair3.log`).
- `dynapogo` is **1943/286** (`jit-dynapogo-inline.log`, 501.49 s; the 19:09
  run is `jit-dynapogo-repair3.log`, 488.15 s). Every failure is in an
  asm.js/WASM folder and exits 0, so the signal count is zero.
- `interpreted` is **2367/397** (`jit-interpreted-inline.log`): 397 asm.js/WASM
  failures, zero signals. The two StackTrace line diffs are fixed.

The branch is committed (`07d6965e8`, follow-up `2372fcc81`) but not merged.
`zig build -Dprofile=jit check` runs the generic smoke, `check_jit.py`, and
`verify_bytecode_pure.py --jit --verify`. The JIT static archive is
deterministic (`ZERO_AR_DATE=1`, epoch/0:0 member metadata). The base object
counts are 538 shared / 536 static; the JIT graph adds 89 backend TUs for 627
shared.

### Second-pass fixes on the 16:37 dylib

These fixed the compiler failures the first pass left open. Each has a
focused repro; the full logs are named below.

- **`DebuggerCommon/AsyncDynamicAttach.js` hang.** With `-nonative`, the
  debugger attach reparsed functions and `DynamicProfileInfo` turned profile
  opcodes on mid-flight. A suspended async/generator frame kept the old
  bytecode offsets, so the async stepper never completed. `DynamicProfileInfo`
  now leaves profile info off when native codegen is disabled
  (`lib/Runtime/Language/DynamicProfileInfo.cpp`), and the debug interpreter
  loop is split into profiled and unprofiled variants selected by
  `DoInterpreterProfile()` (`InterpreterStackFrame.cpp/.h`), including the TTD
  debug loop. The exact command exits 0 and `disable_jit` is 1973/0
  (`jit-disable-reviewfix.log`).
- **Default no-JIT link.** `XDataAllocator.cpp` is in the common object graph
  even with `DISABLE_JIT=1`, but its Apple `mac_fde_wrapper` references were
  compiled unconditionally while `AssemblyCommon.cpp` defines the wrapper only
  when JIT is on. The Apple path is now gated on `ENABLE_NATIVE_CODEGEN`.
  `zig build` (default test profile) links again.
- **Apple JIT W^X abort (first half).** The JIT heap asked for
  `PAGE_EXECUTE_READWRITE` while freeing part of a code page. Apple Silicon
  denies `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` with `EACCES`, and the
  fatal `CustomHeap_BadPageState` path aborted
  (`es6GeneratorJit/async-jit-bugs.js`). The write windows in
  `CustomHeap::EnsureAllocationExecuteWriteable`, `Heap::FreeAllocation`, and
  `JITThunkEmitter` were changed to `PAGE_READWRITE` on Apple ARM64, then
  restored to `PAGE_EXECUTE_READ`. That removed the `EACCES` abort but **did
  not close the transient non-executable window**: the process-wide
  `mprotect(PAGE_READWRITE)` still made already-installed functions on the
  same page non-executable for other threads. The per-thread fix is in the
  third pass below.
- **ARM64 concat overflow flag.** `Lowerer::InsertAddWithOverflowCheck` emitted
  a non-flag-setting `ADD` and then `BVC`; x86's `ADD` sets flags, ARM64's does
  not. The stale V bit sent multi-operand string concat into the fatal OOM
  path (`Operators/strictequal.js`, `es6/es6IsConcatSpreadable.js`). The add
  now always sets flags (`lib/Backend/Lower.cpp`).
- **Generator JIT epilogue.** `InsertNullOutGeneratorFrameInEpilogue` created
  a register operand without assigning a physical register. On ARM64
  `RegNOREG` encodes as x15, so the null store became
  `mov x15,#0; str x15,[x15,#0x20]` and the second `it.next()` faulted at
  address 0x20. The epilogue now uses caller-saved x12 and stores XZR directly
  on ARM64. ARM32 materializes zero into r3, not the uninitialized value and
  not r12 (the ARM32 SCRATCH_REG). All `-JitES6Generators` rlexe variants pass.

### Third pass: per-thread MAP_JIT write windows (18:13 dylib; rebuilt 19:09)

The 18:13 and 19:09 installs are separate object sets, not two names for one
build. The 20:39 binary is the current one.

The `PAGE_READWRITE` window from the second pass was still a process-wide
`mprotect`. A background compiler thread in `BackwardPass` could enter
`EmitBufferManager::CommitBuffer` and make a page writable while the main
thread was executing an already-installed function on the same 16 KB page;
the main thread then took `EXC_BAD_ACCESS/KERN_PROTECTION_FAILURE` (SIGBUS).
Restoring `PAGE_EXECUTE_READ` afterwards did not close the window.

The third pass uses Apple's per-thread `pthread_jit_write_protect_np` and
makes the custom-heap segments real `MAP_JIT` mappings:

- `VirtualAllocWrapper::AllocPages` marks a custom-heap reservation with
  `MEM_RESERVE_EXECUTABLE`. PAL reserves it with `mmap(MAP_JIT, PROT_NONE)`.
  Recycler and ordinary heap reservations keep their old mapping.
- On the first commit of each page, PAL performs the **one** `mprotect` a
  MAP_JIT page accepts on Apple Silicon: `PROT_READ|PROT_WRITE|PROT_EXEC`.
  The page is zero-filled with the calling thread's write window open. PAL
  tracks the transition per page (`pJitState`); it never mprotects that page
  again. On this host a second `mprotect` - even `PROT_NONE` or the same
  flags - returns `EACCES` (13). A base `PROT_READ|PROT_EXEC` mapping cannot be
  written with `pthread_jit_write_protect_np(0)`; the write faults. That is
  why the base stays RWX and the *thread* state supplies the RX view.
- `VirtualProtect` on a JIT region no longer calls `mprotect`.
  `PAGE_READWRITE` and `PAGE_EXECUTE_READWRITE` call
  `pthread_jit_write_protect_np(0)` for the calling thread;
  `PAGE_EXECUTE_READ` calls it with 1. `pProtectionState` still records the
  logical protection, so `HeapPageAllocator::ProtectPages`'s
  `desiredOldProtectFlag == memBasicInfo.Protect` check keeps working.
  `PAGE_NOACCESS`, `PAGE_EXECUTE`, and `PAGE_READONLY` are rejected with a
  clear error. The DBG_DUMP `PAGEALLOCATOR_PROTECT_FREEPAGE` path that asks for
  `PAGE_NOACCESS` is therefore not supported on Apple ARM64; the fast jit
  profile does not use it. `VirtualProtect` also rejects a range that would
  write past the region and checks `lpflOldProtect` in release builds.
- Decommit cannot use `mprotect(PROT_NONE)` after the RWX transition: the
  second `mprotect` returns `EACCES` (13), and `mmap(MAP_FIXED|MAP_JIT)` cannot
  replace the mapping either. `madvise(MADV_DONTNEED)` is advisory and a no-op
  for a partially live MAP_JIT segment on this host, so it does not release
  those physical pages. PAL zero-fills every committed page in the decommit
  range under a `VIRTUALJitWriteProtect(TRUE/FALSE)` window, then keeps
  `madvise` as a hint. A stale call executes zero instructions and traps
  (SIGILL) instead of running stale code. This is containment, not a physical
  release: decommit is page-granular and only reached when the page has no live
  functions, while whole-segment release still `munmap`s in the `MEM_RELEASE`
  path.
- Raw writes that are not bracketed by `VirtualProtect` use the PAL
  `VIRTUALJitWriteProtect` pair: `FillDebugBreak` in
  `AllocNewPage`/`AllocLargeObject`, the `.eh_frame` slot write in
  `XDataAllocator::Alloc` and `JITOutput::RecordUnwindInfo`, and the closing
  of the free-path windows in `FreeAllocation`, `FreeLargeObject(s)`, and
  `FreePage`.

Ownership and threading contract: the thread that performs a write owns the
window. It opens the window immediately before the raw write or `memcpy`,
performs no JavaScript call, allocation, or JIT execution inside it, and
closes it before returning. Windows are not nested: the inner close would
also close the outer window. `CommitBuffer` holds the emit-buffer manager's
critical section across its per-page window, so two writers on the same
allocator do not overlap. Other threads keep their default execute state and
are never mprotected. A violation faults the writing thread with SIGBUS; the
per-thread state dies with the thread, and the page mapping is unchanged.
No caller holds a write window across `Release`/`ReleasePages`: the free paths
close theirs first because release does not write code bytes. PAL's commit and
decommit zero-fills open and close their own window around the memset.

Verified on this host with a standalone C probe that maps MAP_JIT, toggles the
thread state, writes, and executes. The probe is not checked in, so its exact
call counts are anecdotal: one thread wrote slot B 3000 times while another
thread executed slot A on the same page (8,590,887 calls, 0 bad results), and
the engine's direction - a writer thread toggling its own window while the main
thread executes - was 549,992 calls, 0 bad results. A write with the window
closed and an execute with the window open each faulted with SIGBUS, as
expected. `ObjectSpread_Limits.js` with the crashing
`-forceNative -off:simpleJit -bgJitDelay:0` flags was 30/30 clean (the plain
rlexe flags were also 30/30); `es6IsConcatSpreadable.js` was 20/20;
`async-jit-bugs.js` default was 20/20.


One independent review also found a pre-existing engine issue this patch does
not claim to solve: a suspended generator/async frame keeps a raw
`ByteCodeReader` into bytecode that a debugger reparse replaces, and
debug-mode reparse can change layout for reasons other than profile opcodes
(`try/finally`, object literals). `-off:DynamicProfile` reproduces that
separately. It is recorded as an open engine issue, not a regression from this
port; see [Known open engine issues](#known-open-engine-issues).

### Swarm review follow-up (20:39 dylib)

A multi-lens review swarm found and verified three additional runtime issues;
all are fixed on the 20:39 binary.

- **`Function.prototype.arguments` on native frames.** The stack walker's
  `GetJavascriptArgs` returned the interpreted-frame base (`fp+40`) for a
  native JIT frame. Arguments 0-4 were correct, argument 5 read the function
  object, argument 6 read `callInfo`, and an 8-argument read SIGSEGVed.
  `JavascriptStackWalker.cpp` now detects the eight-home native frame by its
  duplicated function/callInfo header and returns the contiguous caller image
  at `fp+104`. `fargs6.js` and `fargs8.js` match the interpreter in
  `-mic`, `-forceNative`, and `-nonative` modes.
- **Decommit zero-fill did not flush the instruction cache.** ARM64's
  I-cache is not coherent with the `memset` stores. A standalone PAL probe
  showed 200/200 stale calls still returned the old function value after
  `VirtualFree(MEM_DECOMMIT)`; with `__builtin___clear_cache` it was 0/200
  stale and 200/200 SIGILL. The JIT commit and decommit zero-fill paths now
  call `FlushInstructionCache`.
- **Zero/overflow `VirtualProtect` size could open the thread-wide JIT write
  window while returning success.** A zero or wrapping `dwSize` has no pages,
  but the Apple JIT branch still toggled `pthread_jit_write_protect_np`.
  `VirtualProtect` now rejects zero/wrapping sizes before any toggle. This is
  a generic PAL guard; the JIT branch is the reason it matters.

The swarm also confirmed the earlier per-thread MAP_JIT model with a standalone
probe: `mprotect` after the first RWX transition fails with EACCES (13), a
base-RX page cannot be written, a writer thread can toggle its own window
while another thread executes the same page, and a thread created inside a
parent's window starts write-protected. Review items that were build hygiene
or coverage gaps are closed; the remaining pre-existing allocator/GC edge
cases, entitlement documentation, and latent robustness notes are listed in
the [swarm review queue](#swarm-review-queue).

### Swarm review queue

A multi-lens review swarm (architecture, ABI, memory/GC, concurrency, build,
test evidence, security, and full-diff hygiene) produced this deduplicated
queue. All P1 merge blockers and the build-system findings are closed; the
P2/P3 items below are a mix of fixed-in-working-tree, open, pre-existing, or
documented gaps. Runtime fixes landed on the 20:39 binary; build/test fixes
landed in `07d6965e8` and `2372fcc81`; the FDE/allocator/installer fixes are
in the working tree pending the next commit.

**Resolved**

- **Build-critical files were untracked.** `07d6965e8` tracks
  `lib/Backend/arm64/DarwinEhFrame.h` and this document. A clean checkout
  builds `-Dprofile=jit`.
- **The JIT `check` target did not exercise JIT code.** `zig build
  -Dprofile=jit check` now runs `tools/build/check_jit.py` (requires the
  `NativeCodeGenerator` symbol and executes a hot call/rest/apply script under
  both `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`) and
  `verify_bytecode_pure.py --jit --verify`. The generic `check-smoke` remains
  interpreter-only. `check_jit: PASS` and `Bytecode is up to date` were
  observed on the current build.
- **`Function.prototype.arguments` read past the homes on native frames.**
  Fixed on 20:39: `GetJavascriptArgs` now returns the contiguous caller image
  when the eight-home signature is present. Repros `fargs6.js` and `fargs8.js`
  match the interpreter.
- **Decommit zero-fill did not flush the I-cache.** Fixed on 20:39:
  `FlushInstructionCache` is called after the JIT commit/decommit zero-fill.
- **Zero/overflow `VirtualProtect` size opened the thread-wide JIT write
  window.** Fixed on 20:39: zero/wrapping sizes are rejected before the toggle.
- **JIT static archive was not reproducible.** Fixed in `2372fcc81`: the
  libtool step runs under `ZERO_AR_DATE=1`; `ar tv` shows the deterministic
  `Dec 31 19:00 1969` / `0/0` member metadata for the 2.2 GB archive.
- **`expect_total` was vacuous for the JIT graph.** Fixed in `2372fcc81`:
  `backend_expected` is the independent sum of
  `backend_group.{cxx,c,asm_files}.len`, asserted to be 89, and `backend_n`
  is asserted against it. The shared JIT total is 538 + 89 = 627.

**P2 - ownership, correctness, and release hardening**

- **Large-object OOM path use-after-free — fixed in the working tree.**
  `AllocLargeObject` now releases the secondary/xdata slot before
  `Release(address, pages, segment)`; for allocations above
  `maxAllocPageCount`, `Release` destroys the segment and its secondary
  allocator. The pre-fix order was a genuine UAF. The `PrependNode == nullptr`
  branch itself is still not deterministically injected.
- **Empty small-object page release — fixed in the working tree.** The
  `FreeAllocation` branch now compares against the 32 tracked chunks
  (`Page::MaxAllocationSize / Page::Alignment`), removes the page node, and
  calls `ReleasePages` under `codePageAllocators->cs`. `Page::IsEmpty` also
  uses the tracked-chunk count instead of a 64-bit all-ones test. The small
  function stress showed 2944 page allocations / 2933 empty-page releases
  with no double release; `maxFreePageCount == 0` decommits or releases the
  segment as before.
- **The custom heap fixes a 4 KiB logical page on a 16 KiB host page.**
  `CustomHeap.h` caps `Page::MaxAllocationSize` at 4096 while
  `AutoSystemInfo::PageSize` is 16384 on Apple ARM64; 12 KiB of every small
  page is unusable and the page-sized fast path cannot match a 4 KiB
  allocation. Pre-existing performance waste, not a correctness failure.
- **Completed-generator write-barrier leak — fixed in the working tree.**
  `SetState(Completed)` now unregisters the frame, `args.Values`, and
  `&args.Values` before nulling them; `SetFrame` retains the registered frame
  address in a low-bit-tagged `uintptr_t` so the JIT epilogue's direct null
  store cannot hide it. A 500-generator run measured 1500 registrations /
  1500 unregistrations and `mapCount=0` in `-ForceNative -JitES6Generators`,
  `-mic:1 -bgjit- -JitES6Generators`, and `-nonative` modes. A new
  `wbMapLock` serializes `Register`/`UnRegister` against
  `BackgroundRescan`'s `LockResize` walk. Async generators still do not
  register their argument buffers (pre-existing asymmetry), and the old
  concurrent map access remains a latent design issue outside
  `-ConcurrentRuntime`.
- **Non-Windows FDE/XData ownership — fixed in the working tree, with one
  test gap.** `CleanupXDataInfo` now deletes the standalone `HeapNewZ`
  wrapper on non-Windows x64/ARM64 (normal cleanup previously leaked it) and
  does not delete the interior `&allocation->xdata` on non-Windows ARM32.
  `OnWorkItemProcessFail` gets a defensive early unregister for a future
  post-registration failure. The `allocation`/`isAllocationCommitted` free
  branch is inert in this tree, and `-InduceCodeGenFailure` throws before
  registration, so the post-`Register` failure path is source-audited rather
  than runtime-exercised.
- **PAL API inconsistencies on JIT regions (partially fixed).**
  `VirtualFree(MEM_RELEASE)` now rejects an interior address with
  `ERROR_INVALID_ADDRESS`; the probe rejected `base+1`, `base+PG`, and
  `base+4*PG` and left the region intact. Still open:
  `VirtualAlloc(MEM_COMMIT, PAGE_READWRITE)` returns memory that is still
  write-protected for the calling thread (`VirtualQuery` says writable), and
  `VirtualProtect` queries/protects under separate lock acquisitions.
  Internal callers compensate; direct embedders remain exposed.
- **Installer symlink escape — fixed in the working tree, with TOCTOU
  residual.** The installer now rejects a symlinked `out/<profile>` root,
  rejects every existing symlink component below the prefix (including
  in-prefix aliases such as `linkdir -> real`), and requires bare
  `--symlink` targets. `tools/build/test_build_tools.py` adds nine
  temporary-fixture tests for the prefix/child/alias/orphan/target cases;
  all 57 build-tool tests pass. Validation and mutation remain separate
  syscalls, so a concurrent attacker can still race the check; there is no
  cross-process lock and stale `.tmp` files are not swept.
- **Helper calls are homed, contrary to the ABI rule in this document.**
  `LowerCall` calls `HomeJsCallArgs` for any `argCount > 0`; helper calls with
  three arguments (e.g. `HelperWMemCmp`, `HelperMemCpy`) therefore write x0-x2
  into `[sp]` and raise the outgoing arg area. Focused tests pass, but this is
  an unresolved exception to the platform-ABI rule. Either restrict homing to
  JS calls or update the contract and add a >8-arg helper test.
- **JIT backend objects are linked after SCACore, not in CMake's position.**
  The JIT dylib has a different initializer order (36 vs 32 init functions)
  than the no-JIT build. No failure observed. The `build.zig` header discloses
  this as a known deviation; matching CMake would require splitting the
  object-group link, which is not proven necessary. Accepted deviation, not
  fixed.

**P3 - open cleanup and latent robustness**

- `XDATA_SIZE` 0x100 applies to every non-Windows ARM64 target, not just
  Apple; the Linux ARM64 path does not need the larger Darwin frame.
- The interpreter thunk's pdata slot is 64 bytes while JIT xdata is 256; the
  size guard is an assert that is off in the fast profile. A longer FDE would
  truncate silently.
- `EmitDarwinArm64EhFrame` is exported from the dylib; it is backend-internal.
- `JITThunkEmitter`'s Apple branches are compiled out by `_CONTROL_FLOW_GUARD`;
  the code is future/Windows-CFG preparation, not Mac evidence.
- The pre-reserved `MEM_RESERVE_EXECUTABLE` path is dead on Apple ARM64
  (`JITOutput` forces `canAllocInPreReservedHeapPageSegment=false`).
- `DISABLE_JIT` / `ENABLE_NATIVE_CODEGEN` guards disagree for a hypothetical
  `-DDISABLE_JIT=0` build; the shipped profiles are consistent.
- Hardened-runtime hosts need `com.apple.security.cs.allow-jit`; the port and
  installer do not document or install the entitlement, and the installed
  binary is ad-hoc signed.
- ARM32 and Windows behavior are inspection-only; no build/run evidence.

**Documented gaps (not review actions)**

- The asm.js/WASM feature gap is recorded in
  [Suite results](#suite-results) and
  [Known open engine issues](#known-open-engine-issues). It needs a separate
  ARM64 port or an accepted platform exclusion. The two StackTrace line diffs
  are fixed by the ARM64 `InlineeCallInfo` legalization change; do not filter
  the remaining asm/WASM tests out of the gate.

### Focused repros (historical)

This table accumulated evidence across the port; each row is historical and
names its binary where the distinction matters. It is not a claim that every
row was rerun on the 20:39 binary. The current dylib keeps these results and
adds the per-thread write-window fix, the `Function.prototype.arguments`
caller-image fix, the decommit I-cache flush, and the zero-size
`VirtualProtect` guard. The suite table in [Suite results](#suite-results) has
the current totals.

Host is `out/jit/bin/ch`. Short scripts were physical files in the scratch
directory; the names below are ephemeral local artifacts. A single call with
`-mic:1` is still interpreted; the second call is the native one. Always pass
`-bgjit-` with `-mic:1`.

| Check | Command shape | Result |
|---|---|---|
| `test/Function/apply3.js` | `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-` | Exit 0. Output matches `apply3.baseline`. The 8-argument line appears three times. |
| `test/inlining/applyBugs.js` | Same flags. Working directory `test/inlining` (it loads `..\UnitTestFramework\UnitTestFramework.js`). | Exit 0. Six `PASSED` lines. |
| Hot `apply(arguments)` of `(2, 3, NaN, null, undefined, false, "hello", o)` after direct calls of the callee | `apply3_heat_direct.js`, same `-mic` flags | Exit 0, prints `val 2` through `[object Object]`, then `PASS`. |
| Three inlined `show.apply(this, arguments)` calls | `apply_probe.js` | Exit 0, call 3 prints every argument, `PASS`. |
| Direct 8-argument `arguments[5]` and a formal | `direct8.js` | Exit 0, `PASS`. |
| Spread of 7 arguments, 50 iterations | `test_spread.js` | Exit 0, `PASS`. |
| Caught throw, 50 iterations | `test_throw_50.js` | Exit 0, `PASS`. Still true on 12:52. |
| `new.target` for 2, 4, 5, 7, and 10 user arguments, plus `super(1+2, foo(), ...[1,2,3])` | `newtarget.js`, both the `-mic` flags and `-forceNative -off:simpleJit -bgJitDelay:0` | Exit 0, `PASS`. Count 5 must stay unbiased. Count 6 and above need the gap. |
| `test/es6/ES6Class_SuperChain.js` | `-forceNative -off:simpleJit -bgJitDelay:0 -args summary -endargs -es6generators` | Prints `pass`, exit 0. On the 12:37 dylib, test 65 failed because `new.target` was the callee. |
| `test/Strings/length.js` | `-mic` flags, and again with `-forceNative` | Exit 0. Previously a `SIGSEGV` on the 11:25 interpreted suite. |
| `es6GeneratorJit/generator-jit-bugs.js` and `async-jit-bugs.js` | Every `rlexe.xml` entry: default `-mic` flags, `-JitES6Generators`, `-JitES6Generators -off:simplejit`, `-JitES6Generators -off:fulljit`, `-off:fulljit`; also the dynapogo flags | Exit 0, `pass`. Includes 31 `BailOutForGeneratorYield` records. Default `async-jit-bugs.js` was also 20/20 clean on the 18:13 dylib. |
| `gen_min.js` (two `it.next()`) | `-mic:1 -bgjit- -JitES6Generators` | `{"value":1,"done":false}` then `{"done":true}`, exit 0. |
| `test/Operators/strictequal.js` | Exact runner flags | Exit 0, output matches baseline. Previously fatal OOM. |
| `test/es6/es6IsConcatSpreadable.js` | Exact runner flags (`-es6tolength`) plus `-forceNative -off:simpleJit -bgJitDelay:0` | Exit 0, `pass`; 20/20 clean on the 18:13 dylib. Previously fatal OOM and then `SIGBUS` 11/20. |
| `test/Object/ObjectSpread_Limits.js` | Plain rlexe flags and `-forceNative -off:simpleJit -bgJitDelay:0` | 30/30 clean in each mode on the 18:13 dylib, output `pass`. Previously `SIGBUS` 10-16 times in 30. |
| `test/DebuggerCommon/AsyncDynamicAttach.js` | Exact `-nonative` command | Exit 0 in ~0.04 s. Previously a 60 s hang. |
| `fargs6.js`, `fargs8.js` (`Function.prototype.arguments`) | `-nonative`, `-mic:1 -bgjit-`, `-forceNative -off:simpleJit -bgJitDelay:0` | All print `6:1,2,3,4,5,6` / `8:1..8`. Before the 20:39 fix, 6 args read the function source for arg 5 and 8 args SIGSEGVed. |
| Decommit I-cache probe | `VirtualFree(MEM_DECOMMIT)` then a stale call | 0/200 stale calls and 200/200 SIGILL after `FlushInstructionCache`; before the flush, 200/200 returned the old value. |

### Suite results

Run one variant at a time. The runner's `--binary` must be absolute. It
spawns `ch` with the test directory as the working directory, so
`--binary=out/jit/bin/ch` is `FileNotFoundError` for every test. The runner
prints `exit code: -N` for signal N (`-6` abort, `-9` killed by the 60-second
timeout, `-10` bus error, `-11` segfault). Those are the same events as
process status 134, 137, 138, and 139. `PYTHONUNBUFFERED=1` makes the log
live. Default `--variants` is both `interpreted` and `dynapogo`; always pass
one. On this Mac the runner still sets `arch='x64'` and applies
`exclude_x64` / `exclude_amd64`. Leave that alone.

| Variant | Binary | Log | Result |
|---|---|---|---|
| `interpreted` (`-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-`) | 13:58 dylib | `jit-interpreted-inline.log` | `passed 2367, failed 397`. All 397 failures are asm.js or WebAssembly with exit 0. Zero signals. `EH` 40/0, `TryCatch` 1/0. |
| `dynapogo` (`-forceNative -off:simpleJit -bgJitDelay:0`) | 13:58 dylib | `jit-dynapogo-inline.log`, 501.49 s | `passed 1943, failed 286`. All 286 failures are in asm.js / WebAssembly folders and exit 0. Zero signals. `apply3` and `applyBugs` are absent from the failure list. |
| `disable_jit` (`-nonative`) | 13:58 dylib | `jit-disable-inline.log` | `passed 1973, failed 0`. The gate. Do not bump it. |

All 286 dynapogo failures are the asm.js / WebAssembly baseline gap:
123 `WasmSpec`, 92 `AsmJs`, 54 `wasm`, 14 `AsmJSFloat`, 2
`WasmSpec.MultiValue`, 1 `ASMJSParser`. Every one exits 0. The earlier
`-6` debugger pile, the `es6GeneratorJit` `-11`s, and the
`Object/ObjectSpread_Limits.js` `-10` are gone from this run.

The asm.js / WebAssembly exit-0 pile is a missing compile message or
`WebAssembly is not defined`. It is not the `apply` segfault. Do not start
that port as part of this worktree; B4 remains the separate Linux x64 proof.

### Remaining gaps after the final pass

The debugger hang, the generator-JIT segfault, `ObjectSpread_Limits.js`,
`strictequal.js`, `es6IsConcatSpreadable.js`, and the two StackTrace line
diffs are closed. `interpreted` is **2367/397** (all 397 are asm.js/WASM,
zero signals); `dynapogo` is **1943/286** (asm.js/WASM only, zero signals);
`disable_jit` is **1973/0**.

The StackTrace fix is in `LegalizeMD::LegalizeLDIMM`. The 64-bit
`InlineeCallInfo` value is `(inlineeStartOffset << 32) | argCount`; the ARM64
legalizer emitted only the low two half-words for a label offset, so the
stack walker read `inlineeStartOffset == 0` for inlinees without a frame
record and attributed the parent to the inlinee's body line. The legalizer
now emits `MOVK` for bits 32–47 and 48–63 on 64-bit targets. Both
`StackTrace/ErrorPrototype.js` and `StackTrace/StackTraceLimit.js` now match
their baselines; the full interpreted suite passed 2367/397 after the fix.

The asm.js and WebAssembly failures are the documented Mac feature gap, not
compiler failures. The JIT profile is built for `aarch64-macos` and does not
define `ASMJS_PLAT` or `ENABLE_WASM` (those are x86/x64 in
`lib/Common/CommonDefines.h`), so asm.js does not print
`Successfully compiled asm.js code` and WebAssembly evaluates to
`undefined`. The folder tags `require_asmjs` and `require_wasm` are intended
to exclude these on unsupported architectures, but `test/runtests.py` pins
`arch='x64'` on non-Windows hosts; the no-JIT gate avoids the pile through
`require_backend`. Do not add an arm64-only filter or delete tags to make the
JIT run green. A native ARM64 asm.js/WASM port is a separate project.

After a compiler change, re-run `dynapogo` as the only suite on the machine,
then `disable_jit` and expect 1973/0. `install_profile.py` now replaces
artifacts atomically (`os.replace` on a same-directory temp file). That is
required on macOS: overwriting a signed `ch`/dylib in place can leave a stale
vnode signature cache and the next exec is killed with SIGKILL (exit 137)
before `main` prints anything. If `ch -version` dies with 137, replace the
inode (`rm` then `cp`) instead of debugging the engine.

### Known open engine issues

These are pre-existing and separate from the JIT merge gate. Do not present
them as fixed by this port.

- **Suspended coroutine across a debugger reparse.** A generator/async frame
  keeps a raw `ByteCodeReader` into the bytecode block that
  `FunctionBody::CleanupToReparseHelper` drops. A debug-mode reparse can also
  change bytecode layout for `try/finally`, object literals, and other
  debug-only emission decisions. Repro with `-nonative -off:DynamicProfile`
  and a generator that yields, attaches a debugger, then resumes: the result
  can be a bare value instead of an iterator result. A correct fix needs to
  retain/rebase the old FunctionBody/bytecode and its operand metadata, or
  avoid reparsing live coroutine frames. This is an ownership change, not a
  one-line guard.
- **JIT-compiled generator `arguments.callee` identity.**
  With `-JitES6Generators`, `arguments.callee` returns a function object with
  the same name and source but `!== g`; the interpreter returns the original
  `g`. `test/es6/generators-apis.js` covers this but runs without
  `-JitES6Generators`. The generator-JIT path builds the arguments object
  from a different function object. Flag-off by default; not a crash. The
  `yield*` completion probe that was also suspected reproduces under
  `-nonative` and is not attributed to this port.
- **TTD debug loop selection.** The TTD debug loop is split profiled/unprofiled
  and compile-verified, but there is no supported short TTD+debugger-action
  test in this tree. Record the gap rather than claiming it is exercised.

### Do not redo these

- `LowerCallIDynamic` still ends in `LowerCall(callInstr, 0)`
  (`lib/Backend/arm64/LowerMD.cpp` around line 506). On Apple it then stores
  the function and `callInfo` with `StoreAppleJsArgSlot` and reloads `x0`–`x7`
  with `ReloadAppleJsCallRegs` immediately before the `blr`. Passing
  `NUM_INT_ARG_REGS` into `LowerCall` runs `HomeJsCallArgs`, which stores
  whatever is currently in the registers over the stack image. By that point
  `x3` has often been reused for the raw count. Spread already stored user
  arguments 1–4 at `[sp+32]` through `[sp+56]` via `LoadDynamicArgumentUsingLength`.
  Homing the registers would overwrite that image.
- Register slots in `LoadDynamicArgument` on Apple are stores to
  `[sp + slot*8]`, including slots 0–7. The reload is what puts them back in
  `x0`–`x7`. The length loop still adds 3 on Apple so user argument 5 is
  `[sp+64]`.
- The Apple gap for a stack read lives once, in
  `Lowerer::GenerateLoadStackArgumentByIndex` (`lib/Backend/Lower.cpp` around
  line 22060). It copies the index and adds 8 when
  `index + actualOffset >= 10`, and it skips coroutines. `actualOffset` is
  `GetFormalParamOffset() + offset` (4 plus the caller's offset). `new.target`
  passes `callInfo.Count` and offset 0, so count 5 compares as 9 and stays in
  the homes (`fp+72`, `Values[5]`). Count 6 compares as 10 and lands at
  `fp+144` (`Values[6]`). A stack-argument element passes the user index with
  offset 1, so user argument 5 is the first biased load. The earlier copy in
  `GenerateFastStackArgumentsLdElemI` was removed. Putting it back applies
  the gap twice and `arguments[5]` reads the wrong word.
- `GetArgsIndirOpndForInlinee` stays unbiased. A blanket `+64` breaks a
  contiguous copy of a direct inlined call. The `0x9` in the hot `apply`
  crash was the callee's incoming `x3`, not an inlinee meta-argc load.
- `GetArgSlotSym((ArgSlot)-1)` allocates a new symbol every call. It is not
  a shared last-writer. `-off:inlineargsopt` did not stop the hot `apply`
  crash. `-off:inline` did, because that path uses the C++ `EntryApply`
  helper.
- `GenerateLoadNewTarget` must keep using `GenerateLoadStackArgumentByIndex`.
  Do not special-case "index >= 5" inside it. Count 5 is still a home.
- Do not revert the Apple `ADD 3` in `LoadDynamicArgumentUsingLength`, the
  six Unix thunks' 80-byte `x0`–`x7` save, `CallDarwinArm64Method`, or the
  SP-relative `.eh_frame` (CFA is `SP + TotalStackSize`, not FP). File-scope
  `::mac_fde_wrapper` is required. A block-scope `extern` inside
  `namespace Memory` declares `Memory::mac_fde_wrapper` and fails the link.
- Integer ARM64 register encodings are already DWARF numbers. A float encode
  is `NEONREG_BASE + n` with `NEONREG_BASE = 50`; DWARF `Vn` is
  `64 + (encode - 50)`. Do not add 64 to the raw encode. Do not edit
  `AssemblyCommon.h`.
- Spilled overflow formals at bailout (`LinearScan.cpp` around line 1228,
  `BailOut.cpp` `FinalizeOffsets`) are still open, and only if a test hits
  them. `new.target` for counts other than 2, 4, 5, 7, and 10 was not
  measured beyond the class-suite `pass`.

### Build, git, and trees

From the worktree root:

```sh
mise x -- zig build -Dprofile=jit > "$TMPDIR/chakra-jit-buildN.log" 2>&1
echo BUILD_EXIT:$? >> "$TMPDIR/chakra-jit-buildN.log"
```

Trust `BUILD_EXIT` and a following `install_profile: out/jit:` line. libtool's
`failed command:` reprint is noise on a successful link. Zig buffers the log
until the link finishes. An incremental `Lower.cpp` or `LowerMD.cpp` rebuild
has been about one to two minutes. The shell tool's own exit status is the
`echo`, which is 0 even when `zig build` failed, so read `BUILD_EXIT`.

Run `git` from the worktree root. If your shell starts in the modernization
checkout, use `git -C <worktree>`. The modernization checkout may carry
unrelated uncommitted parser work such as `lib/Parser/Parse.cpp`; leave it
alone. Do not commit, push, merge, or rebase this worktree unless asked.
Do not delete `out/jit/bin/libChakraCore.dylib`, `~/.cache/zig`, or the
worktree `.zig-cache`. `rg` is not on `PATH` in this shell; `grep` is.

`lldb --batch -s "$TMPDIR/hot.lldb" -- ch ...` works. A multi-line `script` block
in that file drops into the Python prompt and does not run. One `script`
statement per line does. JIT code addresses change every run. At an
`Add_Full` crash, the saved `fp` is the JIT callee; `[fp]` and `[fp+8]` are
the caller `fp` and `lr`. Homes start at `[jit_fp+16]`.

Asserts are off on the jit profile because `DBG` is not defined; the profile
uses `-O3 -g -UNDEBUG`, not `-DNDEBUG`. `ENABLE_DEBUG_CONFIG_OPTIONS` is on,
so `-off:` and `-trace:` names in `ConfigFlagsList.h` are real.
`-off:inline` disables all inlining. `-off:inlineapply` is `PHASE InlineApply`.
Unknown `quit()` throws. Do not test `ch` through a pipe or process
substitution: `ftell` on a FIFO is 0 and an empty script exits 0.

## 1. Where the work lives

| Tree | Path | Branch | Role |
|---|---|---|---|
| Modernization checkout | `<modernization-checkout>` | `modernize/baseline` | Interpreter and language features. JIT stays compiled out. |
| This worktree | `<worktree>` | `jit/arm64-macos` | The only tree that builds the ARM64 backend. |

Edits for this port go in the worktree only. The modernization checkout has
carried unrelated uncommitted parser work during this effort; that work is not
part of the JIT port and is not copied here. The port is committed as
`07d6965e8` plus `2372fcc81`; it is not merged.

The standing arrangement:

- New language features land on modernization. This worktree picks them up by
  rebasing onto that branch. After a rebase, a failure that also fails with
  JIT compiled out means the rebase went wrong. A failure that appears only
  when JIT is on belongs to the compiler.
- Merge this branch back only when the existing regression suite passes with
  JIT on and the interpreter run still passes. That merge can happen before
  every remaining language feature is done. It must not happen while the
  compiler is still failing those tests.
- Do not flip the modernization branch's no-JIT gate as a shortcut. Do not
  start B4, lookbehind, the `v` flag, S12, or S14 from this tree.
- Linux x64 is a later port. Apple's argument image is not the AAPCS image
  Linux ARM64 uses, and it is not the Windows x64 image. The Apple adjustments
  are wrapped in `__APPLE__` (and `_M_ARM64` where the edit is in a file that
  also builds for other architectures). They must not move to Linux with a
  global change.

## 2. What this port is trying to do

ChakraCore already has an ARM64 JIT backend, written for Windows. On Windows
the JIT calls a JavaScript function with the first eight integer arguments in
`x0`–`x7` and only the overflow on the stack. The callee homes those registers
under the frame pointer and reads both the homes and the overflow through one
formula based at the frame pointer.

Apple ARM64 does not pass variadic arguments that way. A C `va_list` on this
Mac is a `char *` (8 bytes), not the AAPCS struct. The interpreter entry thunk
(which establishes a 16-byte frame) and the JIT callee with calls (which homes
eight registers `x0`–`x7` above its 16-byte `(fp, lr)` pair) read from the caller
stack: `[function, callInfo, this, arg0, arg1, …]`, one pointer per slot,
starting at `fp + 16` for the interpreter thunk, and at `fp + 80` for the JIT
callee. Named parameters stay in `x0` and `x1`; the compiler still puts the same
values on the stack so the trampoline can walk one array.

The port makes the JIT produce that same image, and makes the callee read it
back, including arguments that do not fit in the eight homes. The acceptance
bar is the engine's existing regression suite, run so that JIT actually
compiles the tests, plus the same suite still passing interpreted. Section 6
says which runner flags do which of those. A handful of hand-written scripts
(section 5) are the evidence gathered so far. They are not a substitute for
the suite.

## 3. How to build and run

From the worktree root, with Zig 0.17 on `PATH` through `mise`:

```sh
mise x -- zig build -Dprofile=jit
```

`-Dprofile=jit` is defined in `build.zig`. It recompiles the engine with the
test profile's flags and without `-DDISABLE_JIT=1`, then adds the ARM64
backend objects from `tools/build/tables.zig`. Every translation unit in that
build must share the same `DISABLE_JIT` setting. With JIT compiled in,
`lib/Common/CommonDefines.h` turns on native codegen, profile info, the
background job processor, the dynamic interpreter thunk, and copy-on-access
arrays. Those stay off in the `test`, `debug-static`, and `release-shared`
profiles.

The profile installs to `out/jit/` and does not replace `zig-out/`. Its
`zig build check` runs the generic `check_smoke.py`, then `check_jit.py`
(native backend symbol plus a hot call/rest/apply script under both `-mic:1
-bgjit-` and `-forceNative -off:simpleJit -bgJitDelay:0`), then
`verify_bytecode_pure.py --jit --verify` (`build.zig`, the `profile == .jit`
branch). It does not run the regression suite. The 1973/0 gate stays on the
default test profile.

Useful host commands, all against `out/jit/bin/ch`:

| Command | What it does |
|---|---|
| `ch script.js` | Interprets. No `-mic` and no `-ForceNative`. |
| `ch -mic:1 script.js` | Interprets once, then JIT-compiles the hot function. Builtins inject as interpreter code. This is the acceptance mode used for `arguments` and rest. |
| `ch -mic:1 -trace:bailout script.js` | Same, and prints bailout kind and opcode. `ENABLE_DEBUG_CONFIG_OPTIONS` is on for this profile, so the flag exists. Asserts are off because `DBG` is not defined (`-O3 -g -UNDEBUG`; no `-DNDEBUG`). |
| `ch -ForceNative script.js` | JIT-compiles before running, including builtin initialization. Do not use this as the `arguments` acceptance test. See section 7. |

`ch -c` and `ch -e` are not valid. Do not pass `-RecyclerStress`. Do not run
two suites at once: they share `profile.dpl.*` files. Host `print` is the
script's output channel.

The JIT static archive is about 2.2 GB because the objects are built with
`-g`. Zig 0.17's archiver cannot write an archive that large. This profile
calls `/usr/bin/libtool -static` under `ZERO_AR_DATE=1` instead, so member
metadata is the deterministic epoch/0:0. libtool prints `has no symbols` for
a few objects and then reprints the whole command line under `failed command:`.
That reprint is noise when the build then prints `install_profile: out/jit:`
and exits 0. A new archive content hash is a new file under `.zig-cache/o/`,
not an overwrite of the previous 2.2 GB archive. Keep the installed dylib.
Do not delete `.zig-cache` object trees to make room unless a real `ENOSPC`
is in hand; `df -h` on the data volume is the free-space check that has
matched reality.

PAL configure for this profile reports `95/95 checks match the contract`.
Do not edit the PAL contract to silence a mismatch.

## 4. The Apple frame, measured on this Mac

Stack grows down. For a callee that has calls (a normal JavaScript function
that reads `arguments`, calls anything, or has a try), `ARM64StackLayout` in
`lib/Backend/arm64/LowerMD.cpp` homes all eight integer registers. The saved
frame-pointer and link-register pair is 16 bytes, and the frame pointer points
at the saved frame pointer. The caller's outgoing area, which is the argument
image, therefore starts at `fp + 80`.

High addresses at the top:

```text
fp+152   caller image slot 9    = user argument 6
fp+144   caller image slot 8    = user argument 5
fp+104   caller image slot 3    = user argument 0     <-- contiguous from here
fp+96    caller image slot 2    = this
fp+88    caller image slot 1    = callInfo
fp+80    caller image slot 0    = function            <-- old SP, start of image
fp+72    homed x7               = user argument 4
fp+64    homed x6               = user argument 3
fp+56    homed x5               = user argument 2
fp+48    homed x4               = user argument 1
fp+40    homed x3               = user argument 0
fp+32    homed x2               = this
fp+24    homed x1               = callInfo
fp+16    homed x0               = function
fp+0     saved fp               <-- FP points here
         saved lr
         ... callee-saved regs, locals, outgoing args ...
         final SP
```

Slot numbers count from the image: slot 0 is the function object, slot 1 is
`callInfo`, slot 2 is `this`, slot 3 is the first user argument. A call
`print(1, 2, 3, 4, 5, 6)` is 9 slots. A call of a function with 7 user
arguments is 10 slots. `callInfo` for `nth(1, 2, 3, 4, 5, 6, 7)` is
`0x2000008`: count 8 in the low bits (`this` plus 7 user arguments) and flag
bit 25 (`0x2000000`). Treating that word as a JavaScript value is the usual
signature of a load that fell into the caller image's `callInfo` slot.

The first five user arguments happen to sit at the same frame-pointer offsets
the Windows backend computes. `Lowerer::GetFormalParamOffset()` in
`lib/Backend/Lower.h` returns 4. Code in `Lowerer` that builds an
`arguments[i]` address calls that function, so the Windows formula is
`[fp + (index + 5) * 8]`. On this frame that is correct for indexes 0 through
4 (`arguments[0]` at `fp+40`, `arguments[4]` at `fp+72`) and wrong after that:

| Index | Windows formula | What is actually there | Contiguous image address |
|---|---|---|---|
| 0–4 | `fp + (i + 5) * 8` | homed `x3`–`x7`, the right values | `fp + 104 + i * 8` |
| 5 | `fp+80` | function object | `fp+144` |
| 6 | `fp+88` | `callInfo` `0x2000008` | `fp+152` |

There is a second `GetFormalParamOffset`. `LowererMD::GetFormalParamOffset()`
in `lib/Backend/arm64/LowerMD.cpp` returns 2, with a comment that the encoder
accounts for the saved frame pointer and link register. Stack-argument code
inside `Lowerer` methods does not call that one. Do not change one to match
the other.

Named formals are a different path from `arguments[i]`. The encoder in
`BaseAndOffsetFromSym` (`lib/Backend/arm64/EncoderMD.cpp`) adds
`NUM_INT_ARG_REGS * MachRegInt` (64) when a parameter-slot symbol's offset is
already at least 64. User argument 5 is offset 64, so the sixth named
parameter reads the caller image. That is why a function whose body is
`return f`, with six named parameters, returns 6 under `-ForceNative`.
Inlining argument slots are not parameter-slot symbols and are not shifted.
An inlinee's argv is a contiguous copy, not the Darwin incoming image.

`LoadStackArgPtr` (non-coroutine, non-loop-body) already adds that same 64
bytes, so its pointer is `fp+104`. A second bias on that pointer would skip
the first user argument. `LoadInputParamCount` and `LoadArgumentCount` read
the homed `callInfo` at `fp+24` and must stay there. Coroutine frames take the
generator-args pointer and return before the Apple bias. Loop bodies home
fewer registers and bail out through `BailOutFromLoopBody`; they are not
covered by the `fp+80` picture above.

`JavascriptCallStackLayout::FromFramePointer` was left unshifted on purpose.
`ToFramePointer` is its inverse, and bailout frame-height math subtracts from
`(uint8*)ToFramePointer(layout)`. Shifting `FromFramePointer` globally would
break that. `Arm64StackFrame::GetArgv` also stays unshifted: the interpreter
thunk only does `stp fp, lr` and then exposes argv at `fp+16`, and the stack
walker uses `GetArgv` for the function, `callInfo`, and `this` only.

Who writes the image:

- `LowererMD::HomeJsCallArgs` (`LowerMD.cpp`) stores the first
  `min(argCount, 8)` register arguments at `[sp + slot * 8]` before a
  JavaScript call. `LowerCall` calls it on Apple when `argCount > 0`.
- `GetOpndForArgSlot(..., jsStackImage: true)` places a stack argument at
  `[sp + slot * 8]` as well, so slot 8 is at `[sp + 64]` rather than at
  `[sp + 0]`. `LowerCallArgs` passes `jsStackImage true`.
- Helper calls generally use `LowerCall(..., 0)` and are not homed. Their ninth
  integer argument stays at `[sp + 0]`, which is the platform ABI. Do not home
  helper calls. `LowerCall` nevertheless homes any call with `argCount > 0`,
  so `HelperWMemCmp` (`lib/Backend/Lower.cpp:24564`) is a known exception; see
  the P2 item in the review queue.
- `arm64_CallFunction` (`lib/Runtime/Library/arm64/arm64_CallFunction.S`)
  writes `[function, callInfo, values…]` at the new stack pointer, leaves
  `x0`/`x1`, and reloads `x2`–`x7` only for slots the copy wrote. It must not
  load past the allocation. `ldr` there does not clobber `NZCV`.

Dynamic inlined calls (e.g. `func.apply(this, arguments)`) in the JIT backend
(`LoadDynamicArgument`, `LoadDynamicArgumentUsingLength`, and
`LowerCallIDynamic`) still use the old overflow rule and call
`LowerCall(callInstr, 0)`. Section 7.1 discusses the proved root cause of
`sum(...arguments)` (the C++ `CALL_ENTRYPOINT` calling convention mismatch) as
well as the secondary audit needed for these backend dynamic lowering methods.
Do not attempt to modify these lowering methods without following Section 7.1.

## 5. What is in the worktree, and what was measured

### 5.1 Edits

These are committed on `jit/arm64-macos` in `07d6965e8`; `2372fcc81` adds the
JIT check gate, the deterministic archive, and the independent backend-count
assert.

Build and install:

- `build.zig` adds `-Dprofile=jit`, drops `-DDISABLE_JIT=1` for that profile
  only, installs to `out/jit/`, and makes `zig build check` run the generic
  smoke plus a JIT symbol/hot-call smoke and a `--jit` bytecode verify.
- `tools/build/tables.zig` adds the ARM64 backend group on top of the existing
  object groups. The backend group is not a member of `object_groups`. The
  base shared/static object counts stay 538/536; the JIT graph adds 89
  backend TUs and asserts 627 shared.
- `tools/build/install_profile.py` knows the `jit` install prefix.

Executable JIT memory:

- `pal/src/include/pal/virtual.h`, `pal/src/map/virtual.cpp`, and
  `lib/Common/Memory/VirtualAllocWrapper.cpp` reserve custom-heap segments
  with `MEM_RESERVE_EXECUTABLE`, which PAL creates with `MAP_JIT`.
  `MEM_RESERVE_EXECUTABLE` is a PAL allocation flag defined in
  `pal/inc/pal.h` for every target; it is not an Apple-only concept. The Apple
  ARM64 branch adds it to the PAL reservation; the Windows PAL path keeps a
  plain `MEM_RESERVE`. Recycler and ordinary heap allocations keep the normal
  mapping. The first commit of
  each JIT page performs the one `mprotect` Apple Silicon accepts
  (`PROT_READ|PROT_WRITE|PROT_EXEC`) and zero-fills under a per-thread write
  window. After that, `VirtualProtect` on a JIT region never calls `mprotect`:
  `PAGE_READWRITE`/`PAGE_EXECUTE_READWRITE` call
  `pthread_jit_write_protect_np(0)` and `PAGE_EXECUTE_READ` calls it with 1.
  `VIRTUALJitWriteProtect` is the raw-write pair used for `FillDebugBreak` and
  the `.eh_frame` slot. `FlushInstructionCache` already calls
  `__builtin___clear_cache`.
- Decommit cannot revoke a MAP_JIT page after the one-way RWX transition: a
  second `mprotect` (including `PROT_NONE`) returns `EACCES` (13), and
  `mmap(MAP_FIXED|MAP_JIT)` cannot replace the mapping. `madvise(MADV_DONTNEED)`
  is advisory and a no-op for a partially live segment on this host, so PAL
  zero-fills every committed page under a `VIRTUALJitWriteProtect(TRUE/FALSE)`
  window before calling `madvise` as a hint. Stale calls hit zeros and trap.
  This contains stale entry points but does not physically release a partially
  live segment; whole-segment release still uses `munmap`.
- `JITThunkEmitter` is compiled out on this Mac by `_CONTROL_FLOW_GUARD`. Its
  `PAGE_READWRITE`/`PAGE_EXECUTE_READ` pair is future/Windows-CFG preparation,
  not evidence exercised by this port. The live write paths are
  `EmitBufferManager::CommitBuffer`, `InterpreterThunkEmitter`,
  `CustomHeap::FreeAllocation`, and the XData writes.

Call image and the reads that consume it:

- `lib/Backend/arm64/LowerMD.h` and `LowerMD.cpp`: `HomeJsCallArgs`, the
  `jsStackImage` arm of `GetOpndForArgSlot`, the Apple bias in
  `LoadStackArgPtr`, and the Apple bias in `LoadInputParamPtr`.
  `LoadInputParamPtr` loads the address of the homed first user argument
  (`fp+40`) and adds 64, returning the add, so `LoadHeapArguments` walks a
  contiguous argv at `fp+104`. The coroutine arm returns first and is
  unchanged. Inlinee paths use `GetInlineeArgvSlotOpnd` and are not biased.
- `lib/Backend/arm64/EncoderMD.cpp`: parameter-slot symbols whose offset is
  at least 64 gain another 64, in both the stack-pointer form and the
  frame-pointer fallback. That is what makes named parameters past the eighth
  slot correct.
- `lib/Runtime/Library/arm64/arm64_CallFunction.S`: reload `x2`–`x7` from the
  image the copy just wrote.
- `lib/Backend/SimpleJitProfilingHelpers.cpp`: `ProfileParameters` moves its
  layout pointer 8 pointers past `FromFramePointer` on Apple ARM64, then
  profiles `layout->args` as usual. Other `Profile*` helpers stay on
  `FromFramePointer`.
- `lib/Backend/BailOut.cpp`: `BailOutCommonNoCodeGen` keeps
  `FromFramePointer` for `layout->functionObject` and `layout->callInfo`, and
  passes `layout->args + 8` to `ArgumentReader` on Apple ARM64. The
  interpreter's `m_inParams` is that pointer. `BailOutHelper` runs
  `Process` synchronously, and the pointer is only valid until the JIT frame
  returns. The inlinee `ArgumentReader` is unchanged because an inlinee's
  argument vector is a contiguous copy. Elided-yield reads `args[0]` and
  `args[1]` only and was left alone. Loop-body bailout does not use
  `ArgumentReader` on `layout->args`.
- `lib/Backend/Lower.cpp`: `GetArgsIndirOpndForTopFunction` adds 64 bytes when
  the constant index is at least 5, and for a runtime index copies the index,
  branches with `BrLt_A` when it is below 5, and otherwise adds 8 to the copy
  so the scaled address gains 64 bytes. Generators are excluded. This is the
  path a warmed `arguments[i]` takes when `GlobOpt::OptArguments` marks the
  load `usesStackArgumentsObject`. It does not go through `LoadHeapArguments`.
- `lib/Backend/Lower.cpp`: `GenerateLoadStackArgumentByIndex` (around line
  22060) is the single Apple gap for both `new.target` and fast stack
  arguments. It adds 8 to a copy of the index when
  `index + actualOffset >= 10`. The call-site copy that used to live in
  `GenerateFastStackArgumentsLdElemI` is gone. See the handoff section.
- `lib/Backend/arm64/LowerMD.cpp`: `LoadDynamicArgument` stores Apple register
  slots to `[sp + slot*8]`. `LowerCallIDynamic` still calls
  `LowerCall(callInstr, 0)`, then `StoreAppleJsArgSlot` for the function and
  `callInfo`, then `ReloadAppleJsCallRegs`. That reload is the hot
  `apply(arguments)` fix. Do not replace it with `HomeJsCallArgs`.
- `lib/Backend/Lower.cpp`: `LowerRestParameter` adds the same 64 bytes for a
  non-generator. Rest needs one contiguous element pointer. The homes are
  contiguous only through user argument 4; the next word is the caller
  image's function pointer. The caller image at `fp+104` is contiguous for
  every user index, so the rest pointer always uses it. A rest that starts at
  user argument 4 and also takes argument 5 is the case that proved a
  conditional bias was the wrong shape: the first element was correct and the
  second element was the function object.

Unwind registration, DWARF `.eh_frame` through `mac_fde_wrapper`:

- `lib/Common/Memory/arm64/XDataAllocator.cpp` and
  `lib/Backend/PDataManager.cpp` **register** the emitted CIE+FDE through the
  file-scope `mac_fde_wrapper` (`__register_frame`/`__deregister_frame`) on
  Apple ARM64; they are not no-ops. Section 7.2 has the encoding and the
  SP-relative CFA. `ARM64UnwindEncoder` still emits Windows ARM64 unwind codes
  for the non-Apple path; the comment in `PDataManager.cpp` that "ARM64 still
  emits Windows unwind codes" is therefore true only for non-Apple ARM64.
  `lib/Common/PlatformAgnostic/AssemblyCommon.h` still
  calls `__register_frame` on its ARM64 branch and was left unchanged: editing
  that header rebuilds the world, and AMD64 still uses `mac_fde_wrapper`.

Builtin bytecode for the JIT-enabled build:

- `lib/Runtime/Library/InJavascript/JsBuiltIn.bc.32b.h` and
  `JsBuiltIn.bc.64b.h` were regenerated so this `ch` embeds the JIT-variant
  builtin bytecode. `ByteCodeCacheReleaseFileVersion.h` was not modified.
  The no-JIT headers were not regenerated. Regenerating these two files is
  not starting B4. Do not copy them back onto the modernization branch as if
  they were the no-JIT baseline. `verify-bytecode` on the test profile passes
  `--noJit` and checks the no-JIT bytes.

### 5.2 Ownership

These edits do not allocate a new heap object and do not change who frees
one. The outgoing argument image is space in the calling JIT frame. The
biased pointers in `LoadInputParamPtr`, `LoadStackArgPtr`,
`ProfileParameters`, and the bailout `ArgumentReader` are addresses into the
live caller stack. `LoadHeapArguments` copies the values into a heap
arguments object during the helper; it does not keep the stack pointer.
`BailOutHelper` borrows the same image only while the interpreter runs inside
the still-live JIT frame. `LowerRestParameter` hands the element pointer to
`OP_NewScArrayWithElements`, which copies into a new array before it returns.
The five questions in [memory-management.md](memory-management.md) were
applied to each of those borrows. The native Zig example in that guide does
not implement this engine's GC.

### 5.3 Measurements on the 22:16 dylib

Each warmed run is `out/jit/bin/ch -mic:1`. The interpreter column is the
same `ch` with no JIT flag. Programs are small enough to recreate; they are
not checked-in tests.

Interpreter `arguments`. A function prints `arguments.length` and then
`arguments[0]` through `arguments[5]` for a call of one argument and a call
of several. Output:

```text
4
1
5
6
7
8
9
```

Warmed constant index, the case that used to die in `AddLeftDead` with the
right-hand value `0x2000008`:

```javascript
function nth(a, b, c, d, e, f, g) {
    return arguments[0] + arguments[4] + arguments[5] + arguments[6];
}
function two(a, b) {
    return arguments[0] + arguments[1];
}
```

Called 40 times as `nth(1, 2, 3, 4, 5, 6, 7)` and `two(8, 9)`. Output `19`
then `17`. Before the `GetArgsIndirOpndForTopFunction` bias, `-trace:bailout`
showed two `BailOutIntOnly` records at `FromVar` and then a segmentation
fault in `JavascriptMath::AddLeftDead` with `x1 = 0x2000008`.

Warmed variable index. The same `nth` sums `arguments[i]` for `i` from 0
through 6. Output `28` then `17`.

Rest, warmed, including a second JIT function that calls `rest5`:

```javascript
function rest0(a, ...r) { return r.length + r[0] + r[1]; }       // rest0(8, 9, 10)        -> 21
function rest4(a, b, c, d, ...r) { return r[0] + r[1]; }          // rest4(1..6)            -> 11
function rest5(a, b, c, d, e, ...r) { return r[0] + r[1]; }       // rest5(1..7)            -> 13
function span(a, ...r) { return r[0] + r[4] + r[5]; }             // span(1..7)             -> 15
function tail(a, b, c, d, e, ...r) { return r[0]+r[1]+r[2]+r[3]; } // tail(1..9)           -> 30
function empty(a, ...r) { return r.length; }                      // empty(1)               -> 0
function caller() { return rest5(1, 2, 3, 4, 5, 6, 7); }          //                        -> 13
```

Output `21 11 13 15 30 0 13`, interpreter and `-mic:1`. Before the rest
bias, `rest4` returned the string `"5"` concatenated with the function's
source (first rest element correct, second element the function object), and
`rest5` and `span` died in `ProfiledLdElem` on `0x2000008`. `rest0` already
returned 21 because both of its elements sit inside the homes.

Other calls that returned the right values on this dylib:

| Program | Mode | Output |
|---|---|---|
| `print(1, 2, 3, 4, 5, 6)` | `-ForceNative` | `1 2 3 4 5 6` |
| sums of 5, 6, and 7 user arguments, then the 6th and 7th named parameters | `-ForceNative` | `15`, `21`, `28`, `6`, `7` |
| `sum.apply(undefined, [1..7])` and `two.apply(undefined, [8, 9])`, warmed | `-mic:1` | `28`, `17` |
| `new Foo(1..7)`, `new Bar(8, 9)`, `new Foo(1..10)`, storing `new.target === Foo` and `a + last` on `this` | `-mic:1` | `8`, `1`, `17`, `1` and `11`, `1` |

`new` discards a primitive return value, so the `new.target` check has to be
stored on `this`. `Function.prototype.apply` from an interpreted caller goes
through the runtime helper, which already speaks the Darwin image. That
result does not prove a JIT-lowered dynamic call. Spreading `arguments`
exercises invocation of JIT callees via C++ runtime helpers. That path passes
on the 10:21 dylib; section 5.4 is the fix.

### 5.4 Thunk saves and Darwin `CALL_ENTRYPOINT` (2026-10-17)

Two entry paths were still handing a JIT callee a garbage `x2`.

The six Unix assembly transition thunks saved only `x0` and `x1` across their
C++ helper call. That call clobbers `x2`–`x7`. The Windows copies in
`arm64_Thunks.asm`, `arm64_DeferredParsingThunk.asm`, and
`arm64_DeferredDeserializeThunk.asm` already allocate 80 bytes and save
`x0`–`x7`. The Unix files now match them:

- `lib/Runtime/Language/arm64/arm64_Thunks.S` (four thunks)
- `lib/Runtime/Library/arm64/arm64_DeferredParsingThunk.S`
- `lib/Runtime/Library/arm64/arm64_DeferredDeserializeThunk.S`

`stp`/`ldp` pairs sit at `[sp, #16]`, `[sp, #32]`, `[sp, #48]`, and
`[sp, #64]`. The prolog offset is `-80` and the epilog offset is `80`.
`add x0, sp, #16` is unchanged: the saved function pointer is still the
first slot after `fp`/`lr`. `CheckCodeGenThunk` in
`lib/Backend/arm64/Thunks.S` already saved `x0`–`x7` and the double
registers, so it was left alone.

`CALL_ENTRYPOINT_NOASSERT` on Apple, in `lib/Runtime/Language/Arguments.h`,
called `JavascriptMethod` with only `function` and `callInfo` named. Darwin
puts variadic arguments on the stack and does not copy them into `x2`–`x7`.
The interpreter thunk reads the stack image and worked. A JIT callee homes
`x2` and treated leftover register contents as `this`. The Apple branch is
now `CallDarwinArm64Method`. It names eight parameters so the first six real
arguments fill `x2`–`x7`, and it passes `function`, `callInfo`, and the real
arguments again as the variadic tail. That tail is still the stack image
`[function, callInfo, arg0, …]`. Unused register slots are null. The Linux
ARM64 macro stays the null-filled form: AAPCS already uses `x0`–`x7`, and
those nulls are what pushes the image onto the stack there.

Measured on the 10:21 dylib, scripts written to real files:

| Program | Command | Result |
|---|---|---|
| `function foo(a, b) { return a + b; } print(foo(2, 3));` | `-ForceNative` | `5`, exit 0 |
| `fwd` returns `sum(...arguments)` of `1..7`, 50 times, prints `PASS` only if every result is 28 | no flag, and `-mic:1 -bgjit-` | `PASS`, exit 0 |
| warmed constant `arguments` and the rest script from section 5.3 | `-mic:1 -bgjit-` | `19`/`17` and `21 11 13 15 30 0 13` |

`-bgjit-` is required. A short loop without it can finish in the interpreter
before the background compiler installs native code. Do not feed `ch` a
pipe or process substitution: `ftell` on that handle is 0, the script is
empty, and the process exits 0.

### 5.5 Second-pass fixes on the 16:37 dylib

These are the compiler and allocator fixes that closed the remaining
non-asm suite failures. They are part of the committed port (`07d6965e8`);
none is a language batch or a B4 change.

#### Debugger attach under `-nonative`

`DynamicProfileInfo::IsEnabled_OptionalFunctionBody` used to enable profile
info for debug-mode functions even when `IsNoNative()` was true. A debugger
attach reparses source; `FunctionBody::CleanupToReparseHelper` replaces the
bytecode block. A suspended async/generator frame keeps a raw
`ByteCodeReader` with the old offsets, so enabling profile opcodes at attach
changed the bytecode length and the async stepper never saw completion.

The predicate now keeps profile info off for `-nonative` regardless of debug
mode. The debug interpreter loop is generated twice,
`ProcessWithDebuggingProfiled` (only when `ENABLE_PROFILE_INFO`) and
`ProcessWithDebuggingUnprofiled`; `ProcessWithDebugging()` and
`ProcessWithDebugging_PreviousStmtTracking()` choose with
`DoInterpreterProfile()`. `ShouldDoProfile` and the asm.js reparse path use
the same predicate, so profile storage and the selected loop agree. The TTD
debug loop has the same profiled/unprofiled split. Do not re-add the dead
`IsInDebugMode()` arm to the profile predicate.

#### No-JIT link of the Apple unwind path

`lib/Common/Memory/arm64/XDataAllocator.cpp` is in the common object graph
even with `DISABLE_JIT=1`. Its Apple `mac_fde_wrapper` references were gated
only on `__APPLE__`; `AssemblyCommon.cpp` defines the wrapper only when JIT
is compiled in. The Apple branch is now gated on `ENABLE_NATIVE_CODEGEN`.
`PDataManager.cpp` and `UnwindInfoManager.cpp` are backend-only and stay
unguarded. The default test profile links again; this is required for the
B2/B3 no-JIT gate.

#### Apple JIT write windows (superseded by the third pass)

The second pass removed the failing
`VirtualProtect(newprot=0x40, unix=7, errno=13)` request by using
`PAGE_READWRITE` for the write window in
`EnsureAllocationExecuteWriteable`, `Heap::FreeAllocation`'s partial-free
restore, and the `JITThunkEmitter` pair. That fixed the `EACCES` abort but
was still a **process-wide `mprotect`**, so an already-installed function on
the same page became non-executable for other threads and a concurrent
`-forceNative` run took SIGBUS. The third pass replaces that mechanism with
`pthread_jit_write_protect_np` on real MAP_JIT segments; see the handoff
section. `VirtualProtect` no longer mprotects a committed JIT page, and
`PAGE_READWRITE`/`PAGE_EXECUTE_READ` are now per-thread toggles.

#### ARM64 concat overflow flag

`Lowerer::InsertAddWithOverflowCheck` emitted `ADD` (because callers passed
`needFlags=false`) and then `BVC`. x86 `ADD` sets flags; ARM64 `ADD` does not,
so the branch consumed stale `V`. Multi-operand string concat then took its
fatal OOM path in `SetConcatStrMultiItem`. The helper now always emits a
flag-setting add; `Lower.h` lost the misleading parameter. This is shared
lowering, so it also removes the latent bug on Windows ARM64 and ARM32.

#### Generator JIT epilogue null-out

`InsertNullOutGeneratorFrameInEpilogue` runs after register allocation and
creates its own operands. x64/x86 assigned a caller-saved register; ARM64 did
not, so `RegNOREG` encoded as x15 and the null store became
`mov x15,#0; str x15,[x15,#0x20]`. The second `it.next()` faulted at 0x20.
The epilogue now uses x12 for the generator address and stores XZR directly
on ARM64. ARM32 has no zero register, so it materializes zero into r3 first;
r3 is caller-saved and is not r12 (the ARM32 SCRATCH_REG, which holds the
generator address here). The generator completion signal is still a null
frame pointer; `GeneratorEpilogueNoFrameNullOutLabel` is unchanged.

#### Installer atomic metadata

`tools/build/install_profile.py` used `shutil.copy2` over an existing
artifact. macOS caches a code signature per vnode; an in-place-overwritten
signed `ch` or dylib can be killed with SIGKILL (137) on the next exec. The
installer now copies to a same-directory temp file and `os.replace`s it, so
the destination gets a fresh inode. The `.manifest` and `.profile` files are
written through the same temp+`os.replace` path, so a crash cannot truncate
one. The compare-and-skip no-op path is unchanged. If a stale inode still
kills `ch -version`, delete and re-copy the host from `.zig-cache`.

## 6. How the suite corresponds to "JIT on"

`tools/build/check_regression.py` always runs:

```text
test/runtests.py --test --variants=disable_jit --binary=<ch>
```

and requires `Total: passed 1973, failed 0`. The `disable_jit` variant's host
flag is `-nonative` (`test/runtests.py`). `zig build check` on the default
test profile is that gate. `zig build -Dprofile=jit check` does not run the
regression gate; it runs the JIT smoke and bytecode verify described in
section 3. Pointing the official gate at `out/jit/bin/ch` still passes
`-nonative`, so a green result there says the interpreter inside the
JIT-configured binary worked. It says nothing about the compiler.

The runner's other two variants are the ones that compile:

| `--variants` | Host flags | Historical meaning |
|---|---|---|
| `interpreted` | `-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-` | Interpret once, then full JIT. Closest to the `-mic:1` checks in section 5. Excludes tests tagged `require_disable_jit`, and tests tagged `fails_interpreted` or `exclude_interpreted`. |
| `dynapogo` | `-forceNative -off:simpleJit -bgJitDelay:0` | Compile everything before execution, including builtin initialization. Excludes `require_disable_jit` and the `fails_dynapogo` / `exclude_dynapogo` tags. |
| `disable_jit` | `-nonative` | The 1973/0 gate. Excludes `exclude_interpreted`, `fails_interpreted`, and `require_backend`. |

On non-Windows the runner sets its own `arch` variable to `x64` even on this
Mac (`test/runtests.py`). That is the selection the 1973/0 number was counted
with. A JIT run uses the same runner, the same `--test` flavor, and the same
tag rules. Do not invent an arm64-only filter, do not delete `exclude_*` or
`fails_*` tags to make a red run green, and do not bump 1973 unless a new
`rlexe` file is added on the modernization branch. Do not run a second suite
in parallel.

Both JIT variants consume and write `profile.dpl.*`. Run one at a time. The
runner also sets `TZ=America/Los_Angeles`.

A throw is a separate gate from the variant flag. On the 11:25 dylib an
interpreted `throw` is catchable: `out/jit/bin/ch "$TMPDIR/test_throw.js"` and
`out/jit/bin/ch "$TMPDIR/test_throw_50.js"` both print `PASS`. Section 7.2 has
the `-mic:1` and `-forceNative` results. The `disable_jit` suite has since been
re-run repeatedly on the current JIT-enabled binary; the final 13:08 gate is
`1973 passed / 0 failed` (`jit-disable-inline.log`). The no-JIT `ch` from
the modernization checkout remains the rebase oracle. Do not treat the throw
scripts as a substitute for `1973` passed and `0` failed.

## 7. Root causes, fixes, and remaining work

### 7.1 JIT spread of `arguments` and C++ `CALL_ENTRYPOINT` on Apple DarwinPCS

**Status:** Fixed on the 2026-10-17 10:21 dylib. The reproduction in section
5.4 prints `PASS` under `-mic:1 -bgjit-`. The root cause below is kept so
the register recipe is not re-broken. `LowerCallIDynamic` was not this
crash; it is still the secondary check at the end of this section.

Reproduction script:

```javascript
function sum(a, b, c, d, e, f, g) {
    return a + b + c + d + e + f + g;
}
function fwd() {
    return sum(...arguments);
}
var i = 0;
var w = 0;
while (i < 5) {
    print(i);
    w = fwd(1, 2, 3, 4, 5, 6, 7);
    print(w);
    i = i + 1;
}
```

On the 22:16 dylib with `-mic:1 -bgjit-` the first iteration prints `0` and `28`.
Around iteration 2 (or iteration 150 without `-bgjit-`), the process dies with
a segmentation fault at address `0x0000000000000009`.

#### Previous Hypothesis (Disproved)
The previous draft hypothesized that `sum(...arguments)` lowering in
`LowererMD::LowerCallIDynamic`, `LoadDynamicArgument`, or `LowerSpreadCall`
was placing arguments at the wrong stack offset or omitting `HomeJsCallArgs`.
Inspection of lowering and `lldb` shows `LowerSpreadCall` is not where this crash
occurs.

#### Proved Root Cause
1. `fwd` is JIT-compiled. When it executes `sum(...arguments)`, it calls the
   runtime helper `Js::JavascriptOperators::OP_LdCustomSpreadIteratorList`
   (`lib/Runtime/Language/JavascriptOperators.cpp:1597`).
2. `OP_LdCustomSpreadIteratorList` retrieves `arguments[Symbol.iterator]`,
   which resolves to the builtin `Array.prototype.values`.
3. It invokes `Array.prototype.values` via C++:
   ```cpp
   CALL_FUNCTION(threadContext, function, CallInfo(Js::CallFlags_Value, 1), aRight);
   ```
   where `aRight` is `this` (`arguments`).
4. Before the fix, `CALL_ENTRYPOINT_NOASSERT` in
   `lib/Runtime/Language/Arguments.h` expanded to
   `entryPoint(function, callInfo, function, callInfo, ##__VA_ARGS__)`.
   The Apple ARM64 branch now routes through `CallDarwinArm64Method`
   (`lib/Runtime/Language/Arguments.h:115-126`). `entryPoint` has C++ type
   `JavascriptMethod` (`Var (*)(RecyclableObject*, CallInfo, ...)`).
5. **Apple ARM64 Calling Convention (`DarwinPCS`):**
   Under DarwinPCS, variadic C functions pass ONLY the named parameters in
   registers (`x0 = function`, `x1 = callInfo`). **All variadic arguments**
   (`function`, `callInfo`, `aRight`) are placed on the caller's stack at
   `[sp, #0]`, `[sp, #8]`, `[sp, #16]`. Clang does **NOT** load `aRight` into `x2`!
6. When `Array.prototype.values` was interpreted, the interpreter trampoline
   read `this` from the stack (`fp + 32` / `fp + 96`), so it succeeded.
   **However, when `Array.prototype.values` becomes JIT-compiled**, its entrypoint
   prolog homes `x2` to `[fp, #32]` (`ARM64StackLayout::HomedParams`) and assumes
   `this` is in register `x2`.
7. Because the C++ `JavascriptMethod` call never loaded `x2`, `x2` contained
   leftover register garbage from C++ (`x2 = 0x1`).
8. Inside JIT-compiled `Array.prototype.values`, `Object(this)` invoked
   `JavascriptObject::NewInstance(0x1)`, which attempted to read `0x1->type` and
   crashed at memory offset `0x9` (`SIGSEGV`)!
9. **Scope:** This defect affected *every* runtime call site using
   `CALL_FUNCTION` or `CALL_ENTRYPOINT` in
   `lib/Runtime/Language/JavascriptOperators.cpp`,
   `lib/Runtime/Language/InterpreterStackFrame.cpp`,
   `lib/Runtime/Language/JavascriptProxy.cpp`,
   `lib/Runtime/Library/RegexHelper.cpp`,
   `lib/Runtime/Library/JavascriptConversion.cpp`, and
   `lib/Runtime/Library/GlobalObject.cpp`, whenever the callee was
   JIT-compiled.

#### Fix that landed
`CallDarwinArm64Method` in `lib/Runtime/Language/Arguments.h:115-126` replaces
only the Apple `#else` under `#elif defined(_ARM64_)`. The Linux null-filled
macro is unchanged. The template expands the argument pack into six register
slots padded with null, then calls a function type with eight named
parameters and the original stack image as the variadic tail. An empty
argument list still fills `x2`–`x7`. The earlier loop-and-`passed[]` sketch
is not what was compiled.

#### Secondary check, closed on the 12:52 dylib

`LowererMD::LowerCallIDynamic` was the hot `apply(arguments)` crash, and it
is fixed by the store-and-reload described in the handoff. The cold
`apply_probe.js` path passed before that reload existed, because the
register cascade happened to survive until the `blr`. A hot caller reused
`x3` for the count. `LoadDynamicArgumentUsingLength` still adds 3 on Apple.

### 7.2 A `throw` has to be catchable (Unwind / Status 134)

**Status:** Caught on the 2026-10-17 11:25 dylib (historical). `mise x -- zig
build -Dprofile=jit` logged `BUILD_EXIT:0` in `chakra-jit-build20.log`, and
`out/jit/bin/libChakraCore.dylib` was replaced (25MB). These physical scripts
printed `PASS` and exited 0:

- `out/jit/bin/ch "$TMPDIR/test_throw.js"` (one call, no flags)
- `out/jit/bin/ch "$TMPDIR/test_throw_loop.js"` (`iter 0` through `iter 3`,
  then `PASS`)
- `out/jit/bin/ch -mic:1 -bgjit- "$TMPDIR/test_throw_loop.js"`
- `out/jit/bin/ch -forceNative -off:simpleJit -bgJitDelay:0
  "$TMPDIR/test_throw.js"`
- `out/jit/bin/ch -forceNative -off:simpleJit -bgJitDelay:0
  "$TMPDIR/test_throw_loop.js"`

`test_throw_50.js` (50 calls, prints `PASS` only) also exited 0 with no flags,
with `-mic:1 -bgjit-`, and with
`-forceNative -off:simpleJit -bgJitDelay:0`. The regression suite was not run
on that dylib.

The FDE that landed is SP-relative. `arm64_CallEhFrame` sets `fp` to the parent frame, so an FP-based CFA would skip `OP_TryCatch`. CFA is `SP + TotalStackSize` after the prolog, and saved registers are byte offsets from that SP. The try thunk's first store is `stp d8, d9, [sp, #-240]!` so the 240-byte register area exists. `XDataAllocator::Register` calls the global `::mac_fde_wrapper`. A block-scope `extern` inside that method declared `Memory::mac_fde_wrapper` and was the build19 link failure (`undefined symbol: __ZN6Memory15mac_fde_wrapperEPKcPFvPKvE`).

The notes below are the pre-implementation review. The historical FP-based
sketch later in this section is not the encoding that was built.

Review notes from before implementing the recipe:

- The dynamic interpreter thunk really is `stp fp, lr, [sp, #-16]!` then
  `mov fp, sp` (`InterpreterThunkEmitter.cpp`, the non-Windows
  `InterpreterThunk` bytes). The `fp+16` CIE/FDE in this section matches
  that thunk. It does not match a JIT function that homes eight registers:
  there the saved `fp`/`lr` sit at `[fp]` and `[fp+8]`, and the caller's SP
  is `fp+80`.
- The thunk block reserves `sizeof(RUNTIME_FUNCTION)` at the end of the
  executable page. That slot is too small for a CIE plus an FDE. Apple
  `__register_frame` reads the record later, so the bytes must stay in
  readable memory. A `MAP_JIT` page that is no longer readable is the wrong
  place to leave them.
- This note is historical: when it was written, `mac_fde_wrapper` was compiled
  only for AMD64 (`AssemblyCommon.cpp`). It now compiles for `_M_ARM64` too. On
  ARM64, `__REGISTER_FRAME` is raw `__register_frame`. Apple registers one FDE
  at a time; a pointer to the CIE (id 0) does not register the following FDE.
  Do not edit `AssemblyCommon.h`. A local declaration of the wrapper, with the
  `.cpp` built for `_M_ARM64` as well, is the registration path.

Reproduction script (must be saved to a real `.js` file, e.g.
`$TMPDIR/test_throw.js`):

```javascript
function inner() { throw 1; }
function outer() {
    try { inner(); } catch (e) { return e; }
}
var res = outer();
if (res === 1) { print("PASS"); } else { print("FAIL: " + res); }
```

#### Proved Root Cause
1. **Interpreted `throw` fails on `out/jit/bin/ch` before the unwind fix:**
   The `jit` profile compiles with `-D_ENABLE_DYNAMIC_THUNKS=1`
   (`build.zig:295`). When dynamic thunks are enabled, script function
   invocations go through dynamically generated code blocks in
   `lib/Backend/InterpreterThunkEmitter.cpp:177-215`:
   ```arm64
   stp  fp, lr, [sp, #-16]!
   mov  fp, sp
   ...
   blr  x1
   ```
   `InterpreterThunkEmitter::GeneratePdata` only generated a Windows
   `RUNTIME_FUNCTION` struct. At the time, `PDataManager::RegisterPdata`
   (`lib/Backend/PDataManager.cpp:73-83`) was a complete no-op
   (`(void)functionStart;`). It now registers the CIE+FDE through
   `mac_fde_wrapper`.
   When `throw 1` executes in JavaScript, C++ throws `Js::JavascriptException`.
   Apple's `libunwind` walks up the C++ call frame from
   `InterpreterStackFrame::Process`, hits the return address in the
   dynamically allocated `InterpreterThunk` memory block, finds no registered
   `.eh_frame` FDE, and aborts with:
   `libc++abi: terminating due to uncaught exception of type Js::JavascriptException`
   (status 134 / `SIGABRT`).
2. **JIT function `throw` fails before the DWARF path:**
   `lib/Backend/arm64/ARM64UnwindEncoder.cpp` generates Windows ARM64 `.xdata`
   opcodes. The Apple ARM64 branch in
   `lib/Common/Memory/arm64/XDataAllocator.cpp:165-176` now registers the
   emitted CIE+FDE through `mac_fde_wrapper`; before the fix it skipped
   registration because `libunwind` cannot parse Windows `.xdata`.
3. **Files involved.** `AssemblyCommon.h` is at
   `lib/Common/PlatformAgnostic/AssemblyCommon.h` (wrapper in
   `lib/Runtime/PlatformAgnostic/Platform/Unix/AssemblyCommon.cpp`).

#### Historical fix-strategy sketch (FP-based; not what landed)
The original plan was for Apple `libunwind` on ARM64 macOS to accept DWARF
`.eh_frame` records registered via `__register_frame` using the existing
`mac_fde_wrapper` helper in `AssemblyCommon.cpp`. This sketch is retained
because it documents the DWARF frame model; the SP-relative FDE described above
is what was built. Standard ARM64 DWARF unwind encoding:
- DWARF register numbers for ARM64: `fp` = 29, `lr` = 30, `sp` = 31.
- CIE:
  - Code alignment factor = 1, Data alignment factor = -8, Return address reg = 30.
  - Initial instruction: `DW_CFA_def_cfa (31, 0)`.
- FDE for standard frame pointer functions:
  - `DW_CFA_advance_loc (4)` (past `stp fp, lr`)
  - `DW_CFA_def_cfa (29, 16)`: CFA is `fp + 16`.
  - `DW_CFA_offset (29, 2)`: `fp` is saved at `CFA - 16` (`[fp]`).
  - `DW_CFA_offset (30, 1)`: `lr` is saved at `CFA - 8` (`[fp + 8]`).

1. For `InterpreterThunkEmitter`:
   Emit a single uniform CIE+FDE record covering the entire `BlockSize` of the
   thunk buffer, and register it via `mac_fde_wrapper(pdataStart, __register_frame)`
   in `PDataManager::RegisterPdata`.
2. For JIT code:
   In `XDataAllocator::Register`, emit a DWARF CIE+FDE record describing the function's
   code range and register it via `mac_fde_wrapper(xdataInfo->address, __register_frame)`.

### 7.3 One suite run, after those two

Run a single variant at a time, from the worktree:

```sh
mise x -- python3 test/runtests.py --test --variants=interpreted \
    --binary="$(pwd)/out/jit/bin/ch"
```

That is the compiler run that matches the `-mic:1` evidence. The binary path
has to be absolute; `$(pwd)` produces one when the command runs from the
worktree root. The runner spawns `ch` with the test directory as cwd, so
`--binary=out/jit/bin/ch` fails every test with `FileNotFoundError`.

The 19:09 run (historical, `jit-interpreted-repair3.log`, 52.15 seconds; the
pre-repair `jit-interpreted-jitwrite.log` took 45.03 seconds) measured the same
totals as the current 13:58 run (`jit-interpreted-inline.log`):

```
Total: passed 2367, failed 397
```

`EH` passed 40/0 and `TryCatch` passed 1/0. All 397 failures are asm.js or
WebAssembly with exit 0 (the missing `Successfully compiled asm.js code`
message or `WebAssembly is not defined`). Zero tests exited on a signal.

`test/Function/apply3.js` matches its baseline on the 12:37 dylib with
default inlining (`-maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit-`,
exit 0). The 8-argument `apply(arguments)` line is printed three times.
`test/inlining/applyBugs.js` prints six `PASSED` lines and exits 0.
`test/Strings/length.js` exits 0 under those flags and under `-forceNative`.
The crash was the outgoing `CallIDynamic` image: user args 0–4 were moved
into x3–x7 before callInfo and the entry point were set up, and the count
was left in x3. On Apple those values are stored in the argv image at
`[sp + slot*8]` and x0–x7 are reloaded from that image immediately before
the `blr`. `LowerCall` is still called with `argCount == 0`, so
`HomeJsCallArgs` does not store the registers back over that image.

`-ForceNative foo(2, 3)` already prints `5`. Dynapogo on the 12:37 dylib,
before the `new.target` load change (`chakra-jit-dynapogo.log`, about 494
seconds), was `passed 1916, failed 313`. Of those failures, 286 exited 0: 285
were asm.js or WebAssembly baseline failures, and 1 was a non-asm exit-0
baseline failure (`ASMJSParser`). The other 27 exited on a signal: 22
`SIGABRT` (`-6`, mostly debugger tests, including `typedarray/samethread.js`
inside a long DataView log), 4 `SIGSEGV` (`-11`, both `es6GeneratorJit`
files), and 1 `SIGBUS` (`-10`, `Object/ObjectSpread_Limits.js`). That list is
historical. On the 13:58 dylib the same suite is `passed 1943, failed 286`;
all 286 are in asm.js/WASM folders and exit 0, so the signal count is zero
(`jit-dynapogo-inline.log`, 501.49 s). The 19:09 run had the
same totals (`jit-dynapogo-repair3.log`, 488.15 s). `Function/apply3.js` and
`inlining/applyBugs.js` are not in the failure list.

`es6/ES6Class_SuperChain.js` failed one case on that dylib:
`super(1+2, foo(), ...[1,2,3])` saw `new.target` as the callee. The load
uses `GenerateLoadStackArgumentByIndex` with `callInfo.Count`. On Apple that
address is a home while `index + actualOffset < 10` and steps eight pointers
into the caller image after that, so count 5 stays put and count 6 and above
reach `new.target`. The separate stack-args bias at the call site was removed
so the gap is applied once. On the 12:52 dylib, direct `new` of 2, 4, 5, 7,
and 10 arguments and that `super` spread all print `PASS` under `-mic:1
-bgjit-` and under `-forceNative`.

`disable_jit` on the current 20:39 binary is `passed 1973, failed 0`
(`jit-disable-inline.log`). The earlier 1972/1 run on the 12:52
dylib missed only `DebuggerCommon/AsyncDynamicAttach.js`, which printed
`PASSED` and then hung inside `ScriptContext::DebugProfileProbeThunk`; the
second-pass `DynamicProfileInfo`/debug-loop split is the fix (see
[Second-pass fixes](#second-pass-fixes-on-the-1637-dylib)). The modernization
checkout's no-JIT `ch` remains the rebase oracle. The expected total is still
1973/0; do not treat the historical 1972/1 run as the current gate.

Triage from the `Total:` line and the failing file names:

- A test that fails `disable_jit` on this binary and also fails on the no-JIT
  `ch` is a rebase or language problem.
- A test that fails `disable_jit` only on this binary is the JIT-configured
  interpreter (dynamic thunk, copy-on-access arrays, builtin bytecode), not
  the register allocator.
- A test that passes `disable_jit` and fails `interpreted` or `dynapogo` is
  the compiler.

Do not update the 1973/0 expectation to match a red JIT run.

### 7.4 Historical leftovers and remaining gaps

#### 1. `-ForceNative` crash in `InlineCache::TryGetProperty` under `InjectJsBuiltInLibraryCode`

**Status:** Fixed on the 2026-10-17 10:21 dylib. `-ForceNative` of
`foo(2, 3)` prints `5` and exits 0. The thunk shape below is what landed.

**Proved root cause:**
There are 6 assembly transition thunks across:
- `lib/Runtime/Language/arm64/arm64_Thunks.S`:
  1. `InterpreterStackFrame::DelayDynamicInterpreterThunk`
  2. `DynamicProfileInfo::EnsureDynamicProfileInfoThunk`
  3. `ScriptContext::ProfileModeDeferredParsingThunk`
  4. `ScriptContext::ProfileModeDeferredDeserializeThunk`
- `lib/Runtime/Library/arm64/arm64_DeferredParsingThunk.S`:
  5. `JavascriptFunction::DeferredParsingThunk`
- `lib/Runtime/Library/arm64/arm64_DeferredDeserializeThunk.S`:
  6. `JavascriptFunction::DeferredDeserializeThunk`

All 6 Unix ARM64 thunks were originally written for the interpreter before JIT
register-passing conventions existed. They allocate only 32 bytes
(`PROLOG_SAVE_REG_PAIR_INDEXED fp, lr, -32`) and only save/restore `x0, x1`
(`stp x0, x1, [sp, #16]` / `ldp x0, x1, [sp, #16]`).
When they call their C++ helpers (`EnsureDynamicInterpreterThunk`,
`EnsureDynamicProfileInfo`, `DeferredParse`, etc.), the C++ call clobbers
caller-saved registers `x2`–`x7` (`this` and `arg1`–`arg5`)!
Under `-ForceNative` (and in `dynapogo`), functions like `functionBuiltins` are
called through these thunks with `x2 = undefined`, `x3 = engineInterfaceObject`.
The thunk C++ call clobbers `x3` to `0x0`. When `functionBuiltins` jumps via `br x16`
into its JIT-compiled entrypoint, it expects `engineInterfaceObject` in `x3`,
reads `0x0`, and crashes at `ProfiledLdFld` → `InlineCache::TryGetProperty(instance = 0x0)`!

The Unix thunks now match that Windows frame. Pairs are stored at
`[sp, #16]`, `[sp, #32]`, `[sp, #48]`, and `[sp, #64]` (that last pair is
`x6` and `x7`). `add x0, sp, #16` still addresses the saved function.

#### 2. Spilled overflow formals at bailout
In `lib/Backend/arm64/EncoderMD.cpp:1511-1536`, when an `IsParamSlotSym` has
offset >= 64, the encoder adds `NUM_INT_ARG_REGS * MachRegInt` (+64 bytes) to
reach the caller's stack image above the homed parameter area.
However, in `lib/Backend/BailOut.cpp:273-302`,
`BailOutInfo::FinalizeOffsets` calculates:
```cpp
int offset = -(offsets[i] + StackSymBias);
```
without adding the Apple 64-byte offset for `IsParamSlotSym` when slot >= 8.
Similarly, `lib/Backend/LinearScan.cpp:1228` records
`*offset = -((int)stackSym->m_offset + BailOutInfo::StackSymBias);`.
When a function with more than 5 user arguments has an overflow parameter spilled
and encounters a bailout, `FinalizeOffsets` must apply the matching +64 byte bias
if `stackSym->IsParamSlotSym()` and `offset >= 64`.

#### 3. `GenerateLoadStackArgumentByIndex`

**Status:** The Apple gap landed here on the 12:52 dylib. Threshold is
`index + actualOffset >= 10`. Count 5 for `new.target` stays a home. User
argument 5 and `new.target` at count 6 and above step into the caller image.
Do not add a second bias at the `GenerateFastStackArgumentsLdElemI` call site.
A tagged-index path that never reaches this function is still unchecked.

#### 4. Linux x64 JIT
Not this merge. The Apple `+ 64` and the `[sp + slot * 8]` image are Apple-specific.

## 8. Decisions already made

These are easy to "simplify" into a bug. Each one was measured.

- `FromFramePointer` stays where it is. Bailout frame height uses the inverse.
- `GetArgv` stays where it is. The interpreter thunk's argv is `fp+16`, and
  the walker only reads function, `callInfo`, and `this` through it.
- Inlinee argument vectors and `GetArgsIndirOpndForInlinee` stay unbiased.
  They are a contiguous copy.
- The inlinee `ArgumentReader` in `BailOut.cpp` stays unbiased. The shift is
  only in `BailOutCommonNoCodeGen`.
- Elided-yield keeps reading `layout->args[0]` and `args[1]`.
- `LoadStackArgPtr`'s existing 64-byte bias is not applied again.
- `LoadInputParamCount` stays on the homed `callInfo`.
- Helper calls are intended to stay on the platform ABI. Only JavaScript calls
  should be homed. The `HelperWMemCmp` call at `lib/Backend/Lower.cpp:24564`
  currently passes `argCount == 3` and is homed anyway; that is the open P2
  exception, not a second rule.
- `AssemblyCommon.h` stays out of this change to avoid triggering a global
  rebuild. DWARF unwind registration invokes `mac_fde_wrapper` directly from
  `PDataManager.cpp` and `XDataAllocator.cpp`.
- `MAP_JIT` is for JIT code pages only. On Apple ARM64 the base mapping is
  committed RWX once (the only shape the OS accepts for a writable MAP_JIT
  page); `VirtualProtect` does not mprotect it again, and the write/execute
  view is the calling thread's `pthread_jit_write_protect_np` state. Do not
  replace this with process-wide `PAGE_READWRITE`/`PAGE_EXECUTE_READ`
  mprotect windows: they made already-installed code non-executable and
  produced the `ObjectSpread_Limits.js` SIGBUS race. Decommit zero-fills the
  page under a write window; `madvise` is advisory and does not physically
  release a partially live MAP_JIT segment.
- `InsertAddWithOverflowCheck` always emits a flag-setting add. The branch
  after it consumes V; ARM/ARM64 `ADD` does not set flags.
- The generator epilogue null-out assigns an explicit caller-saved register
  (x12 ARM64, r12 ARM32) and stores XZR/zero. Do not leave a post-regalloc
  `RegOpnd` without a physical register.
- The Apple `mac_fde_wrapper` references in `XDataAllocator.cpp` are gated on
  `ENABLE_NATIVE_CODEGEN`; the file is in the no-JIT common graph, while
  `AssemblyCommon.cpp` defines the wrapper only with JIT. Do not remove the
  guard or the default test profile will not link.
- `install_profile.py` replaces artifacts and metadata with a same-directory
  temp plus `os.replace`. Do not overwrite a signed `ch`/dylib in place on
  macOS; a stale vnode signature cache can kill the next exec with SIGKILL
  (137).
- The no-JIT builtin bytecode headers and
  `ByteCodeCacheReleaseFileVersion.h` stay as the modernization branch has
  them.
- The base object counts stay 536 static / 538 shared. The JIT graph
  independently asserts 89 backend TUs (`backend_expected`), so its shared
  total is 627; do not fold the backend count into the base baseline.
- No kill switch, no `-RecyclerStress`, no second suite in parallel, no
  commit unless the maintainer asks, no push, no merge, and no rebase unless
  modernization has moved.
- `LowerCallIDynamic` keeps `LowerCall(callInstr, 0)` and reloads `x0`–`x7`
  from the Apple argv image. It does not call `HomeJsCallArgs`.
- The Apple index gap lives only in `GenerateLoadStackArgumentByIndex`.
- The 1973/0 `disable_jit` expectation is unchanged.

## 9. Operational protocol and guardrails

These are the operational rules, diagnostic maps, and verification procedures
for this worktree. They apply to maintainers and coding agents alike; they are
not tied to a particular assistant.

### 9.1 Critical footguns to avoid

> [!CAUTION]
> **FOOTGUN 1: Never test `ch` using bash process substitution `<(printf '...')` or pipes.**
> In `bin/ch/Helpers.cpp:LoadScriptFromFile`, file size is measured using `fseek(file, 0, SEEK_END)`
> and `ftell(file)`. On `/dev/fd/N` (FIFO / named pipe), `ftell` returns `0`. `ch` will read 0 bytes,
> execute an empty script, print nothing, and exit with code `0`.
> A non-interactive run can falsely report a test as "passed" when it was never executed.
> **RULE:** ALWAYS write test code to a physical file on disk (e.g. `$TMPDIR/test_jit.js`) before invoking `ch`.

> [!CAUTION]
> **FOOTGUN 2: Never run short loops without `-bgjit-` for JIT verification.**
> Background JIT compilation is asynchronous. In loops with fewer than ~200 iterations, the script will
> finish entirely inside the interpreter before the background JIT compilation thread has finished
> generating and installing native code. The test will exit with code 0 without exercising native code!
> **RULE:** ALWAYS pass `-bgjit-` when testing JIT execution (e.g., `ch -mic:1 -bgjit- $TMPDIR/test_jit.js`),
> and ALWAYS include an explicit check that prints `"PASS"` at the end, asserting that stdout contains `"PASS"`.

> [!CAUTION]
> **FOOTGUN 3: Never modify files outside `<worktree>`.**
> Do not touch the modernization checkout `<modernization-checkout>`.
> Do not commit, do not push, and do not rebase unless explicitly asked.

### 9.2 Signal and crash diagnostics

When a test crashes or exits non-zero, use this table to identify the category:

| Exit Code | Signal | Root Cause / Meaning | Immediate Remediation |
|---|---|---|---|
| **134** | `SIGABRT` | Uncaught C++ exception / `libunwind` failure. C++ `throw` occurred in a thread without a registered DWARF `.eh_frame` FDE. | Check dynamic interpreter thunk registration (`InterpreterThunkEmitter.cpp`) or JIT code unwind registration (`XDataAllocator.cpp`). |
| **138** | `SIGBUS` | Apple Silicon W^X permission fault (`EXC_BAD_ACCESS` / `KERN_PROTECTION_FAILURE`). | `MAP_JIT` page was modified with the calling thread's write window closed, or that thread executed while its window was open. Check `VIRTUALJitWriteProtect` pairing and `pthread_jit_write_protect_np`; a process-wide `mprotect` window is the old race. |
| **139** | `SIGSEGV` | Memory segmentation fault. | Inspect faulting address in `lldb`: <br>• `0x0` or `0x8`: Null pointer dereference (e.g., clobbered `x2..x7` register). Check assembly thunks.<br>• `0x1` or `0x9`: Invalid JS `Var` (e.g., `CALL_ENTRYPOINT_NOASSERT` variadic argument failure under DarwinPCS).<br>• Stack address: Misaligned stack pointer (`sp % 16 != 0`). Apple ABI strictly mandates 16-byte SP alignment at all function call boundaries. |

### 9.3 Verification checklist

These stages are retained as a reproduction order. All four have been run;
do not restart at stage 1 unless a regression reopens that area. Stages 1 and
2 were done on the 10:21 dylib; section 5.4 has those results. Stage 3 was
done on the 11:25 dylib and still passed on 12:52; section 7.2 has the
commands. Stage 4 totals are in
[Current state and handoff](#current-state-and-handoff).

#### Stage 1: Fix Assembly Transition Thunks (§7.4 Bullet 1) — done
1. Edit:
   - `lib/Runtime/Language/arm64/arm64_Thunks.S`
   - `lib/Runtime/Library/arm64/arm64_DeferredParsingThunk.S`
   - `lib/Runtime/Library/arm64/arm64_DeferredDeserializeThunk.S`
   Save `x0`–`x7` (frame size 80) across C++ helper calls.
2. Build: `mise x -- zig build -Dprofile=jit`
3. Test:
   ```sh
   cat << 'EOF' > "$TMPDIR/test_forcenative.js"
   function foo(a, b) { return a + b; }
   print(foo(2, 3));
   EOF
   out/jit/bin/ch -ForceNative "$TMPDIR/test_forcenative.js"
   ```
   **Pass Criteria:** Outputs `5` and exits with code `0`. (Previously crashed with `SIGSEGV` in `InlineCache::TryGetProperty`).

#### Stage 2: Fix C++ `CALL_ENTRYPOINT` for JIT Callees (§7.1) — done
1. Edit `lib/Runtime/Language/Arguments.h` to use `CallDarwinArm64Method` template casting to 8 named parameters.
2. Build: `mise x -- zig build -Dprofile=jit`
3. Test:
   ```sh
   cat << 'EOF' > "$TMPDIR/test_spread.js"
   function sum(a, b, c, d, e, f, g) { return a + b + c + d + e + f + g; }
   function fwd() { return sum(...arguments); }
   for (var i = 0; i < 50; i++) {
       var res = fwd(1, 2, 3, 4, 5, 6, 7);
       if (res !== 28) { print("FAIL: " + res); quit(1); }
   }
   print("PASS");
   EOF
   out/jit/bin/ch -mic:1 -bgjit- "$TMPDIR/test_spread.js"
   ```
   **Pass Criteria:** Prints `PASS` and exits with code `0`. (Previously crashed at iteration 2 with `SIGSEGV` at `0x9`).

#### Stage 3: Implement ARM64 `.eh_frame` Unwind Registration (§7.2) — done
1. Emit a CIE+FDE for the dynamic interpreter thunk and register it with the global `mac_fde_wrapper` from `PDataManager::RegisterPdata`.
2. Emit an SP-relative CIE+FDE for each JIT function and register it from `XDataAllocator::Register`. The declaration of `mac_fde_wrapper` is at file scope. A declaration inside the method names `Memory::mac_fde_wrapper` and does not link.
3. Build: `mise x -- zig build -Dprofile=jit` (`BUILD_EXIT:0`,
   `chakra-jit-build20.log`).
4. Measured on the 11:25 dylib (historical), all exit 0 and print `PASS`:
   - `out/jit/bin/ch "$TMPDIR/test_throw.js"`
   - `out/jit/bin/ch "$TMPDIR/test_throw_50.js"`
   - `out/jit/bin/ch -mic:1 -bgjit- "$TMPDIR/test_throw_50.js"`
   - `out/jit/bin/ch -forceNative -off:simpleJit -bgJitDelay:0
     "$TMPDIR/test_throw_50.js"`
   A single `-mic:1` call is still interpreted. The 50-iteration script is the
   one that enters the native frame. The 10:21 dylib aborted 134 on the same
   catch.

#### Stage 4: Regression suites

`interpreted` on the 13:58 dylib is 2367/397 (all 397 asm.js / WebAssembly,
exit 0, zero signals; `jit-interpreted-inline.log`). `dynapogo` on the 13:58 dylib is
1943/286 (all 286 failures are in asm.js/WASM folders and exit 0, zero
signals; `jit-dynapogo-inline.log`, 501.49 s). `disable_jit` on the same binary
is 1973/0 (`jit-disable-inline.log`). The 19:09 runs (`jit-interpreted-repair3.log`,
`jit-dynapogo-repair3.log`, and `jit-disable-repair3.log`) are the prior object
set; they showed the same totals. The absolute binary and the one-variant rule
are in the handoff section. Those runs closed the `es6GeneratorJit` segfaults
and `ObjectSpread_Limits.js`; the per-thread write-window fix is described in
the handoff. The only remaining JIT failures are the documented
asm.js/WASM feature gap; do not filter it out of the gate. Re-measure
`apply3`, `newtarget.js`,
`test_throw_50.js`, and `test_spread.js` after any lowering or allocator
change.

