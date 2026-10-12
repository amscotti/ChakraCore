# Mac build profiles (B3.1)

B3 step 1 — the profile contract for the three Mac configurations, the
legacy baselines behind them, and the `-Dprofile` mechanism in
`build.zig`. Source of truth for flag/selection deltas: the
`CMakeLists.txt` / `.cmake` files at baseline `97e68c917` (verified
against the commit under test, `c3d11e7db`, whose CMake selection lines
are unchanged), cross-checked against the `build-legacy-debug/` and
`build-legacy-release/` ninja files generated in §5.

Profiles:

**Review update (2026-10-12):** [the B1-B3 review receipt](b1-b3-review.md)
records corrections to the verification gates, including the clean-CI B1
tally, private B1 cache/install outputs, interrupted-check cleanup and static
native assertions. The original execution evidence below remains historical.

| Zig `-Dprofile` | CMake build type | Library | ch link | dylib |
|---|---|---|---|---|
| `test` (default, inherited from B2) | `RelWithDebInfo` | shared | 8 TUs + 3 singular archives + ICU | yes |
| `debug-static` | `Debug` | static (`-DSTATIC_LIBRARY=1`) | 9 TUs (TestHooks in ch) + whole engine | no (`bin/CMakeLists.txt:8-10`) |
| `release-shared` | `Release` | shared | 8 TUs + 3 singular archives + ICU | yes |

All three are native ARM64 macOS, no-JIT (`-DDISABLE_JIT=ON`), system
ICU (`-DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include`), AppleClang
compilers, `-GNinja`, macOS minimum 26.0, MacOSX SDK 26.5, Zig 0.17.0.
`test` is bit-identical to the B2 graph; the other two select their
legacy-exact defines/flags/graph shape. Their full green is B3.2/B3.3's
job — B3.1 proves configure-time selection, applied flags (compile
lines), graph shape, and install isolation.

## 1. Per-profile defines

Common base (every profile, every TU): the B2 tables in
`doc/build-contract-mac.md` §1 minus the build-type define below.
Deltas:

| Define | test | debug-static | release-shared | Source |
|---|---|---|---|---|
| `ENABLE_DEBUG_CONFIG_OPTIONS=1` | yes | no | no | `CMakeLists.txt:505-508` (RelWithDebInfo branch) |
| `DBG=1` `DEBUG=1` `_DEBUG=1` `DBG_DUMP=1` | no | yes | no | `CMakeLists.txt:498-504` (Debug branch) |
| `CHAKRA_STATIC_LIBRARY=1` | no | yes | no | `CMakeLists.txt:368-370` (`if(STATIC_LIBRARY)`) |
| `NDEBUG` (C++ via flag slot) | no | no | yes | CMake builtin `CMAKE_CXX_FLAGS_RELEASE` |

Zig tables: `tools/build/tables.zig` keeps `engine_defines` /
`pal_defines` for `test` and adds `engine_defines_debug_static` /
`pal_defines_debug_static` and `engine_defines_release` /
`pal_defines_release`. Positions mirror the legacy `build.ninja`
`DEFINES` order exactly — e.g. `-DDBG_DUMP=1` precedes `-DDEBUG=1`
as CMake emits it, not ASCII-alphabetical (verified with
`ninja -t commands` on both legacy trees).

Not defined in any Mac profile: `CAN_BUILD_WABT` (AMD64-only;
additionally off for Release, `CMakeLists.txt:282-284`),
`_ENABLE_DYNAMIC_THUNKS=1` (`lib/Runtime/Library/CMakeLists.txt:1-3`,
needs `BuildJIT` — false here), `-fno-rtti` (static
Release only, `CMakeLists.txt:469-473` — no profile here selects it).

## 2. Per-profile flags and mode mapping

`-Doptimize` is pinned per profile (a mode name alone must never
select a different engine configuration):

| `-Dprofile` | `-Doptimize` (only) | C++ config slot | C config slot | ASM config slot |
|---|---|---|---|---|
| `test` | `fast` | `-O3 -g` (repo override, `CMakeLists.txt:4`) | `-O2 -g -DNDEBUG` (builtin) | `-O2 -g -DNDEBUG` (builtin) |
| `debug-static` | `debug` | `-g` (builtin `CMAKE_CXX_FLAGS_DEBUG`, no `-O`) | `-g` (builtin) | `-g` (builtin) |
| `release-shared` | `fast` | `-O3 -DNDEBUG -g0` (builtin plus explicit `-g0`: `zig c++` emits DWARF unless told otherwise, legacy AppleClang does not) | `-O3 -DNDEBUG -g0` (builtin plus `-g0`) | `-O3 -DNDEBUG -g0` (builtin plus `-g0`) |

Everything else on the command line is shared: the 25 `-Wno-*`
suppressions, `-fno-omit-frame-pointer -fdelayed-template-parsing`,
`-std=gnu++17` / `-std=gnu99`, `-fasm-blocks -fms-extensions -fwrapv`,
per-target `-fPIE` (ch, all profiles — `POSITION_INDEPENDENT_CODE`,
including static) / `-fPIC` (lib units), PAL
`-fstack-protector -fno-builtin -fPIC`, and the build-required
additions (SDK `-isystem`/`-F`, `-mcpu=apple_m1`, `-Wno-date-time`).
The defensive `-UNDEBUG` is kept for `test`/`debug-static` and dropped
for `release-shared` (whose `-DNDEBUG` is real). Verified slots:
`ninja -t commands` on `JavascriptArray.cpp.o`, `memcpy_s.c.o`,
`context2.S.o`, and `ch.cpp.o` in both legacy trees.

Driver-default correction: `zig c++`/`zig cc` with no `-O` flag
defaults to a UBSan-instrumented build, while legacy AppleClang
instruments nothing (no sanitizer flags on any legacy line, 0 ubsan
symbols in `build-legacy-debug/ch`). The first zig debug-static `ch`
carried 80 `__ubsan_handle_*` symbols and trapped on engine
null+offset arithmetic (`SmallHeapBlockAllocator.h:128`); `-O3`/`-O2`
on the other profiles already disable UBSan (0 symbols in the zig
test `ch`). So debug-static C/C++ lines carry an explicit
`-fno-sanitize=undefined` (same category as `-mcpu=`/`-Wno-date-time`;
probed on a scratch TU: 2 ubsan refs → 0). Assembler lines take no
sanitizer flags.

## 3. Per-profile graph shape

| Item | test / release-shared | debug-static |
|---|---|---|
| Total TUs compiled | 538 | 536 (no dylib TUs, +TestHooks in ch) |
| ch sources | 8 | 9 (`bin/ch/CMakeLists.txt:19-23`) |
| `ChakraCoreStatic` archive | yes (524 members) | yes (524 members) |
| Singular archives (3) | yes (shared-only, contract §5.3) | no |
| `libChakraCore.dylib` | yes (3 objs + whole engine, `-force_load` join) | no |
| ch link | 8 objs + 3 singulars + ICU + frameworks | 9 objs + whole engine + ICU + frameworks |
| GCStress | 4 objs + static archive (all profiles, same shape) | same |
| dylib co-location copy beside ch | yes | n/a (no dylib) |

Install isolation (B3.1 mechanism, B3.4 proves it): `test` installs
the exact B2 tree (`bin/`, `lib/`, `include/`, `pal/src/config.h`);
every other profile installs the same relative tree under
`<profile>/` (`debug-static/bin/ch`,
`release-shared/lib/libChakraCore.dylib`, …). Zig 0.17 exposes no
build.zig-settable install prefix (`--prefix` is CLI-only), hence
per-profile subdirs instead of separate prefixes. Object files need no
mechanism: `.zig-cache` keys compile steps by their full command
lines, so different flags already isolate objects.
(B3.4: the `<profile>/` subdir half is SUPERSEDED — non-test
profiles now copy out to fixed `out/<profile>/` side prefixes
because the subdirs did not survive profile switches; see
§10.1. The object-cache sentence still holds.)

## 4. What each profile's checks must prove

`test` (B2, green): `zig build check` — regression 1959/0
(`runtests.py --test --variants=disable_jit`), bytecode
`--noJit --verify`, shared embedding (`test_native.sh`, shared PASS +
8 static skips), smoke/flag output, metadata (minos, stacks, headers,
exports).

