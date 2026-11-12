# W4 differential receipt: dynapogo + interpreted wasm (darwin/arm64)

Date: 2026-11-12. Binary: `out/jit/bin/ch` (FullJIT-forced lane:
`-ForceNative -off:simpleJit -bgJitDelay:0`), post copysign +
bounds-check (UBFX zero-extend) fixes. Commit lineage: `f021f2fe4` + 3
uncommitted files (`lib/Backend/Lower.cpp`,
`lib/Backend/arm64/LowerMD.{h,cpp}`).

## Result

- `misc.js`, `memory.js`: FAIL (dynapogo, pre-fix) -> PASS post-fix,
  byte-identical stdout to `-nonative` interpreter lane. Root causes:
  ARM64 `Copysign_A` lowering absent (shared SSE path x86/x64-only);
  wasm bounds-check 32-bit wraparound on negative indices (x64
  zero-extension assumption does not transfer to ARM64).
- Remaining dynapogo failures, each reproduced under `-nonative`
  interpreted lane (59 passed / 8 failed overall), i.e. NOT JIT
  regressions — pre-existing / P1-P3 scope:
  `atomics_test`, `badfuncformat`, `oom` x4, `polyinline`, `reload`.
- `debugger_basic` (3 entries, manifest `wasm[36-38]`, phase=deferred,
  `require_debugger`): deterministic SIGABRT under forced FullJIT, exit 0
  under `-nonative`. Module/call replication without the debugger
  machinery passes FullJIT; bare `debugger;` passes both tiers. Single
  passing run out of ~10 was a timing flake (async Attach/detach vs
  background JIT). Needs x64 oracle to separate debugger-harness issue
  from ARM64 JIT bug. Out of P1/P2 gate scope (manifest phase=deferred).
- Triage hygiene: never invoke `ch` with `-dbgbaseline:<path>` against an
  in-tree baseline — the flag rewrites the file (observed CRLF->LF
  rewrite of `test/wasm/debugger_basic.js.dbg.baseline`, restored via
  `git checkout`). Copy baselines to `/tmp` for direct runs.
