# E1 diagnosis tooling — agent-startup SIGBUS

Read-only diagnostics for the agent-startup SIGBUS diagnosed in
`doc/e1-agent-startup-sigbus.md`. Nothing here is wired into the build or
test gates; these are evidence-collection tools.

- `sigbus-trace.c` — DYLD_INSERT_LIBRARIES interposer. Interposes
  `munmap`/`mmap`/`mprotect`/`vm_allocate`/`vm_deallocate` into a ring; on
  SIGBUS/SIGSEGV prints siginfo, registers, backtrace, the PAL PCMI
  region walk (symbol offsets embedded for the S6e test-profile dylib;
  override with SIGBUS_TRACE_IMAGE/PVMOFF/CTOFF/MGROFF), card-table
  mapping state, and the engine's X64WriteBarrierCardTableManager diag
  fields. SIGTERM dumps a summary (driver kill). Opt-in probe wave
  (SIGBUS_TRACE_PROBE=1) calls exported OnThreadInit on 700 threads to
  validate card-section commits for secondary-thread stacks.
  Build: `clang -arch arm64 -O2 -dynamiclib -o sigbus-trace.dylib
  sigbus-trace.c && codesign -s - sigbus-trace.dylib`.
- `rvs.mjs` — churn driver (adapted from the S6e reviewer's rvb.mjs):
  `node rvs.mjs <pre|predbg|post|postdbg> <killms> <agents> <count> <par>
  [interp]`. Bins live in the (git-ignored) `out/sigbus/bin-*` dirs of the
  lane worktree. Note: any `*dbg` label symbolicates against the pre-fix
  debug image's offsets — harmless for fault counting, but do not interpose
  dbg legs (symbolication would use the wrong image).
- `cs-hammer.c` — mutual-exclusion hammer for the PAL CriticalSection
  exported from libChakraCore.dylib: `./cs-hammer <dylib> <threads>`.
- `lldb-catch.sh` — retry-loop lldb capture with OnSegmentAlloc/OnThreadInit
  breakpoints (breakpoints bind late on this dlopen-based launcher; kept for
  reference).
- `nq_16.js` — sharp 16-agent no-Quit churn script (rvs.mjs regenerates
  per-agent-count variants).