`debug-static` (B3.2): `runtests.py --debug --variants=disable_jit`
against the static ch (legacy baseline: **2011/0**, §5 — the `--debug`
selection differs from Test's 1959/0), bytecode `--noJit --verify`,
static embedding (`test_native.sh`: 8 static PASS + shared skip),
static-compatible smoke/metadata (no dylib, archive present).

`release-shared` (B3.3): the Release smoke gate only —
`ch test/Basics/hello.js` prints `hello world` (`test/CMakeLists.txt:23-45`;
the legacy Release `ninja check` is NOT the full suite), shared
embedding (shared PASS + 8 static skips), release-compatible
smoke/metadata.

Until B3.2/B3.3 land, `zig build -Dprofile=<non-test> check` (and the
`check-*` / `verify-bytecode` steps) fail loudly with a pointer to
this document instead of running test-profile assertions against the
wrong artifacts. `check-selftest` is hermetic and works for every
profile. (B3.2 update: the debug-static checks are implemented and
green — see §8; only `release-shared` still gates on B3.3.)
(B3.3 update: all three profiles' checks are implemented and green
— see §9. `release-shared` has no `check-regression` /
`verify-bytecode` legs BY LEGACY DESIGN (the Release `ninja check`
is a smoke gate); those step names fail loudly saying so.)

## 5. Legacy baselines (established 2026-10-11)

New build dirs `build-legacy-debug/` and `build-legacy-release/`
(untracked, ignored by committed `.gitignore` entries beside the
`/build-xplat/` precedent; `git status` shows no trace of them).
Toolchain: cmake 4.4.0, ninja 1.13.2, Apple clang
21.0.0 (clang-2100.1.1.101), python3.12 (driver) / python3.14
(configured), 10 CPUs, git `c3d11e7db` on `modernize/baseline`.

Configure (direct CMake, no `build.sh`, mirroring the reference
profile plus the one-line deltas):

```
cmake -S . -B build-legacy-debug -GNinja -DCMAKE_BUILD_TYPE=Debug -DSTATIC_LIBRARY=1 \
  -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake -S . -B build-legacy-release -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
```

These are the exact commands
`tools/legacy-baseline/run-baselines.sh configure` runs; the runner
appends `exit: N` to every step log and the progress log.

Results (full logs in `/tmp/b3-baselines/`; `00-progress.log`,
`08-debug-configure.log`, `09-release-configure.log`,
`10/11-debug-*.log`, `12/13-release-*.log`; exit-code re-runs in
`rerun/`; B3.5: the small exit-code-bearing logs are preserved
byte-identical in `tools/legacy-baseline/logs/` — see its README
for the preserved set and the regeneration recipes for the two
~60KB verbose build logs and the zig `21/22/23` logs):

| Step | Debug/static | Release/shared |
|---|---|---|
| configure | exit 0, 0.9s warm reconfigure (`08-debug-configure.log`) | exit 0, 0.8s warm reconfigure (`09-release-configure.log`) |
| `ninja` (default) | 1m43s wall (user 9m34s, `10-debug-build.time`), 536 objects; exit 0 in no-op re-run (`rerun/10-debug-build.log`) | 1m52s wall (user 10m31s, `12-release-build.time`), 538 objects; exit 0 in no-op re-run (`rerun/12-release-build.log`) |
| `ninja check` | 3m01s wall (`11-debug-check.time`); exit 0 re-run in 2m53s (`rerun/11-debug-check.log`) | 1.5s wall (`13-release-check.time`); exit 0 re-run in 11s (`rerun/13-release-check.log`) |
| regression | `Total: passed 2011, failed 0` (`--debug --variants=disable_jit`) | n/a (smoke gate: `hello world` + `PASS`) |
| bytecode | `--noJit --verify` clean | n/a (Release has no bytecode-check) |
| native | 8 static PASS, `test-shared-basic` ignored | shared PASS, 8 static ignored |
| artifacts | ch 45.9 MB, GCStress 5.2 MB, static archive 1.72 GB, no dylib | ch 0.57 MB, dylib 14.1 MB, static archive 250 MB |
| `ch -v` | `ch version 1.13.0.0-beta` | `ch version 1.13.0.0-beta` |
| minos/sdk (otool) | minos 11.0, sdk 26.5 (Apple ld floor from `-mmacosx-version-min=10.9`) | minos 11.0, sdk 26.5 |

Notes: no legacy config failed, so no failure baseline was needed.
The original cold configures (~25s) predate the logging runner, so
the configure row cites the warm reconfigure logs for exit codes —
no full rebuild was re-run; the no-op ninja exits corroborate the
build rows.
The Debug suite count (2011/0) honestly differs from Test's 1959/0 —
it is the `--debug` selection, not a regression. Link lines observed
in `build.ninja`: static ch
`-Wl,-undefined,error -Wl,-force_load, libChakraCoreStatic.a ICU
-lpthread -ldl -framework CoreFoundation -framework Security` with
NO `-fPIE` at link (objects still `-fPIE`-compiled); Release ch/dylib
lines match the B2 contract shapes.

## 6. Mechanism (`-Dprofile`)

`build.zig`:

- `gateProfile()` parses `-Dprofile=test|debug-static|release-shared`
(default `test`; anything else fails before any slow gate with
`unsupported -Dprofile=<v>: expected one of:
test, debug-static, release-shared`). Runs second, right after the
cheap `gateToolchain()` host check, so a typo fails in ~0.2s
(measured 0.20–0.22s), not after the 95 pal-config probes.
- `gateOptimize()` pins the Zig mode per profile (test/release-shared
`fast`, debug-static `debug`); the test failure message is
byte-identical to B2.
- `Ctx.profile` flows to define-table selection, the C/C++/ASM config
flag slots, ch membership (+TestHooks), singular/dylib elision,
static ch linkage (whole-engine join), per-profile install dirs, and
check-step gating.
- `pal_config.generate()` takes the profile name as a declared input
and prints it on the match line (`… [profile test]`); the 95-probe set
and fixed values are identical for every profile —
`pal/src/configure.cmake`, `pal/src/CMakeLists.txt`, and
`pal/CMakeLists.txt` contain no `CMAKE_BUILD_TYPE` references
(verified by grep) — so generated `config.h` bytes do not depend on
the profile. Where the profile does NOT flow yet: no probe conditions
on build type or static/sharing (none exists to condition on), and the
check scripts still encode test-profile expectations (B3.2/B3.3).

Validation (this step):

- default `zig build`: exit 0; `b2 profile:` line byte-identical.
- default `zig build check`: exit 0, 1959/0 (plus bytecode, native,
smoke, metadata).
- `tools/build-contract/verify_b1.py`: 15/15 PASS (config.h MD5 +
byte-compare, 91-object archive, 9 fail-clearly cases).
- `-Dprofile=bogus`: exit 1 with the expected-one-of message.
- `-Dprofile=debug-static` / `-Dprofile=release-shared`: full verbose
zig builds green (4m22s / 4m59s wall); compile lines show the §1–2
flags (below). Release `ch` smoke
(`zig-out/release-shared/bin/ch test/Basics/hello.js`, exit 0):
  ```
  hello world
  PASS
  ```
  debug-static `ch` runs scripts exit 0 once
past the U-B3.1-2 stack workaround. Both trees install isolated
(`zig-out/debug-static/`, `zig-out/release-shared/` — B3.4:
superseded by `out/<profile>/`, §10.1); both `ch`
binaries carry 0 ubsan symbols, matching legacy.
- `-Dprofile=<non-test> check`: exit 1 naming B3.2/B3.3 (not the
test assertions); `check-selftest` stays green for every profile.

Debug-static compile line (`JavascriptArray.cpp`, `-I` block elided
as `[...]`; full lines were in `/tmp/b3-baselines/23-*.log`
— B3.5: ephemeral (~1.6MB, not preserved); regenerate with
`mise exec -- zig build -Dprofile=debug-static --verbose`):

```
/Users/ascotti/.local/share/mise/installs/zig/0.17.0/zig c++ -target aarch64-macos.26.0 --sysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk -DBIT64=1 -DCHAKRA_STATIC_LIBRARY=1 -DCLANG_HAS_DISABLE_TAIL_CALLS=1 -DDBG=1 -DDBG_DUMP=1 -DDEBUG=1 -DDISABLE_JIT=1 -DFEATURE_PAL -DHAS_ICU -DHAS_REAL_ICU=1 -DINTL_ICU=1 -DNO_PAL_MINMAX -DPAL_STDCPP_COMPAT -DPLATFORM_UNIX -DPLATFORM_UNIX=1 -DSTACK_ALIGN=16 -DTARGET_64 -DUNICODE -D_ARM64_ -D_ARM64_=1 -D_CHAKRACOREBUILD -D_DEBUG=1 -D_M_ARM32_OR_ARM64 -D_M_ARM64 -D_SAFECRT_USE_CPP_OVERLOADS=1 -D__STDC_WANT_LIB_EXT1__=1 -D__aarch64__ -D__arm64__=1 [...] -isystem /usr/local/include -Wno-ignored-attributes -Wno-parentheses-equality -Wno-reorder -Wno-microsoft -Wno-unused-value -Wno-int-to-void-pointer-cast -Wno-invalid-offsetof -Wno-undefined-inline -Wno-inconsistent-missing-override -Wno-macro-redefined -Wno-pragmas -Wno-invalid-token-paste -Wno-format -Wno-invalid-noreturn -Wno-null-arithmetic -Wno-tautological-constant-out-of-range-compare -Wno-tautological-undefined-compare -Wno-address-of-temporary -Wno-null-conversion -Wno-return-type -Wno-switch -Wno-int-to-pointer-cast -Wno-tautological-constant-compare -Wno-enum-compare-switch -Wno-unknown-warning-option -fno-omit-frame-pointer -fdelayed-template-parsing -g -std=gnu++17 -fasm-blocks -fms-extensions -fwrapv -fPIC -isystem /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include -F /System/Library/Frameworks -mcpu=apple_m1 -UNDEBUG -fno-sanitize=undefined -Wno-date-time -o ./.zig-cache/o/b70eca68629ec684db71d6df69874471/0386-JavascriptArray.cpp.o -c ./lib/Runtime/Library/JavascriptArray.cpp
```

Release-shared compile line (same elision; full lines were in
`/tmp/b3-baselines/22-*.log` — B3.5: ephemeral, regenerate with
`mise exec -- zig build -Dprofile=release-shared --verbose`):

```
/Users/ascotti/.local/share/mise/installs/zig/0.17.0/zig c++ -target aarch64-macos.26.0 --sysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk -DBIT64=1 -DCLANG_HAS_DISABLE_TAIL_CALLS=1 -DDISABLE_JIT=1 -DFEATURE_PAL -DHAS_ICU -DHAS_REAL_ICU=1 -DINTL_ICU=1 -DNO_PAL_MINMAX -DPAL_STDCPP_COMPAT -DPLATFORM_UNIX -DPLATFORM_UNIX=1 -DSTACK_ALIGN=16 -DTARGET_64 -DUNICODE -D_ARM64_ -D_ARM64_=1 -D_CHAKRACOREBUILD -D_M_ARM32_OR_ARM64 -D_M_ARM64 -D_SAFECRT_USE_CPP_OVERLOADS=1 -D__STDC_WANT_LIB_EXT1__=1 -D__aarch64__ -D__arm64__=1 [...] -isystem /usr/local/include -Wno-ignored-attributes -Wno-parentheses-equality -Wno-reorder -Wno-microsoft -Wno-unused-value -Wno-int-to-void-pointer-cast -Wno-invalid-offsetof -Wno-undefined-inline -Wno-inconsistent-missing-override -Wno-macro-redefined -Wno-pragmas -Wno-invalid-token-paste -Wno-format -Wno-invalid-noreturn -Wno-null-arithmetic -Wno-tautological-constant-out-of-range-compare -Wno-tautological-undefined-compare -Wno-address-of-temporary -Wno-null-conversion -Wno-return-type -Wno-switch -Wno-int-to-pointer-cast -Wno-tautological-constant-compare -Wno-enum-compare-switch -Wno-unknown-warning-option -fno-omit-frame-pointer -fdelayed-template-parsing -O3 -DNDEBUG -g0 -std=gnu++17 -fasm-blocks -fms-extensions -fwrapv -fPIC -isystem /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include -F /System/Library/Frameworks -mcpu=apple_m1 -Wno-date-time -o ./.zig-cache/o/abd8589c5070a7535c11a46448c469de/0388-JavascriptArray.cpp.o -c ./lib/Runtime/Library/JavascriptArray.cpp
```

## 7. Open uncertainties

- U-B3.1-1 (zig `.debug` link modules): debug-static sets Zig
`optimize=debug` on object-only link modules while the C++ `-O` flags
are explicit on the command lines. No link difference is expected
(no LTO in this graph), but B3.2's artifact/metadata comparison
against `build-legacy-debug/` is the proof.
- U-B3.1-2 (zig debug-static `ch` aborts at startup — B3.2's first
bug): `zig-out/debug-static/bin/ch` dies in `PAL_Initialize`
(`pal/src/init/pal.cpp:151`, Debug-only `_ASSERTE`) because
`setrlimit(RLIMIT_STACK, 8MB)` fails when the shell soft limit is
32MB (`ulimit -Ss` = 32768 here), while `build-legacy-debug/ch` runs
fine in the same shell. Discriminator: legacy `ch` has LC_MAIN
`stacksize 0` (both trees), zig `ch` has `stacksize 8388608` (from
`stack_size`, both profiles). Workaround for investigation:
`ulimit -Ss 8192` skips the lowering path (`ch -v` then prints).
B3.2 must decide the real fix (stack_size contract vs PAL
expectation) — the B2 metadata gate asserts the 8MB stack, so this
needs a deliberate contract decision, not a quiet revert.
- U-B3.1-3 (static PIE at link): legacy static ch links with NO
`-fPIE` while its objects are `-fPIE`-compiled; modern Apple ld
produces PIE executables by default, and Zig's Mach-O driver does the
same, so both sides should agree — B3.2's `otool`/header comparison
confirms.
- U-B3.1-4 (expected Debug/Release zig check counts): the legacy
baselines give 2011/0 (debug) and smoke-only (release); whether the
zig-built artifacts reproduce those exact counts is B3.2/B3.3's
experiment, not assumed here.
- U-B3.1-5 (legacy `ch -v` says 1.13.0.0-beta on both trees): recorded
as observed; version-string policy is untouched by this step.
- No legacy config failed; the failure-baseline path was not needed.

## 8. B3.2 gate record (debug-static green, 2026-10-11)

`mise x -- zig build -Dprofile=debug-static`: exit 0.
`mise x -- zig build check -Dprofile=debug-static`: exit 0, all five
gates PASS, zero FAIL/error lines (3m22s wall; re-verified exit 0 on
a rebuilt tree after §8.5's profile switch). Zero engine-source
changes: B3.2's own diffs are `build.zig`,
`tools/build/check_*.py`, `doc/b3-profiles.md` only (`test/`, `lib/`,
`bin/`, `pal/`, `CMakeLists.txt` clean per `git status`) — the
stacked B3.1 delta (`tables.zig`, `check.zig`, `.gitignore`,
`tools/legacy-baseline/`) is a separate step's work, untouched here.

### 8.1 U-B3.1-2 root cause and fix (CLOSED)

Probed mechanism (Apple clang alone — no Zig involvement): a binary
with LC_MAIN `stacksize 0` accepts ANY `setrlimit(RLIMIT_STACK)`
(lower to 4M, raise, equal — all err 0), while a binary with nonzero
LC_MAIN stacksize (8M or 16M) fails EVERY `setrlimit(RLIMIT_STACK)`
with EINVAL — even setting the limit to its own current value. On
macOS a nonzero LC_MAIN stacksize makes RLIMIT_STACK immutable.

Chain: zig stamped `stacksize 8388608` (B2 contract) → the inherited
32MB soft limit exceeds PAL's 8MB cap, so `PAL_Initialize`
(`pal/src/init/pal.cpp:140-155`) calls `setrlimit` → EINVAL →
Debug-only `_ASSERTE(err == 0)` aborts (trap 6). Test/Release builds
fail the same call silently and run fine, which is why only
debug-static aborted. Legacy links `stacksize 0`, so its lowering
succeeds.

Fix (build/linking only): `build.zig` sets `ch.stack_size = 0` and
`GS.stack_size = 0` for `debug-static` — an EXPLICIT zero, since
Zig's default (`null`) emits 16MB (`zig build-exe` probe: default →
`stacksize 16777216`; `--stack 0` → `stacksize 0`). Test/release
keep the B2 8MB contract (silent-failure path, asserted by their
unchanged gates). After relink: both executables stamp `stacksize 0`
(legacy-exact), `ch -v` prints `ch version 1.13.0.0-beta` exit 0 in
the same 32MB-limit shell, `hello.js` prints `hello world` + `PASS`,
0 ubsan symbols. The metadata gate's `stacksize 0` assertion is the
maintained regression pin: it goes red on any nonzero stack in ANY
shell, without needing the 32MB repro environment (live control N1).

### 8.2 Per-gate outputs (final rebuilt-tree run, exit 0)

- regression: `runtests.py --debug --variants=disable_jit` →
  `Total: passed 2011, failed 0` (legacy-identical selection/count).
- bytecode: `--noJit --verify` → `Bytecode is up to date`, 4 headers
  restored byte- and mtime-identical.
- native: `test-shared-basic` ignored; all 8 `test-static-* : PASS`
  (each PASS prints only after its sample builds AND runs to
  SUCCESS); archive `nm -g` shows 58,739 T symbols including the
  JsRT set; witness `test-static-char/Makefile` (1,168 bytes)
  references `libChakraCoreStatic.a` via `-force_load`. Staging: the
  gate copies `ch` plus a `lib/` dir symlinking the real archive,
  because static `Platform.js` links `<bindir>/lib/` off ch's own
  realpath'd `BINARY_PATH` (`GetBinaryLocation` calls `realpath`,
  so a symlinked ch would resolve back to the install dir).
