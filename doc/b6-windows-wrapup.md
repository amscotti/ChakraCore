# B6 Windows native bring-up — wrap-up

Branch: **`b6/windows-native-bringup`** · Base: **`modernize/phase4`** @ `17e04305`
Run record: `doc/b6-windows-run.md` · Fix log: `doc/b6-windows-fixes.md` (fix entries 1–17 + pending debug-CRT work)

## Result

`zig build` (Zig 0.17.0) to a **native Windows x64 (`x86_64-windows-msvc`) ChakraCore** (throwaway Win11 VM, `192.168.1.24`, MSVC 14.44 + SDK 26100 + vcpkg ICU 79):

| Step | Status |
|---|---|
| Step 1 pre-flight (tooling gates) | ✅ green after fixes |
| Step 2 default-profile native build | ✅ green (1262 steps) |
| Step 3 smoke/metadata/fingerprint/bytecode gates | ✅ EXIT=0 |
| Step 4 big regression `check` (49 tests) | ❌ **deterministic failures** — see open item below |
| Step 5a nojit gates | ✅ EXIT=0 (bytecode + metadata + smoke + gcstress) |
| Step 5b debug-static gates | ❌ blocked at `ch.exe` link — **debug-CRT link injection unfinished** (see below) |
| Step 6 wasm/fulljit gates | ⏸ not yet run |
| Steps 7–8 packaging/check-install | ⏸ not yet run |

## The 10 functional fixes actually landing in this branch

1. Python fixture CRLF bug (`check_metadata_linux.py` self-test SHA).
2. Python POSIX-only test guards (`test_build_tools.py`).
3. `ar` OSError guard for Windows.
4. `extern "C" errno_t rand_s(unsigned int*)` declaration in `CommonPal.h` (win32 branch).
5. Dummy `JITClient.h` MIDL never materializes — guard `#include "JITClient.h"` with `_M_ARM`.
6. SDK 26100 `winrt\` headers missing from zig include path.
7. `lib/Parser` not in `ch_group.extra_includes` — needed only by static-profile `TestHooks.cpp`.
8. `#pragma optimize("g", on)` — `-Wno-ignored-pragmas` scoped suppression.
9. `-DNOMINMAX` compile flag, and `rand_s` explicit declaration.
10. PAL-only identifiers on Windows: `PAL_wcslen`/`PAL_wcscmp` → `wcslen`/`wcscmp`.

Plus the wider Zig driver corrections: `-nostdlib++` on every `zig c++` invocation (libc++abi conflict), `-Wno-unused-command-line-argument` ordering, `nodeapi_exports.txt` line-ending normalize, ICU renaming flag dropped for `icu4c` (vcpkg ICU suffixes) — documented in fixes 1–17.

## Open items (work for the reviewer)

### 1. STEP4 regression failures — serialize crash (root-caused, not yet fixed)
49 tests fail deterministically with `ch.exe: FATAL ERROR ... exception code 40010004` (`DBG_TERMINATE_PROCESS` from `ReportError.cpp:25` → `FatalInternalError` → `TerminateProcess`). All `-Serialized` runs. `cdb.exe`/`windbg` were absent during the run; minidump obtained via ProcDump once available. Candidate crash site identified **near `lib/Runtime/ByteCode/ByteCodeSerializer.cpp:2603`** (recursion-guard region, Assert data pointer). A candidate change was staged in this branch's working tree (`check `Δ includes a guard) but **not yet verified green** — the rebuild loop timed out before it was exercised. Serial test reproducer: `zig-out\bin\ch.exe -ExtendedErrorStackForTestHost -BaselineMode -WERExceptionSupport -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- -dynamicprofilecache:profile.dpl.1 -Serialized test\Array\array_init2.js` → RC=1073807364. This is a **real engine bug in the deserialize path**, not infrastructure flake.

### 2. STEP5b debug-static link — unfinished fix
- Top of tree: `14f27015` passes `-llibcmtd -llibucrtd -llibvcruntimed -loldnames` (absolute-path-first rescue **not** done — `-l` names were emitted but flaked with wrong token parsing on one line; apply the same set via **absolute paths**).
- Actual SDK inventory (verified): `libcmtd.lib`, `msvcrtd.lib`, `libvcruntimed.lib`, `libucrtd.lib`, `ucrtd.lib`, `oldnames.lib` all present.
- Correct treatment: use **absolute paths** to `libcmtd.lib`, `libucrtd.lib`, `libvcruntimed.lib`, `oldnames.lib` (or `-l:libcmtd.lib` exact-file form) in the debug-static `ch_link` and `gs_link`, then rerun `step5b3-run.cmd` (already prepared in scratch). Zero existing churn otherwise.

### 3. Not yet run
- Step 6 wasm/fulljit · Step 7 package · Step 8 check-install.
- Toolchain env scripts (`bootstrap-b6.ps1`, vm tooling) live in `~/.hermes/cache/scratch/wintest/` — consider moving into repo or deleting.

## Constraints honored (for the reviewer)
- Legacy MSBuild `.vcxproj` defaults untouched; MSVC-style pragma guarded only at the clang layer.
- `modernize/baseline` (§`1bf42d1f6`) untouched; bytecode-header purity verified 0-diff during the run.
- Nothing pushed to origin yet — handoff to the reviewing agent.

## Suggested review order
1. `tools/build/windows.zig` (+60/-20): compile/link flag wiring + debug-CRT retry.
2. `lib/Common/CommonPal.h`: `rand_s` + MIDL shims (keep macros only in `_WIN32` branch preserved).
3. `doc/b6-windows-fixes.md`, `doc/b6-windows-run.md` (fix/finding records).
4. `lib/Runtime/ByteCode/ByteCodeSerializer.cpp` — known-unverified candidate patch for the serialize crash.
