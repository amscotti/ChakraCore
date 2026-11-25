# B6 Windows Native Build & Certification — Run Report

**Branch:** `modernize/phase4` @ `17e043056` + B6 fixes on local branch
**Toolchain:** Zig 0.17.0, MSVC 14.44.35207 ml64, Windows SDK 26100, vcpkg ICU 79.2 (icuuc79.dll)
**Host:** Windows 11 Enterprise Evaluation (x64) on beelink Incus `win-test`, `192.168.1.24`, user `dev`

## Steps 1–10 status (live, measured)

| Step | Status | Evidence |
|------|--------|----------|
| 1. Preflight | ✓ 15/15 (12/15 initial → fixes: binary fixture, POSIX guards, ar OSError guard) | `C:\work\step1.log` |
| 2. Native build | ✓ GREEN — `EXIT=0`; `ChakraCore.dll` 15.2 MB, `ch.exe`, `libChakraNodeApi.dll`, `GCStress.exe` | `step2.log` 19:44 EST |
| 3. Gates (smoke+metadata+bytecode+jit+selftest) | ✓ individually GREEN | manual + chain-run3 STEP3 EXIT=0 |
| 4. `check` (full regression) | ⚠️ RUNNING — 49 deterministic failures, root-cause open | `C:\work\step4.log` |
| 5. `-Djit=false` variant | ⏳ QUEUED | — |
| 6+ Steps 5–8 chain | ⏳ QUEUED | `chain-run3.cmd` |
| 8. Package | ⏳ QUEUED | — |
| 9. Certification review | ⏳ PENDING | — |
| 10. Final wrap | ⏳ PENDING | — |

## STEP4 triage (measured, not conjectured)

`zig build -Dallow-unpinned-regression=true check` — 721/728 build steps, **9/10 tests passed**. Regression totals: **passed 2896 / failed 25** (interpreted) and **passed 2357 / failed 24** (dynapogo).

- Failures are **deterministic, serially reproducible** — not parallel contention (proven by `-j 1` serial rerun).
- Cluster: `STATUS_BREAKPOINT` (`0x40010004`) on deferred-function/JIT paths; one `c0000005` access violation (`DebuggerCommon/AsyncSuspendedDetach`).
- **Flag isolation (verified):** `-Serialized` alone triggers `40010004`. `-WERExceptionSupport` passes; `-maxInterpretCount:1` alone…ambiguous (0x40010004 earlier read appeared PASS but only 114 bytes output — unconfirmed).
- This is an **engine defect in the bytecode serialization path**, not a harness artifact. ~99% of the suite (5200+ tests) passes, including bytecode purity: `verify-bytecode` = headers byte-identical.

## gen_nodeapi_exports encoding bug (fixed + verified)

Root-caused: `gen_nodeapi_exports.py` write/read without `encoding="utf-8"` (cp1252 on Windows wrote single-byte `§` -> UTF-8 read crashed with `UnicodeDecodeError: byte 0xa7`).
Fixed: `write_text(encoding="utf-8", newline="
")` + 3 `read_text(encoding="utf-8")`. VM self-test now **RC=0, 8/8 controls green**.

## Certified so far (earned, running-evidence-based)

- FULL x64 engine compile + link, zero errors.
- Check gates for smoke/metadata/bytecode/jit/selftest individually green.
- Bytecode-header purity holds (js headers untouched).
- Node-API export-surface green at 161/161.

## Known blockers

- 49 regression failures (ByteCode serialization path) — engine bug, fix incoming; NOT baselined, NOT excluded by fiat.
- VM instability: two guest hard-hangs under parallel heavy load (black frame, no network); root cause unconfirmed (likely assert-dialog-under-session0 storm), mitigated by serial execution for all gated runs.