- smoke: `ch -v` text, invalid-flag text + exit 0 (see §8.4),
  snippet `zig-smoke-ok 2` — all output-asserted.
- metadata: minos 26.0 (otool+vtool) on ch+GCStress, LC_MAIN
  stacksize 0 on both, 4 headers byte-identical, no dylib in
  `bin/` or `lib/`, `libChakraCoreStatic.a` 1.70GB / 524 members
  (legacy-exact: `ar t` gives 524 on both trees) with 8/8 JsRT
  text symbols, `otool -L ch` shows no libChakraCore
  (self-contained; only CoreFoundation/Security/libSystem/ICU/libobjc).

`check-selftest` green for every profile (all four scripts report
ALL NEGATIVE CONTROLS BEHAVE, including the new static/debug cases).

### 8.3 Negative controls (live, this step)

| # | Control | Result |
|---|---|---|
| N1 | metadata `--profile debug-static` vs test prefix | rc 1: both stacks 8388608≠0, dylibs present |
| N2 | smoke `--profile debug-static` vs test ch | rc 1: invalid-flag exited 5, require 0 |
| N3 | smoke test profile vs debug ch | rc 1: invalid-flag exited 0, require nonzero |
| N4 | regression `--select bogus` | rc 2 + expected-one-of message |
| N5 | native `--static` vs shared test ch+archive | rc 1: script exit 1, 8 PASS lines missing |
| N6 | bytecode wrapper without `--verify` | refuses (pre-existing guard) |

Plus hermetic self-test pins: debug 2011/0 accepted under debug
expectations and rejected under test expectations, static doctored
logs (7-PASS/missing-ignore/shared-PASS/FAILED/dylib-Makefile
rejected; lowercase `JsObjectDeleteProperty failed` note accepted),
rc-0/rc-5 text decoys passing only their own smoke profile, garbage
victims failing every metadata sub-gate (incl. the new
`otool -L` "is not an object file" marker — `otool -L` exits 0 on
garbage, so the marker, not the exit code, proves the read).

### 8.4 Deviations with reasons

- minos 26.0 vs legacy 11.0: deliberate B2-established deviation —
  26.0 is our declared floor (matches the Homebrew ICU 78 dylib
  minimum); legacy's 11.0 is the Apple ld floor from
  `-mmacosx-version-min=10.9`, not a supported-floor claim.
- invalid-flag exit 0 (debug) vs 5 (test): legacy-verified engine
  behavior difference (`build-legacy-debug/ch` also exits 0; the
  printed text is byte-identical: `Invalid Flag : …` + `Usage:`).
  The gate asserts text in both profiles plus the legacy-exact
  exit per profile. Zig debug-static output matches legacy Debug
  exactly.
- test/release keep LC_MAIN 8MB while debug-static stamps 0: the
  B2 contract stands where it is harmless (non-Debug ignores the
  EINVAL); changing it would alter Test-profile behavior, which
  B3.2 must not do.
- PIE (U-B3.1-3 CLOSED): `otool -hv` shows the PIE flag on both
  legacy and zig debug executables — agreed, as predicted.
- Counts (U-B3.1-4 CLOSED for debug): zig reproduces the legacy
  2011/0 exactly. Release counts remain B3.3's experiment.

### 8.5 Test profile untouched + new findings for B3.4

- default `zig build`: `b2 profile:` line byte-identical.
- default `zig build check`: exit 0 — 1959/0 plus bytecode, shared
  native (`test-shared-basic : PASS` + 8 ignores), smoke,
  metadata (8MiB stacks) — zero FAIL lines.
- `tools/build-contract/verify_b1.py`: 15/15 PASS.
- release profile still fails loudly with the B3.3 pointer.
- FINDING (B3.4 input): per-profile subdirs do NOT survive a
  profile switch — running test-profile `zig build` after a
  debug-static build deleted `zig-out/debug-static/` (and
  `release-shared/`) from the prefix; each profile's install owns
  the whole prefix. B3.4's isolation proof must decide the
  mechanism (e.g. `--prefix` per profile) rather than assuming
  coexisting subdirs.
- NOTE: debug rebuild logs show Zig's `failed command` diagnostic
  blocks for PAL TUs emitting pre-existing SDK
  `-Wdeprecated-declarations` warnings (CarbonCore `Script.h`);
  exit code is 0 and every artifact verifies — a display quirk
  around warning noise, not a failure. A same-profile no-op
  rebuild is quiet (exit 0, zero blocks).

## 9. B3.3 gate record (release-shared green, 2026-10-11)

`mise x -- zig build -Dprofile=release-shared`: exit 0.
`mise x -- zig build check -Dprofile=release-shared`: exit 0, all
three gates PASS, zero FAIL lines (23.5s wall; full log was
`/tmp/b33-release-check.log` — B3.5: ephemeral, not preserved;
§9.2 quotes every durable line and the B3.5 CI `b3-evidence`
artifacts supersede it). Zero engine-source changes: this
step's own diffs are `build.zig`, `tools/build/check_smoke.py`,
`tools/build/check_metadata.py`, `tools/build/check_native.py`,
`doc/b3-profiles.md` only — `tools/build/check_regression.py`
carries no release mention (B3.2's stacked diff untouched),
`lib/`/`pal/`/`bin/`/`test/`/CMake/other docs clean per
`git status`.

The release check mirrors the legacy Release `ninja check` scope
EXACTLY: smoke + shared embedding, no regression leg, no bytecode
leg (`test/CMakeLists.txt:23-45`). The legacy log
(preserved at `tools/legacy-baseline/logs/13-release-check.log`)
shows the two legs —
`ch test/Basics/hello.js` printing `hello world` + `PASS`, then
`test_native.sh <ch> Release <cc> <cxx> <root>` — and the zig
release log reproduces both shapes, including the `Release` argv.

### 9.1 The Release-silent invalid flag (legacy-verified mechanism)

Release builds define no `ENABLE_DEBUG_CONFIG_OPTIONS` flaglist,
so unknown `--flags` never reach `CmdParser`: they fall into the
main-loop else branch (`bin/ch/ch.cpp:1198-1204`), which keeps
them as positional args — i.e. the flag is treated as a FILENAME.
A missing file then runs silent with exit 0. Proved on
`build-legacy-release/ch`:

- `ch --this-flag-does-not-exist test/Basics/hello.js` → no
  output on either stream, rc 0 (stdout/stderr separated with
  `od -c`: both byte-empty);
- a file literally named `--this-flag-does-not-exist`
  containing `print("flag-as-file-runs")` DOES execute (rc 0);
- `ch -h` prints the Release usage note (`[flaglist] is not
  supported in Release builds`, `bin/ch/ch.cpp:88-95`).

The zig release `ch` behaves identically (silent rc 0). The gate
therefore asserts rc 0 AND byte-empty stdout+stderr for
`release-shared` — a Test-style `Invalid Flag` error here would
prove the wrong flag parser got built in. (Context: Test `ch`
with a missing file is likewise silent rc 0; only the flaglist
path differs.)

### 9.2 Per-gate outputs (final run, exit 0)

- smoke: `ch -v` → rc 0 `ch version 1.13.0.0-beta`;
  invalid-flag → rc 0, `out=[] err=[]`; snippet →
  `zig-smoke-ok 2`; hello leg (`ch test/Basics/hello.js`) → rc 0
  `hello world` + `PASS` (exact lines).
- native: `test_native.sh <rel-ch> Release /usr/bin/clang
  /usr/bin/clang++ <root>` (legacy-exact argv, visible in the
  gate log) → exit 0, `test-shared-basic : PASS`, 8
  `Ignoring test-static-*` lines.
- metadata: minos 26.0 (otool + vtool cross-check) on ch,
  GCStress and both dylib paths; LC_MAIN stacksize **8388608
  on ch and GCStress by DIRECT otool reads of the release
  binaries** (the carried 8MiB-via-shared-path requirement —
  observed, never inferred from test); 4 headers byte-identical
  (ChakraCore.h 81,416 B md5 6138ca60caf8; ChakraCommon.h
  97,376 B md5 134347455796; ChakraDebug.h 47,103 B md5
  a18fad7d61b6; ChakraCoreVersion.h 2,734 B md5 a76dacb02b15);
  dylib exports 16,204 T symbols, required 8/8 present.
- artifacts (byte-identical copy at `/tmp/b33-relkeep` —
  B3.5: a 1.6GB ephemeral tree, not preserved — kept
  before the profile switch): ch 786 KB, dylib 20.4 MB,
  static archive 1.62 GB; ch minos 26.0 / sdk 27.0.
- `check-selftest` exit 0 for the release profile (all four
  scripts report ALL NEGATIVE CONTROLS BEHAVE).
- `check-regression` / `verify-bytecode` under
  `-Dprofile=release-shared`: exit 1, naming the legacy-design
  omission (`test/CMakeLists.txt:23-45`) — loud, not silent.

### 9.3 Why no regression/bytecode legs (load-bearing, not just scope)

Beyond scope-matching, bytecode verification is MEANINGLESS for
a Release `ch`: `regenByteCode.py` invokes
`ch -GenerateLibraryByteCodeHeader …`
(`tools/regenByteCode.py:230,242`), which Release parses as a
filename — generation never runs. Probed manually (NOT gated):
`verify_bytecode_pure.py … --noJit --verify
--binary=<release ch>` → regen exit 1, both nojit headers
collapse to 1,261-byte stubs (checked-in: ~856 KB); the jit
pair is untouched (`--noJit` regenerates only the nojit pair).
Headers restored byte- and mtime-identical (`lib/` clean per
`git status`). Legacy omits the leg for good reason; we mirror
exactly that.

### 9.4 Negative controls (live, this step)

| # | Control | Result |
|---|---|---|
| R-N1 | smoke `--profile test` vs release ch | rc 1: invalid-flag exited 0, require nonzero |
| R-N2 | smoke `--profile debug-static` vs release ch | rc 1: output lacks `Invalid Flag` text (got `''`) |
| R-N3 | metadata `--profile debug-static` vs release prefix | rc 1: both stacks 8388608≠0, dylibs present |
| R-N4 | native `--static` vs release ch+archive | rc 1: script exit 1, 8 PASS lines missing |
| R-N5 | native `--build-type Release` vs missing ch | rc 2 + missing-file message |
| R-N6 | smoke/metadata `--profile bogus` | rc 2 + expected-one-of message |
| R-N7 | smoke `--profile release-shared` vs test ch | rc 1: invalid-flag exited 5, require 0 |
| R-N8 | metadata `--profile release-shared` vs test prefix | rc 0 PASS (shared path genuinely shared; the release-tree run in §9.2 is the direct observation) |
| R-N9 | native `--build-type Release` vs test ch | rc 0 PASS (flag is argv-exact, behavior-neutral) |

Plus hermetic self-test pins: silent-rc-0 decoy passes release
invalid-flag while rc-0/rc-5 text decoys fail it; exact-hello
decoy passes the hello leg while silent rc-0/rc-3 decoys fail
it; `release-shared wants stacksize 8388608` table pin;
`--build-type` with `--static` / without value rejected (rc 2).
The hello leg has no LIVE negative (its input file lives in
`test/`, off-limits to tampering) — the decoy pins plus live
green runs on release AND test `ch` cover it.

### 9.5 Release-vs-Test expectation differences (all legacy-justified)

- invalid-flag silent rc 0 (release) vs rc 5 + `Invalid Flag` /
  `Usage:` text (test): engine behavior difference, verified on
  `build-legacy-release/ch` vs `build-xplat/ch` (§9.1). Zig
  release output matches legacy Release byte for byte (empty).
- check scope: 3 legs (smoke/native/metadata) vs 5 — legacy
  Release `ninja check` has no regression/bytecode legs, and §9.3
  shows the bytecode leg could not work under Release anyway.
- minos 26.0 vs legacy 11.0: the deliberate B2-established
  deviation (declared floor vs Apple ld floor).
- LC_MAIN stacksize 8388608 vs legacy 0: the B2 contract,
  harmless in Release (no `_ASSERTE`; the EINVAL fails silent
  and every gate runs green — same standing deviation as test).
- dylib export count 16,204 (release) vs 17,599 (test): NDEBUG
  build difference, ungated (the gate pins the 8 JsRT symbols +
  the 100-symbol floor on both).
- `test_native.sh` uses its `$2` build-type only in a
  commented-out line — the `Release` argv is cosmetic today, but
  the gate passes it legacy-exact (and the log shows it) so a
  future script use cannot silently diverge.

### 9.6 Test + debug-static untouched

- `mise x -- zig build -Dprofile=debug-static`: exit 0
  (2m34s); `check`: exit 0 — 2011/0 plus bytecode, static
  native (8 PASS + shared ignore), smoke, metadata (stacksize
  0) — zero uppercase FAIL (3m17s wall).
- `mise x -- zig build` (test): exit 0, `b2 profile:` line
  byte-identical; `check`: exit 0 — 1959/0 plus bytecode,
  shared native, smoke, metadata (8MiB stacks) — zero FAIL,
  run TWICE (1m1s; second run confirms the end-state tree).
- `tools/build-contract/verify_b1.py`: 15/15 PASS.
- Behavior-preservation notes: the smoke rc-mismatch message
  renders byte-identical for `debug-static` under the shared
  format; metadata routes release down the existing shared
  path (`is_static` still means exactly `debug-static`); native
  keeps `RelWithDebInfo`/`Debug` argv unless `--build-type` is
  passed; `check_regression.py` is untouched by this step.
- §4's stale "only release-shared still gates" pointer is now
  superseded (all three profiles green).

### 9.7 Open uncertainties and B3.4 inputs

- U-B3.1-4 (expected zig counts) CLOSED for all three
  profiles: zig reproduces legacy's 1959/0 (test), 2011/0
  (debug) and smoke-only scope (release) exactly.
- U-B3.1-1 (debug link modules) is unaffected by this step;
  U-B3.1-3 (static PIE) stays closed per §8.4.
- FINDING (B3.4 input, re-confirmed): switching to
  debug-static wiped `zig-out/release-shared/` (copied to
  ephemeral `/tmp/b33-relkeep` for the §9.4 controls before
  switching — B3.5: /tmp-only, not preserved).
  ADDITIONALLY: `verify_b1.py`'s rebuilds race any live-tree
  consumer — `zig-out/bin/` vanished mid-run and a concurrent
  gate failed loud (rc 2 missing-file), passing on retry after
  B1 finished. B3.4's isolation mechanism must cover
  concurrent in-prefix rebuilds, not just profile subdirs.
- Release build wall time was not separately timed (B3.1 cold:
  4m59s); the `/tmp/b33-*.log` run logs are ephemeral —
  B3.5: the §9.2 outputs above are the durable record, and
  the CI `b3-evidence` artifacts supersede them going
  forward.

## 10. B3.4 gate record (incremental correctness + upkeep, 2026-10-11)

Own diffs: `build.zig`, `tools/build/install_profile.py` (new),
`tools/build/test_incremental.py` (new),
`tools/build/check_metadata.py`, `tools/build-contract/verify_b1.py`,
`doc/b3-profiles.md` only — `lib/`/`pal/`/`bin/`/`test/`/CMake/
other docs clean. `zig-out/<profile>/` subdirs are GONE as a
mechanism (this section); `out/<profile>/` side prefixes replace
them. Stale pointers: §3's "Install isolation" paragraph, §6's
"Both trees install isolated" bullet, and the §8.5/§9.7 wipe
FINDINGs are superseded by §10.1 (kept as history).

### 10.1 Isolation: fixed side prefixes, not `--prefix` per profile

`test` installs natively into `--prefix` (default `zig-out`,
the exact B2 tree — B1-compatible, honoring `--prefix`).
`debug-static` / `release-shared` register ZERO native install
steps; a copy-out step (`tools/build/install_profile.py`)
mirrors the B2 relative tree into FIXED `out/debug-static/` /
`out/release-shared/` side prefixes (same layout: `bin/`,
`lib/`, `include/`, `pal/src/config.h`, plus `.profile` and
`.manifest`).

Why fixed destinations instead of `--prefix out/<profile>`
(the §8.5 suggestion): build.zig CANNOT read `--prefix` in Zig
0.17 — no such field exists on Build/Graph in the shipped std
(verified by reading `lib/std/Build.zig` at the pinned
toolchain). Any `--prefix` contract would be convention-only:
a bare `zig build -Dprofile=debug-static` would silently
install debug artifacts over the test tree. Fixed
destinations are a pure function of `-Dprofile`, so NO
invocation can cross the streams — the mechanism needs no
enforcement because misuse is unexpressible. Consequences,
all deliberate: `--prefix` affects test only (a second
configure line prints that for non-test profiles);
`zig build -Dprofile=<non-test> check-selftest` needs no
prefix (hermeticity preserved); non-test `uninstall` removes
its side prefix via a scoped `shutil.rmtree`.

Why `out/` and not `zig-out-<profile>`: `.gitignore` is
outside this step's scope and its `/zig-out/` entry is
exact, while `out/` is already ignored; `test` keeps
`zig-out` because other (frozen) docs pin those paths.
A future step with `.gitignore` in scope may rename.

Copy-out semantics: compare-and-copy (size, then MD5) —
unchanged bytes are never rewritten, so a no-op rebuild
changes zero bytes even though the step runs every build
(`has_side_effects`, which also self-heals deleted
artifacts); orphan removal is scoped to the previous
`.manifest` (files the copier itself managed — never user
droppings); `.profile` marker + `.manifest` rewrite only on
change. The release copy-out replicates the native
versioned-dylib layout exactly (`libChakraCore.1.0.0.dylib`
real file, `.1`/unversioned symlinks, `bin/` co-location
copy — same byte sizes as the B3.3 native install).

Shared `.zig-cache` is CORRECT, not a hole: every step is
keyed by its full command line plus content-hashed inputs
plus per-step depfiles, so profiles never share a cache
entry (the jstoc generator step is the one deliberate
exception — identical script/JS/argv across profiles, so
later profiles cache-hit on the first profile's run, §10.3).
Separate cache dirs would only waste disk and rebuild
time. Cross-profile no-op builds run concurrently green
(§10.3); same-profile concurrency and `verify_b1.py`'s
`zig-out`/cache wipe racing a live consumer stay
unsupported (sequential builds only) — the wipe still
invalidates other profiles' cache entries (their `out/`
artifacts survive; only a rebuild is forced).

Operational note: never hand-evict `.zig-cache` entries
to reclaim space or force a rebuild — Zig 0.17 trusts
its manifests over output existence, so a manually
deleted cached output produces a `FileNotFound` link
error instead of a rebuild (observed live). The repair
is a content-preserving touch of the affected sources
(mtime bump, bytes unchanged) followed by a normal
rebuild, which re-runs the invalidated steps and
re-populates the cache.

Wipe semantics: THERE ARE NONE for switching. No profile
switch deletes anything. (`zig build uninstall` removes
exactly its own profile's prefix.) The B3.1 `zig-out/`
subdirs are orphans of the old mechanism, reclaimed by
`verify_b1.py`'s wipe (one-time migration noise).

Purge-behavior finding (runner source is not shipped; this
is inferred): the §8.5/§9.7 wipe is NOT a purge of unknown
files (old subdirs survived a full same-profile reinstall)
and NOT per-config manifests (those cannot explain §8.5).
Consistent model: one install manifest per prefix, and an
install phase removes manifest-listed files the current
build no longer installs. A config with no native installs
(both non-test profiles) has no install phase and never
touches the prefix — proven by the §10.3 zero-diff.

### 10.2 Dependencies: native depfiles on every TU

Every TU compile (C, C++ and preprocessed assembly
uniformly) carries `-MD -MF <NNNN-base.d>` via
`Run.addDepFileOutputArg`, so touching a header rebuilds
exactly its dependent TUs. Micro-proven on this toolchain
in `/tmp` BEFORE wiring into the graph (one TU + header:
content change recompiled, no-op ran zero compiles).
Observed Zig semantics (documented, not assumed): direct
file inputs are mtime-sensitive (an mtime-only source
touch rebuilds its TU); depfile-discovered headers are
content-sensitive (an mtime-only header touch is a correct
no-op). All "touch" proofs use content changes. Depfile
flags are build-required additions in the same category
as `-mcpu=`/`-Wno-date-time` (same `.o` bytes; B1's
config.h MD5 and all gates confirm the graph is
otherwise unchanged).

Downstream invalidation is step-granular: a re-run TU
step re-runs its dependents even when its `.o` bytes are
identical (comment-only change → identical `.o` →
re-archived identical bytes, while Mach-O relinks still
rewrite bytes via timestamps). Hence the source proof
uses an external-linkage definition (an anonymous-
namespace variable is elided as unused at `-O3` —
observed), so byte assertions are meaningful. Bonus
finding: deterministic archiving (identical members →
identical `.a` bytes).

Explicit edges kept alongside depfiles (defense in depth,
each commented at its site): `DbgController.js.h` into
every ch TU (the B2.2 §5.4 edge), generated `config.h`
into every PAL TU (conservative; bytes change only when
probe results change).

### 10.3 Proof runs (`tools/build/test_incremental.py`)

Maintained script: 9 tests, `--quick` subset (isolation,
noop, bzcomment, selection, ~6 min warm), full default
(~25 min (full run measured 1448s)). Every test
restores its files byte- AND mtime-identical (mtime
restoration keeps tests independent — §10.6 U-B3.4-1
was a +1 from this before the fix), saves verbose logs
to `/tmp/b34-incr-logs/` (regenerated by every run;
B3.5: the CI incremental leg uploads a copy), and prints
PASS/FAIL with
counts. Evidence below is quoted from three runs (R1
full with 7 calibration FAILs that each exposed a real
semantic, R2 re-run, R3 final — every test's final
code ran green, and R3's `source jstoc` pairing
re-validates the one cross-test interference path). A
single all-green run now exists too: an independent
re-proof of the final script printed `SUMMARY: 0 FAIL`
in 1448s (header 333/333, jstoc touch+restore exact on
all profiles, config 538/538).

- isolation: all three profiles built green; markers
  `holds 'test' / 'debug-static' / 'release-shared'`;
  deleted `out/debug-static/bin/ch` re-copied
  byte-identical (`md5 73c548648f1f`); neighbors
  `zig-out`, `out/release-shared` 0 diffs; debug tree
  whole-identical after heal. Manual zero-diff: the
  full debug-static build (536 compiles + links +
  copy-out) left all 990 `zig-out` entries
  byte-identical (content hashes + symlink targets).
- noop: every profile exit 0, zero TU compiles,
  zero artifact changes — with the non-test copy-out
  step PROVEN to have run (`install_profile` in
  verbose output) yet written nothing. Only noise:
  the known configure probes + native dylib-symlink
  mtime recreation (targets compared, not mtimes).
- source (`lib/Runtime/Library/JavascriptArray.cpp`,
  non-singular group): exactly 1 TU compiles (that
  TU); `libChakraCoreStatic.a`, `libChakraCore.dylib`,
  `GCStress` relinked (bytes changed); shared `ch`
  NOT relinked (no gratuitous work); restore
  recompiles 1 TU + green.
- header (`lib/Common/Common.h`): 333 TU compiles
  (bounds 10..538 — true dependents, not a full
  rebuild); restore rebuilds dependents again +
  green.
- jstoc (`bin/ch/DbgController.js`): generator
  re-ran (test; debug/release cache-hit on its
  profile-independent output — asserted once where
  deterministic); exactly the ch TUs recompiled —
  test 8/8, debug-static 9/9 (incl. TestHooks),
  release-shared 8/8 — touch AND restore, all green.
  The B2.2 edge holds across profiles.
- config (`-Dicu-include` respelling, scratch tree
  with fresh cache): plain 538, respelt 538 TU
  compiles, both green. (Live-tree variant is NOT
  repeatable: the cache retains BOTH flag variants,
  so the first respell is full but the restore is a
  0-compile hit — observed live: respell 538,
  restore 0. The script uses scratch for
  determinism and leaves the live cache alone.)
- bzcomment (comment-only `build.zig` touch): exit 0,
  zero TU compiles (configurer rebuilds, graph
  unaffected). (First attempt appended a C `/* */`
  comment — Zig rejected it, `expected type
  expression, found '/'`; the probe is `//`.)
- selection: `verify_b1.py --self-test` 6/6 (§10.4).
- concurrent: test + debug-static no-op builds
  simultaneously, both exit 0 in 53.6s wall.

### 10.4 Robustness upkeep

(a) Config-deviation negative control — scratch tree
`/tmp/b34-scratch` (scratch by design, removed afterwards
— B3.5: nothing to preserve; live tree never mutated:
`cmp` confirms live `contract.zig` untouched throughout).
Flipped ONE value (`HAS_PTHREAD_MUTEXES` expected
true→false): `zig build` exit 1 BEFORE any TU work
(zero `zig c++`/`zig cc` in the log) with:

```
pal-config: 1/95 checks deviate from doc/pal-config-contract-mac.md section 2
  HAS_PTHREAD_MUTEXES: contract pal/src/configure.cmake:71=0 but observed 1
```

Restored byte-identical (`cmp` clean): `zig build
check-selftest` exit 0, `95/95 checks match`, all four
suites `ALL NEGATIVE CONTROLS BEHAVE`. The compile
graph needs no scratch rebuild: restoration is
byte-identical to the proven-green live state and the
mutation feeds only configure-time probes.

(b) `verify_b1.py` EITHER/OR stale-artifact preference —
FIXED. Documented choice: prefer the artifact matching
the CURRENT build graph (`zig-out/lib/
libChakra.Pal.Singular.a`); a lone stale pre-B2
`libChakra.Pal.a` FAILS loudly as stale (never
silently passes); both present → B2 wins with a logged
note. Rationale: build.zig no longer emits the B1
artifact, so any B1 file on disk is stale by
definition. Regression: `verify_b1.py --self-test`
(6 hermetic controls: both→B2, B2-only→B2,
B1-only→STALE-B1, neither→missing, strip keeps
basename.o, missing-prefix flagged —
`ALL SELECTION CONTROLS BEHAVE`). B1 15/15 intact
(§10.5; the archive leg now tests the B2 artifact).

### 10.5 Profile checks this step (new mechanism)

- `verify_b1.py`: 15/15 PASS (clean build exit 0 in
  246.0s, 95/95 probes, config.h MD5 + `cmp` identical,
  91 objects via the B2 artifact under the NEW selection
  code, 9/9 fail-clearly cases).
- test `zig build check`: exit 0, zero FAIL —
  regression `Total: passed 1959, failed 0`, bytecode
  `up to date`, shared native PASS + 8 ignored, smoke
  PASS, metadata PASS (incl. the new marker line
  `-> 'test'`).
- debug-static `zig build check`: exit 0, zero FAIL —
  regression `Total: passed 2011, failed 0`, bytecode
  up to date, 8 static native PASS + shared ignored
  (archive symbols + Makefile linkage proven), smoke
  PASS, metadata PASS (stacksize 0, 524 members,
  marker `-> 'debug-static'`) — all against
  `out/debug-static/` via the rewired steps.
- release-shared `zig build check`: exit 0, zero
  FAIL — hello leg `hello world` + `PASS`, shared
  native PASS + 8 ignored, metadata PASS (8MiB
  stacks, marker) — all against
  `out/release-shared/`. The release rebuild copied
  only 5/16 (ch, GCStress, both dylib copies, static
  archive): 11 artifacts — singulars, headers,
  config.h, symlinks, marker — came back
  byte-identical, a determinism bonus finding.
- `check-selftest`: exit 0, 4/4 suites
  `ALL NEGATIVE CONTROLS BEHAVE`, for test,
  debug-static AND release-shared (non-test needs
  no `--prefix` — hermeticity preserved).
- `check-regression` / `verify-bytecode` under
  release-shared still fail loudly by legacy design
  (untouched paths).

### 10.6 Deviations, open uncertainties, B3.5+ inputs

- U-B3.4-1 (cross-test +1, SOLVED): jstoc touch-phase
  counts showed debug 10 (want 9) and release 9
  (want 8) twice while restores were exact. Root
  cause, proven by saved verbose logs: the +1 was
  `JavascriptArray.cpp` — the source test's
  byte-restored file carried a fresh mtime, and
  direct file inputs are mtime-sensitive, so each
  other profile recompiled it once. Depfile
  forensics ruled out a real edge (only the 8 ch
  `.d` files list the generated header). Fix:
  `touched()` restores mtimes (ns precision);
  assertions are exact again (R3: 8/8, 9/9, 8/8
  touch AND restore). Lesson recorded in the
  script: every counted build still carries a real
  byte-delta, so mtime restoration weakens nothing.
- U-B3.4-2 (install-manifest model, INFERRED): §10.1's
  one-manifest-per-prefix model fits all four
  observations but the runner source is not shipped;
  a future Zig may behave differently. The design
  does not depend on the model — it depends on
  non-test configs registering zero install steps,
  which is API-visible and future-proof.
- U-B3.1-1 (debug link modules): still untouched by
  this step; the debug 2011/0 + metadata re-proof
  stands as before.
- Full-script all-green run now on record (§10.3):
  independent re-proof `SUMMARY: 0 FAIL` in 1448s,
  superseding the R1+R2+R3 composite as the
  single-run proof. Runtime: ~25 min (full run
  measured 1448s), ~6 min `--quick` warm.
- B3.5+ candidates: `.gitignore`-in-scope prefix
  rename (`out/` → symmetric names); depfile-driven
  per-header fan-in report (the `.d` pool already
  contains it); scratch-tree `verify_b1.py` (leave
  the live cache alone); same-profile concurrency
  (still excluded).

## 11. B3.5 gate record (CI wiring + carried closes, 2026-10-11)

Own diffs: `.github/workflows/zig-macos.yml` (new),
`tools/build/ci_record.py` (new),
`tools/legacy-baseline/logs/` (14 preserved small logs +
README), `doc/b3-profiles.md` only. `build.zig` is
UNTOUCHED — no CI need demanded a graph change, so none
was made. `lib/`/`pal/`/`bin/`/`test/`/CMake/other docs
clean per `git status` (§12.3). The pre-existing
`.github/workflows/agreement.yml` (CLA bot) is untouched;
legacy `azure-pipelines.yml` / `.cirrus.yml` are untouched
comparison data (plan §2.1), not reproduced.

### 11.1 Workflow design (`.github/workflows/zig-macos.yml`)

Triggers: `push`, `pull_request`, `workflow_dispatch` (with
an `incremental: quick|full` choice input, §11.2).
`permissions: contents: read`; per-ref concurrency with
`cancel-in-progress`; `PYTHONUNBUFFERED=1` globally.

Runner: `macos-26` (ARM64), pinned by major — never
`macos-latest` — so an image retirement cannot silently
move the toolchain (`macos-14` retires 2026-11-02;
`macos-26` GA since 2026-02-26). Verified against the
`actions/runner-images` macos-26-arm64 readme (image
20260907.0351.1, macOS 26.6.2): it carries Xcode 26.6
(default) AND Xcode 26.5 (build 17F42 — byte-same as the
local B3 toolchain), both shipping macOS SDK 26.5, plus
preinstalled cmake 4.4.3, ninja 1.13.2, python 3.14.7 and
AppleClang 21.0.0. The workflow explicitly selects
`Xcode_26.5.app` (the default 26.6 also ships SDK 26.5
today, but its AppleClang may drift; the pin removes
that variable) and then ASSERTS SDK 26.5, failing loudly
with a listing of every installed Xcode/SDK when the pin
rots. Only `icu4c` is brew-installed; cmake/ninja/clang/
python presence is asserted and their versions RECORDED,
not pinned. Zig comes from `jdx/mise-action@v4`, which
installs exactly what repo-root `mise.toml` pins (0.17.0)
— one pin for laptop and CI — and every zig invocation
spells `mise exec -- zig ...`, identical to the local
recipe. Action pins: `actions/checkout@v5` (node24),
`actions/upload-artifact@v6` +
`actions/download-artifact@v6` (same major: the v4+
artifact wire format must match on both halves),
`jdx/mise-action@v4` (all verified current 2026-10-11;
floating major tags — SHA pins are dependabot territory,
explicitly deferred).

Jobs (matrix legs run on SEPARATE runners — §11.3):

| Job | Steps | Tally (job output + artifact) |
|---|---|---|
| `zig` × test/debug-static/release-shared | setup anchors → `ci_record inputs` → `zig build` → `ci_record manifest` → `zig build check` → `zig build check-selftest` → tally greps | `test: regression=1959/0 …`, `debug-static: regression=2011/0 …`, `release-shared: hello=PASS …` |
| `b1-contract` | setup → `ci_record inputs` → `verify_b1.py --log` → tally | `b1-contract: 14/14 PASS` in a clean checkout; optional reference comparison adds a 15th check |
| `legacy-test` | setup (no zig) → legacy inputs → cmake configure (F0 recipe) → `ninja` → `ninja check` → tally | `legacy-test: regression=1959/0 …` |
| `incremental` | setup → `test_incremental.py --quick` (or full on dispatch) → tally | `incremental(quick): 0 FAIL` |
| `record` | `needs` all four → download all artifacts → assemble `b3-evidence/` (`SUMMARY.md`, tallies, inputs, manifests) → upload | the bundle |

Setup steps (checkout, mise, xcode-select, brew icu4c)
are YAML-anchored in the `zig` job and aliased by the
other three build jobs (legacy aliases three of four —
no zig). Per-leg `ci-evidence/` uploads carry inputs
(`inputs.json`/`.md`, full `zig-env.txt`), the artifact
manifest (sizes + sha256 + symlink targets), the tally,
and the raw `build.log`/`check.log`/`selftest.log`.
Upload steps carry `if: always()` so a red leg still
leaves its partial evidence. The `record` job consumes
ARTIFACT files, not `needs.*.outputs` (matrix-leg outputs
are not reliably addressable per leg); job outputs still
carry each tally for the UI, and every tally step also
writes `$GITHUB_STEP_SUMMARY`. The manifest-before-check
step ordering is load-bearing: `check` litters
`<prefix>/bin/_ttdlog/` with ~400 time-travel trace
files (created beside the running `ch` per
`bin/ch/Helpers.cpp:590-594`) that the manifest must not
record, and later copy-out orphan-reaping never clears
them (out-of-manifest droppings survive per §10.1).

`--prefix` appears NOWHERE in the workflow — the CI pin
for carried close (a): ONLY the test profile honors
`--prefix` (§10.1; build.zig cannot read it in Zig 0.17,
so any non-test `--prefix` contract would be
convention-only). CI uses default prefixes everywhere:
test lands in `zig-out/`, the other two in their fixed
`out/<profile>/` side prefixes. A future author adding
`--prefix out/...` to "isolate" a leg would silently do
nothing for non-test profiles — this paragraph is the
pin against that mistake.

Artifacts expire on GitHub's retention clock; the
PERMANENT record is the quoted outputs in §§8–12, not
the artifact zip. `ci-evidence/` exists only on CI
runners (and `/tmp` in the local sim) — it is never
committed (`.gitignore` is outside B3.5's scope, so no
ignore entry was added; see §12.4).

### 11.2 The incremental-scope decision

`test_incremental.py` runs `--quick` (isolation, noop,
bzcomment, selection) on every push/PR; full (~25 min
warm on 10 cores locally, ~45+ min on a 3-CPU runner)
runs via `workflow_dispatch` with `incremental: full`
(a shell-side `if` on the rendered input passes no flag
for `full` and `--quick` for every other event,
including the dispatch default — never a GHA `&&`/`||`
expression, whose falsy `''` middle would deaden the
`full` hatch). Either choice is attackable, so the
reasoning is on the record:

For quick-every-run: CI's marginal value is a regression
tripwire on an always-clean tree, and `--quick` still
cold-builds two profiles from scratch (isolation),
proves no-op rebuilds change zero bytes with zero TU
compiles (noop), proves build-script edits do not
spuriously invalidate the graph (bzcomment), and keeps
the B1 archive-selection regression pinned (selection).
The expensive legs — header fan-in counts, the
scratch-tree full rebuild, exact jstoc counts,
concurrent builds — prove LOCAL incremental semantics
whose breakage surface is `build.zig`/depfile edits,
which the maintainer-local full gate (recorded 0 FAIL
in 1448s, §10.3) covers before merge.

Against full-every-run: ~45+ runner-minutes per push for
legs whose failure mode is already gated locally, on
runners with a third of the local CPU count.

Escape hatches, both deliberate: any maintainer can run
the full depth on demand from the Actions tab (no code
change), and the local full gate stays the normative
proof (its command is one line: `python3
tools/build/test_incremental.py`). If full-on-dispatch
goes stale from disuse, that is visible (no runs) —
unlike a silently-skipped local gate.

### 11.3 verify_b1 live-tree wipe race (§9.7): CLOSED (2026-10-12 review)

**Superseded by the 2026-10-12 review:** `verify_b1.py` now builds into its
own temporary cache/install prefix, with child-process cleanup before those
directories are released. It does not wipe the live `.zig-cache/zig-out`.
A complete 15/15 run and a concurrent debug-static metadata check passed.
The account below describes the original B3 implementation and decision.

The race: `verify_b1.py` starts by wiping the live
`.zig-cache/` and `zig-out/` and rebuilding (§9.7:
`zig-out/bin/` vanished mid-run under a concurrent
gate, which failed loud rc 2 missing-file and passed on
retry). B3.5's decision (carried close (b)) is to
DOCUMENT the scope and workaround, with ZERO
`verify_b1.py` diff this step.
Reasons, for the reviewers who will judge this call:

No small fix closes it. A lock honored only by
`verify_b1.py` guards B1-vs-B1 and nothing else — `zig
build`/`zig build check` cannot take a Python lock
without a wrapper at every invocation, and an unenforced
wrapper is theater with a failure mode (stale locks)
worse than the disease. The REAL fix is scratch-tree
verification (copy the tree minus build outputs, verify
there, leave the live cache alone) — already listed as
a B3.5+ candidate in §10.6, and it is NOT small: it
moves every path, timeout and failure expectation in
the B1 gate and needs its own 15/15 proof. Doing that
inside the CI step, unasked and unreviewed as B1
surgery, risks the one contract B3 must not wobble.

Scope rule (normative): SEQUENTIAL builds only on a
live tree. Never run `verify_b1.py` concurrently with
any `zig build*` in the same checkout — including the
`out/<profile>/` consumers (their artifacts survive a
wipe, but their cache entries do not, forcing a
rebuild). CI is STRUCTURALLY immune: every job runs on
its own runner with its own checkout, so no two writers
ever share a tree — the workflow needs no lock because
the race cannot occur there.

Workaround, exact: run B1 alone. Its failure signature
under violation is loud, never silent — a concurrent
consumer dies rc 2 on a missing file mid-run (observed
in §9.7) — and passes on retry after B1 finishes. No
silent corruption mode is known: the wipe only deletes
regenerable outputs, and every gate re-asserts outputs
from scratch.

### 11.4 Baseline /tmp-log rot: small logs preserved, the rest repointed

Disposition (carried close (c)): preserve genuinely
small, repoint the rest. `tools/legacy-baseline/logs/`
now holds 14 files (~18 KB), each `cmp`-identical to
its `/tmp/b3-baselines/` original: `00-progress.log`,
`08/09-*-configure.log`, `11-debug-check.log`
(2011/0), `13-release-check.log` (smoke gate), the four
`.time` files, and the five `rerun/` exit-code re-runs.
Its README names the set, the reason (exit-code
evidence that rots with /tmp), and the regeneration
recipe for everything excluded: the two ~60KB verbose
build logs (`LOG=<dir> run-baselines.sh`), the ~1.6MB
zig verbose logs (`zig build --verbose`), and the
`/tmp/b33-*` / `/tmp/b34-incr-logs` ephemera (every
durable fact already quoted in §§9–10; B3.5 CI
artifacts supersede them). Nine citations in this
document were repointed accordingly (§5 results
paragraph; §6 both compile-line paragraphs; §9
preamble, §9.2, §9.7 twice; §10.3, §10.4) — each keeps
its history and gains a `B3.5:` note, none was
rewritten. (§9.1 carries zero /tmp citations; the §9
hit sits in the section preamble.)

### 11.5 Validation: lint + local simulation (no push authorized)

Static: `actionlint` v1.7.12 (built from source with
go1.27.1): ZERO findings on `zig-macos.yml`, before and
after the §11.5 fixes. Python YAML parse: 5 jobs, 12 +
9 + 9 + 9 + 3 steps, anchors resolve (the b1 checkout
alias is byte-equal to the zig checkout step), every
step is a `run`/`uses` mapping, every `uses` pinned
with a major tag. (PyYAML reads bare `on:` as boolean
True — a YAML 1.1 artifact; Actions reads YAML 1.2,
where it is `on`.) Schema eyeballed against verified
sources: the `macos-26` runner + Xcode/SDK table
(runner-images readme, image 20260907.0351.1), action
major currency (checkout v5, upload/download v6,
mise-action v4), download-all (`path`-only) semantics,
matrix outputs deliberately unconsumed (artifact files
instead), `if: always()` uploads, dispatch choice
input. No `yamllint` — actionlint + parse + eyeball is
the chain, stated so the next author knows its links.

Dynamic: `/tmp/b35-ci-sim/sim.sh` executes every
build/check/tally `run` block sequentially and literally
(`matrix.profile` substituted per leg; `GITHUB_*`
redirected to files; the runner-only `sudo xcode-select`
replaced by a select-state assert since local
`Xcode.app` is already 26.5; `brew install icu4c` run
literally — exit 0, already installed). NOT sim-executed:
the `record`-job copy/summary loops (S10 mirrors the
tally loop's shape with sim-local paths — the
inputs/manifests-copy loops and the step-summary loop
were eyeball-verified) and every `uses:` step
(checkout/mise/upload/download have no local equivalent
— action-verified). Per-step `--- <name> exit: N` lines
are the true record (the phase footers always print
`exit 0` — a harness wart, not evidence). Result:
`SIMULATION COMPLETE: FAILED=1` with EXACTLY two
failing steps, both predicted before S5 ran (below);
every other step exit 0:

| Phase | Wall | Result |
|---|---|---|
| S0 setup asserts | 6s | zig 0.17.0, Xcode 26.5, SDK 26.5, icu4c@78 78.3 |
| S1 inputs ×5 | 3s | `ci_record --self-test` + inputs for 3 profiles + b1 + incr legs |
| S2 clean | 13s | `.zig-cache`/`zig-out`/`out/<non-test>` wiped (`out/test262*` kept) |
| S3 zig test | 273s | cold build + manifest + check 1959/0 + selftest 4/4 + tally |
| S4 zig debug-static | 408s | cold build + manifest + check 2011/0 + selftest 4/4 + tally |
| S5 zig release-shared | 239s | build + manifest + check + selftest green; 2 TALLY GREPS RED (the bug) |
| S6 b1-contract | 180s | `SUMMARY: 15/15 PASS` |
| S7 legacy-test (fresh `build-ci-test/`, F0 recipe) | 164s | configure + build + check 1959/0 + shared PASS (dir removed at S9) |
| S8 incremental `--quick` | 308s | `SUMMARY: 0 FAIL` (incl. a cold debug-static rebuild post-B1-wipe) |
| S9 final test `check` on the end-state tree | 69s | 1959/0 again |
| S10 record assembly | 0s | 6/6 tallies assembled |

Tallies (legs `*/tally.txt`, release re-proofed):

```
test: regression=1959/0 bytecode=up-to-date native=shared-PASS
debug-static: regression=2011/0 bytecode=up-to-date native=8x-static-PASS
release-shared: hello=PASS native=shared-PASS
b1-contract: 15/15 PASS
legacy-test: regression=1959/0 native=shared-PASS
incremental(quick): 0 FAIL
```

Manifest highlights (sizes + sha256 recorded per file):
test 14 files — ch 786,288 B, dylib 21,274,888 B,
static archive 1,702,175,808 B; debug-static 10 files
— ch 186,090,096 B, archive 1,699,294,408 B, no dylib;
release 15 files — ch 719,024 B, dylib 16,968,200 B,
archive 225,531,912 B. Legacy F0 artifacts: ch
667,552 B sha256
`f7b65040…bd9c85d1`, dylib 17,621,184 B sha256
`a9cd9523…301d9485`.

Size note: debug-static `ch` (186MB) is 4× its legacy
Debug twin (45.9MB) with near-identical per-TU code
(sample TU `__text` 837KB vs 811KB) because the delta
is the linker, not the flags — neither side strips,
runs dsymutil, or dead-strips (verified in both link
lines), but Zig's Mach-O linker keeps
every input atom (90MB `__text` ≈ the 87MB member sum,
plus the full 33MB `__eh_frame`) while system `ld`
coalesces per-TU weak copies down to 17MB; proven by
relinking our own archive with system `ld` (49MB
total). Cosmetic: no size gate exists, release `ch`
matches legacy (719KB vs 0.57MB), all gates green.

THE SIM CAUGHT TWO REAL BUGS PRE-MERGE (as designed —
this is the validation working, not failing):

1. Release tally greps expected literal `hello world` /
   `PASS` lines, but `check_smoke.py` logs the hello
   leg as a one-line repr (`check_smoke: hello -> rc=0
   out='hello world\nPASS\n'`). Predicted from a
   read of the script before S5 ran; S5 failed exactly
   there and nowhere else. Fixed in workflow + sim
   (two `grep -F` lines: the repr line and the `PASS:
   ... + hello.js outputs match` summary) and RE-PROVED
   with a full release-leg re-run: build + manifest +
   check + selftest + fixed tally, `FAILED=0`.
2. Legacy shasum line pointed at
   `build-ci-test/bin/libChakraCore.dylib`, which does
   not exist — the dylib lands at the build ROOT — and
   a trailing `|| true` masked it (shasum.txt held only
   the ch line). Fixed to the exact root path with the
   `|| true` REMOVED (both files asserted), and the
   fixed line executed literally against the live tree
   before S9 removed it (both hashes above).

Honesty bounds: the sim proves the recipe on THIS
machine (10-core M1 Max, warm sources, SDK 26.5,
icu4c@78 78.3, cmake 4.4.0, python 3.12) — not the
runner reality (3-CPU `macos-26`, python 3.14, cmake
4.4.3, cold brew install). The first real Actions run
happens post-merge; THAT run is the workflow's own
proof. The sim also runs sequentially while CI runs
parallel legs — the recipe is identical, the schedule
is not (and the parallel schedule is what makes CI
immune to §11.3's race).

## 12. Final B3 gate record + scoped Zig-default rollout (2026-10-11)

### 12.1 B3 Done, clause by clause (plan §B3)

- Each declared profile passes its matching checks:
  test 1959/0 + bytecode + shared native (§8.5, §9.6,
  sim S3+S9); debug-static 2011/0 + bytecode + 8 static
  native (§8, sim S4); release smoke gate — hello leg
  + shared embedding + metadata (§9, sim S5 re-proof).
  Static native tests are NOT assumed covered by the
  shared build's eight expected skips: eight static
  suites EXECUTE to PASS against the static archive
  (§8.2). GREEN.
- Source/header/JS-generator/configuration changes
  rebuild their dependents (exact TU counts + green),
  flavor switches isolate artifacts (fixed side
  prefixes — switching deletes nothing), and no-op
  rebuilds behave (zero TU compiles, zero artifact
  byte changes): the §10.3 full run (`SUMMARY: 0
  FAIL` in 1448s) plus the §11.5 `--quick` re-proof.
  GREEN.
- Clean CI runs record inputs and outputs
  (`zig-macos.yml` + `ci_record.py`: zig version/env,
  target, flavor, defines, dependency versions,
  artifact sizes/hashes, gate tallies as outputs +
  artifacts) and keep the equivalent legacy leg green
  (fresh-dir F0 recipe, 1959/0, sim S7). GREEN
  modulo the post-merge first run (§11.5 bounds).
- Release artifacts have output-checked,
  release-compatible smoke/embedding checks (§9.2,
  sim S5); the legacy Release `ninja check` is only a
  smoke gate, mirrored exactly — no regression or
  bytecode legs by legacy design
  (`test/CMakeLists.txt:23-45`, §9.3). GREEN.

B3 Done (CONDITIONAL): ALL CLAUSES GREEN on this host;
the verdict is final only on the first green post-merge
Actions run (zero runs have occurred — the sim proves
the recipe on this host only, §11.5 bounds).

### 12.2 Scoped Zig-default rollout decision

Per plan §B3 ("Default the Mac route to Zig only for
profiles meeting these gates"): ALL THREE declared Mac
profiles meet their gates on this host, so the Mac
route defaults to Zig for EXACTLY these configurations
— native ARM64 macOS, no-JIT, system ICU, minos 26.0,
SDK 26.5, Zig 0.17.0 — CONDITIONAL on the first green
post-merge Actions run (same condition as §12.1):

| Default Zig | CMake equivalent | Library |
|---|---|---|
| `zig build` (test) | RelWithDebInfo | shared |
| `zig build -Dprofile=debug-static` | Debug + `STATIC_LIBRARY` | static |
| `zig build -Dprofile=release-shared` | Release | shared |

What "default" means mechanically: there is no flag to
flip — it means docs and CI point at `zig build` for
these profiles, and the legacy cmake recipes will
stay as comparison data; the legacy Test leg will be
kept green in CI. NOT defaulted — staying
legacy-or-unsupported until their own named gates
pass: no-ICU, embedded ICU, tracing, plugins, extra
architectures, any JIT mode, and every non-Mac
platform (B4 owns Linux).

### 12.3 B3.5 scope audit

New: `.github/workflows/zig-macos.yml`,
`tools/build/ci_record.py`,
`tools/legacy-baseline/logs/` (14 logs + README).
Edited: `doc/b3-profiles.md` (9 citation repoints +
§§11–12). `build.zig`: ZERO B3.5 diff (its `M` below
is the stacked B3.1–B3.4 work — no CI need demanded a
graph change). Final `git status --short`:

```
 M .gitignore
 M build.zig
 M tools/build-contract/verify_b1.py
 M tools/build/check_metadata.py
 M tools/build/check_native.py
 M tools/build/check_regression.py
 M tools/build/check_smoke.py
 M tools/build/tables.zig
 M tools/pal-config/check.zig
?? .github/workflows/zig-macos.yml
?? doc/b3-profiles.md
?? tools/build/ci_record.py
?? tools/build/install_profile.py
?? tools/build/test_incremental.py
?? tools/legacy-baseline/
```

That is exactly the B3.1–B3.4 set plus the B3.5 set.
`lib/`/`pal/`/`bin/`/`test/`/CMake/other-docs: ZERO
diff. `build-ci-test/` (sim-only) removed; `out/`,
`.zig-cache/`, `zig-out/` ignored; `ci-evidence/`
never touched the repo (sim wrote to `/tmp`).

### 12.4 Open uncertainties carried forward

- The first post-merge Actions run validates runner
  reality: `Xcode_26.5.app` presence, the icu4c
  bottle, and 3-CPU timings (expect ~2–3× local
  walls; timeouts are ceilings at 60–150 min).
- `ci-evidence/` has no `.gitignore` entry
  (`.gitignore` outside B3.5's scope) — a future
  step with it in scope should add it beside the
  `/out/` precedent.
- The B1 live-tree wipe race is closed by private cache/install outputs
  (2026-10-12 review); same-profile installation concurrency stays excluded.
- Action SHA pins are dependabot territory (floating
  majors documented in §11.1).
- Full-on-dispatch staleness watch: visible as "no
  runs" (§11.2).
- U-B3.1-1 (debug link modules): untouched since
  B3.1; the debug 2011/0 + metadata re-proofs stand.
- STALENESS NOTE (found during B3.5, out of scope to
  fix): `doc/b2-verification.md`'s `build.zig:N`
  refs predate the B3.1–B3.4 graph growth (e.g. the
  TU-count assert it cites at `:417` now lives at
  `:570`) — that doc is
  frozen to B3.5, so re-pinning belongs to a future
  docs pass. THIS document carries no `build.zig:N`
  refs (verified by grep 2026-10-11), so B3.5 adds
  no new line-number rot.
