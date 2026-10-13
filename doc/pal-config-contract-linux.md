# PAL configuration contract (Linux reference profile)

B4.2 — the per-setting PAL configuration contract for exactly ONE
profile: native x64 Linux, JIT enabled, RelWithDebInfo, system ICU, in
the B4 container. Source of truth: `pal/src/config.h.in`,
`pal/src/configure.cmake`, and `Build/CMakeFeatureDetect.cmake` at
`1eb0a7bc1` on branch `modernize/baseline` (the B4.1 snapshot; those
files are byte-identical to the F0 engine baseline `97e68c917` per
[b4-linux.md](b4-linux.md) §1), cross-checked against the reference
build's generated `config.h`.

Reference generated header used:
`/tmp/chakra-b4/build-xplat/pal/src/config.h`
(MD5 `f73cd5b2968beaad6b6266a2cb61b06a`, 4839 bytes, VM-only, not
committed; cited by content hash). Derivation sources: that header,
`build-xplat/CMakeCache.txt` (probe `INTERNAL` results),
`build-xplat/CMakeFiles/CMakeConfigureLog.yaml` (3975 lines: exact
probe command lines, diagnostics, exit codes), and the three CMake
sources above.

Conventions: every value cites a source line or an observed generated
value. Items that could not be pinned down are marked **UNCERTAIN** with
the reason. Nothing here is invented: environment-local values (image
digest, package versions) are recorded as observed in
[b4-linux.md](b4-linux.md) §1. Where a probe's source or citation is
identical to the Mac contract, this doc cites the same
`configure.cmake` lines and records only the Linux value and mechanism.

## 0. Profile and assumptions

Reference profile (same environment as [b4-linux.md](b4-linux.md)):

- Target: x86_64 Linux (`CMAKE_SYSTEM_NAME=Linux`,
  `CMAKE_SYSTEM_PROCESSOR=x86_64`). Native toolchain view; the build
  host is qemu-user emulation (see S1). No cross-compilation, no
  Android branch (`pal/src/configure.cmake:3` takes the probe path),
  not Darwin/FreeBSD (the `else() # Anything else is Linux` branch at
  `configure.cmake:904-915`).
- Configure: `cmake -S /src -B /src/build-xplat -GNinja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_C_COMPILER=clang` ([b4-linux.md](b4-linux.md) §3 stage 2).
  No `-D` overrides for any `HAVE_*`/`HAS_*`/`SIZEOF_*`/`MMAP_*`/
  `PLATFORM_*` variable were passed (verified: no non-`INTERNAL`
  entries for those prefixes exist in `CMakeCache.txt`).
- Toolchain (observed): clang 18.1.3 (`1:18.0-59~exp2`, target
  `x86_64-pc-linux-gnu`), CMake 3.28.3, Ninja 1.11.1, Ubuntu 24.04,
  glibc 2.39 (`ldd (Ubuntu GLIBC 2.39-0ubuntu8.9) 2.39`), kernel
  6.8.0-139 (lima VM), qemu-user 8.2.2.
- Feature resolution: `DISABLE_JIT:BOOL=OFF` (JIT on),
  `ENABLE_DEBUG_CONFIG_OPTIONS=1` (`CMakeLists.txt:507`,
  RelWithDebInfo branch), `CAN_BUILD_WABT` defined (observed on TU
  command lines; AMD64 branch `CMakeLists.txt:282-284`).
- ICU: system libicu-dev 74.2 (`74.2-1ubuntu3.1`) via default search;
  no `-DICU_INCLUDE_PATH` was passed. Unlike Mac, the
  `CMakeLists.txt:598-602` short-circuit did NOT trigger (it is gated
  on `ICU_INCLUDE_PATH`), so `HAVE_LIBICU_UCHAR_H` was resolved by
  running the `check_include_files(unicode/uchar.h …)` probe at
  `configure.cmake:36` (cache `INTERNAL=1`), and the `:917-922` gate
  passed. It is not a `config.h` setting (see §4).
- `Build/CMakeFeatureDetect.cmake` contributes no `config.h` setting:
  it only probes `CLANG_HAS_DISABLE_TAIL_CALLS_CFG` (`:17-27`;
  cache `1`, `COMPILED=TRUE`, `EXITCODE=0`) and, gated by `:29`,
  emits `-DCLANG_HAS_DISABLE_TAIL_CALLS=1` (`:30`).
- Global probe inputs: `configure.cmake:20` sets
  `CMAKE_REQUIRED_DEFINITIONS` to `-D_DEFAULT_SOURCE
  -D_POSIX_C_SOURCE=200809L` (non-Darwin, non-FreeBSD), and `:23`
  appends `-D_FILE_OFFSET_BITS=64`, so every `configure.cmake` probe
  below compiled with all three (visible on all probe compile lines
  in `CMakeConfigureLog.yaml`). `CMAKE_C_FLAGS` is empty (observed
  `CMAKE_C_FLAGS: ""` in the log), so C probes carry no `-std`,
  no `-Werror` and no `-O` flag. `CMAKE_CXX_FLAGS` is `-Werror` plus
  the 25 `-Wno-*` suppressions (`CMakeLists.txt:330-336`, Linux-only;
  `:410-436`) plus, for `configure.cmake` probes,
  `-fno-omit-frame-pointer -fdelayed-template-parsing`
  (`CMakeLists.txt:453-457`); C++ probes additionally carry
  `-std=gnu++17` (`CMAKE_CXX_STANDARD`). The tail-calls probe keeps
  `-Werror` plus suppressions and `-std=gnu++17` but lacks the three
  required defines (separate directory scope) and lacks
  `-fno-omit-frame-pointer`/`-fdelayed-template-parsing` (it runs
  before those are appended at `:453-457`) — all confirmed on its
  log line.
- `func_tmpl` CMake-version provenance: the re-exported
  function-probe template is documented as CMake 4.4.0's
  `Modules/CheckFunctionExists.c` verbatim (Mac
  `tools/pal-config/probes.zig:10-12`), while the Linux reference
  ran CMake 3.28.3. Verdicts are safe regardless: func/libfunc
  verdicts are determined by compile-plus-link success of a
  declare-and-reference stub — absent functions fail at link (§2.2),
  and `check.zig`'s `compileAndMaybeRun` likewise passes them on
  compiler exit success with no execution — not by template wording.
  (B4.3 directly compared the two templates: identical modulo the
  trailing newline.) B4.3 adds the link-input half of that sentence:
  success must be measured against the real system objects (L14),
  since the bundled stubs over-export version-retired symbols.

## 1. Classification and B4 consequence

| Classification | Meaning | B4 consequence |
|---|---|---|
| FIXED-BY-TARGET | Value follows from the declared target (Linux branch, unconditional non-Android set, or template literal); no probing involved | emit literal |
| COMPILE-CHECKED | Value comes from a compile/link-only probe (`check_include_files`, `check_function_exists`, `check_library_exists`, `check_struct_has_member`, `check_type_size`, `check_cxx_symbol_exists`) | emit via Zig check |
| NATIVE-RUNTIME-VERIFIED | Value comes from `check_cxx_source_runs`: the probe must be executed natively | verify at build time |

Semantics for the Linux build (see §5 for the fail-clearly spec):

- **emit literal**: the Zig build writes the recorded value with no
  check. The guard is the target gate: these literals are valid only
  for the declared `x86_64-linux-gnu` profile.
- **emit via Zig check**: the Zig build performs an equivalent
  compile/link check at build time and emits its result. Any deviation
  from the recorded value fails the build with the diff (fail-clearly,
  not silent drift). The C++ checks MUST reproduce the inherited
  `-Werror`: three recorded values depend on it (§3 L1-L3).
- **verify at build time**: the Zig build compiles, runs natively, and
  requires the probe outcome to equal the recorded value; any
  difference fails the build with the probe identity, expected value,
  and observed value. This profile is native-only, so build machine
  equals target; cross builds are rejected (see §5).

Counts: FIXED-BY-TARGET 33, COMPILE-CHECKED 62,
NATIVE-RUNTIME-VERIFIED 32 — 127 unique settings covering all 128
emitted `#define`/`#undef` lines (`HAS_FTRUNCATE_LENGTH_ISSUE` is
emitted twice from one variable, §3 quirk L12). The counts match Mac
by construction (same template, same probe list); the membership and
most values differ.

## 2. Settings

Emission template (`config.h.in`: 101 `#cmakedefine01` + 10 plain
`#cmakedefine` lines): `01` → `#define X 0`/`1`; plain set →
`#define X value`; plain unset → `/* #undef X */`.

### 2.1 FIXED-BY-TARGET (33) — emit literal

Unconditional non-Android sets (`configure.cmake:871-872`):

| Setting | Value | Source |
|---|---|---|
| `SYNCHMGR_SUSPENSION_SAFE_CONDITION_SIGNALING` | `1` | `configure.cmake:871` unconditional `set` |
| `ERROR_FUNC_FOR_GLOB_HAS_FIXED_PARAMS` | `1` | `configure.cmake:872` unconditional `set` |

Linux branch (`configure.cmake:904-915`; justification: the reference
profile satisfies neither the Darwin nor the FreeBSD branch):

| Setting | Value | Source |
|---|---|---|
| `DEADLOCK_WHEN_THREAD_IS_SUSPENDED_WHILE_BLOCKED_ON_MUTEX` | `0` | `configure.cmake:905` Linux branch |
| `PAL_PTRACE` | `ptrace((cmd), (pid), (void*)(addr), (data))` | `configure.cmake:906` Linux branch |
| `PAL_PT_ATTACH` | `PTRACE_ATTACH` | `configure.cmake:907` Linux branch |
| `PAL_PT_DETACH` | `PTRACE_DETACH` | `configure.cmake:908` Linux branch |
| `PAL_PT_READ_D` | `PTRACE_PEEKDATA` | `configure.cmake:909` Linux branch |
| `PAL_PT_WRITE_D` | `PTRACE_POKEDATA` | `configure.cmake:910` Linux branch |
| `JA_JP_LOCALE_NAME` | `"ja_JP_LOCALE_NOT_FOUND"` | `configure.cmake:911` Linux branch |
| `KO_KR_LOCALE_NAME` | `"ko_KR_LOCALE_NOT_FOUND"` | `configure.cmake:912` Linux branch |
| `ZH_TW_LOCALE_NAME` | `"zh_TW_LOCALE_NOT_FOUND"` | `configure.cmake:913` Linux branch |
| `HAS_FTRUNCATE_LENGTH_ISSUE` | `0` | `configure.cmake:914` Linux branch; emitted twice (`config.h.in:85,127`, see §3 L12) |

Never set on the Linux path (justification: no `set` or probe writes
the variable on any non-Android path outside the Darwin/FreeBSD
branches, so plain `#cmakedefine` emits `/* #undef X */` and
`#cmakedefine01` emits `0`; the reference configure passed no
overriding `-D`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_COREFOUNDATION` | `0` | set only on Darwin (`configure.cmake:875`); observed `0` |
| `HAVE__NSGETENVIRON` | `0` | set only on Darwin (`configure.cmake:876`); observed `0` |
| `FREEBSD_LIBC` | `/* #undef FREEBSD_LIBC */` | set only on FreeBSD (`configure.cmake:899-903`); observed verbatim |
| `STATVFS64_PROTOTYPE_BROKEN` | `0` | no setter anywhere (only an Android-branch comment, `configure.cmake:993`); observed `0` |

Template literals (plain `#define` in `config.h.in`, no CMake
substitution — identical on every platform):

| Setting | Value | Source |
|---|---|---|
| `PAL_THREAD_PRIORITY_MIN` | `0` | `config.h.in:114` literal |
| `PAL_THREAD_PRIORITY_MAX` | `0` | `config.h.in:115` literal |
| `CHECK_TRACE_SPECIFIERS` | `0` | `config.h.in:134` literal |
| `PROCFS_MEM_NAME` | `""` | `config.h.in:135` literal |
| `HAVE_GETHRTIME` | `0` | `config.h.in:136` literal |
| `HAVE_LOWERCASE_ISO_NAME` | `0` | `config.h.in:137` literal |
| `HAVE_READ_REAL_TIME` | `0` | `config.h.in:138` literal |
| `HAVE_UNDERSCORE_ISO_NAME` | `0` | `config.h.in:139` literal |
| `MKSTEMP64_IS_USED_INSTEAD_OF_MKSTEMP` | `0` | `config.h.in:140` literal |
| `NEED_DLCOMPAT` | `0` | `config.h.in:141` literal |
| `OPEN64_IS_USED_INSTEAD_OF_OPEN` | `0` | `config.h.in:142` literal |
| `SELF_SUSPEND_FAILS_WITH_NATIVE_SUSPENSION` | `0` | `config.h.in:143` literal |
| `SET_SCHEDPARAM_NEEDS_PRIVS` | `0` | `config.h.in:144` literal |
| `SIGWAIT_FAILS_WHEN_PASSED_FULL_SIGSET` | `0` | `config.h.in:145` literal |
| `SYNCHMGR_PIPE_BASED_THREAD_BLOCKING` | `0` | `config.h.in:146` literal |
| `WRITE_0_BYTES_HANGS_TTY` | `0` | `config.h.in:147` literal |
| `HAVE_FTRUNCATE_LARGE_LENGTH_SUPPORT` | `1` | `config.h.in:148` literal |

### 2.2 COMPILE-CHECKED (62) — emit via Zig check

Header existence (`check_include_files`, `configure.cmake:25-35`).
Absent headers fail with `fatal error: '<h>' file not found`:

| Setting | Value | Source |
|---|---|---|
| `HAVE_IEEEFP_H` | `0` | `:25`; `ieeefp.h` not found; observed `0` |
| `HAVE_ALLOCA_H` | `1` | `:26`; observed `1` |
| `HAVE_SYS_VMPARAM_H` | `0` | `:27`; `sys/vmparam.h` not found; observed `0` |
| `HAVE_MACH_VM_TYPES_H` | `0` | `:28`; `mach/vm_types.h` not found; observed `0` |
| `HAVE_MACH_VM_PARAM_H` | `0` | `:29`; `mach/vm_param.h` not found; observed `0` |
| `HAVE_PROCFS_H` | `0` | `:30`; `procfs.h` not found; observed `0` |
| `HAVE_CRT_EXTERNS_H` | `0` | `:31`; `crt_externs.h` not found; observed `0` |
| `HAVE_SYS_TIME_H` | `1` | `:32`; observed `1` |
| `HAVE_PTHREAD_NP_H` | `0` | `:33`; `pthread_np.h` not found; observed `0` |
| `HAVE_SYS_LWP_H` | `0` | `:34`; `sys/lwp.h` not found; observed `0` |
| `HAVE_RUNETYPE_H` | `0` | `:35`; `runetype.h` not found; observed `0` |

Function/link existence (`check_function_exists`,
`configure.cmake:38-39,49-72`; `check_library_exists(pthread …)`,
`:40-48` — compile-plus-link checks, no execution). Absent functions
fail at link (`linker command failed with exit code 1`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_KQUEUE` | `0` | `:38`; link fail; observed `0` |
| `HAVE_GETPWUID_R` | `1` | `:39`; observed `1` |
| `HAVE_PTHREAD_SUSPEND` | `0` | `:40` (`pthread`); cache `INTERNAL=` (empty); observed `0` |
| `HAVE_PTHREAD_SUSPEND_NP` | `0` | `:41` (`pthread`); cache empty; observed `0` |
| `HAVE_PTHREAD_CONTINUE` | `0` | `:42` (`pthread`); observed `0` |
| `HAVE_PTHREAD_CONTINUE_NP` | `0` | `:43` (`pthread`); observed `0` |
| `HAVE_PTHREAD_RESUME_NP` | `0` | `:44` (`pthread`); observed `0` |
| `HAVE_PTHREAD_ATTR_GET_NP` | `0` | `:45` (`pthread`); observed `0` |
| `HAVE_PTHREAD_GETATTR_NP` | `1` | `:46` (`pthread`); cache `1`; observed `1` |
| `HAVE_PTHREAD_GETCPUCLOCKID` | `1` | `:47` (`pthread`); cache `1`; observed `1` |
| `HAVE_PTHREAD_SIGQUEUE` | `1` | `:48` (`pthread`); cache `1`; observed `1` |
| `HAVE_SIGRETURN` | `1` | `:49`; glibc provides `sigreturn`; cache `1`; observed `1` |
| `HAVE__THREAD_SYS_SIGRETURN` | `0` | `:50`; link fail; observed `0` |
| `HAVE_COPYSIGN` | `1` | `:51`; observed `1` |
| `HAVE_FSYNC` | `1` | `:52`; observed `1` |
| `HAVE_FUTIMES` | `1` | `:53`; observed `1` |
| `HAVE_UTIMES` | `1` | `:54`; observed `1` |
| `HAVE_SYSCTL` | `0` | `:55`; glibc removed `sysctl`; link fail; observed `0` |
| `HAVE_SYSCONF` | `1` | `:56`; observed `1` |
| `HAVE_LOCALTIME_R` | `1` | `:57`; observed `1` |
| `HAVE_GMTIME_R` | `1` | `:58`; observed `1` |
| `HAVE_TIMEGM` | `1` | `:59`; observed `1` |
| `HAVE__SNWPRINTF` | `0` | `:60`; link fail; observed `0` |
| `HAVE_POLL` | `1` | `:61`; observed `1` |
| `HAVE_STATVFS` | `1` | `:62`; observed `1` |
| `HAVE_THREAD_SELF` | `0` | `:63`; link fail; observed `0` |
| `HAVE__LWP_SELF` | `0` | `:64`; link fail; observed `0` |
| `HAVE_MACH_THREADS` | `0` | `:65` (`pthread_mach_thread_np`); link fail; observed `0` |
| `HAVE_MACH_EXCEPTIONS` | `0` | `:66` (`thread_set_exception_ports`); link fail; observed `0` |
| `HAVE_VM_ALLOCATE` | `0` | `:67`; link fail; observed `0` |
| `HAVE_VM_READ` | `0` | `:68`; link fail; observed `0` |
| `HAS_SYSV_SEMAPHORES` | `1` | `:70` (`semget`); observed `1` |
| `HAS_PTHREAD_MUTEXES` | `1` | `:71` (`pthread_mutex_init`); observed `1` |
| `HAVE_TTRACE` | `0` | `:72`; link fail; observed `0` |

Struct members (`check_struct_has_member`, `configure.cmake:74-77`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_STAT_TIMESPEC` | `0` | `:74` member `st_atimespec` of `struct stat`; `no member named 'st_atimespec'`; observed `0` |
| `HAVE_STAT_NSEC` | `0` | `:75` member `st_atimensec` of `struct stat`; `no member named 'st_atimensec'`; observed `0` |
| `HAVE_TM_GMTOFF` | `1` | `:76` member `tm_gmtoff` of `struct tm`; cache `1`; observed `1` |
| `HAVE_GREGSET_T` | `1` | `:77` member `uc_mcontext.gregs[0]` of `ucontext_t`; glibc `ucontext.h` needs no `_XOPEN_SOURCE` (contrast Mac Q5); cache `1`; observed `1` |

Type sizes (`check_type_size`, `configure.cmake:79-97`). Same naming
indirection as Mac: the `HAVE_*` existence flag is what `config.h.in`
consumes:

| Setting | Value | Source |
|---|---|---|
| `HAVE_BSD_REGS_T` | `0` | `:80` `struct reg` (+`machine/reg.h`); `fatal error: 'machine/reg.h' file not found`; cache `FALSE`; observed `0` |
| `HAVE_PT_REGS` | `1` | `:83` `struct pt_regs` (+`asm/ptrace.h`); cache `TRUE`; observed `1` |
| `HAVE_SIGINFO_T` | `1` | `:86` `siginfo_t` (+`signal.h`); cache `TRUE`; observed `1` |
| `HAVE_UCONTEXT_T` | `1` | `:89` `ucontext_t` (+`ucontext.h`); no SDK `#error` on glibc (contrast Mac Q5); cache `TRUE`; observed `1` |
| `HAVE_PTHREAD_RWLOCK_T` | `1` | `:92` `pthread_rwlock_t` (+`pthread.h`); observed `1` |
| `HAVE_PRWATCH_T` | `0` | `:95` `prwatch_t`; `use of undeclared identifier 'prwatch_t'`; cache `FALSE`; observed `0` (see §3 L13: `:94` sets the singular `CMAKE_EXTRA_INCLUDE_FILE`, so no extra include is in effect) |
| `SIZEOF_OFF_T` | `8` | `:97`; the size variable itself (`SIZEOF_OFF_T:INTERNAL=8`); observed `8` |

Symbol existence (`check_cxx_symbol_exists`,
`configure.cmake:99-104`; C++ probes, so `-Werror` applies, though
none of these failures are warning-related):

| Setting | Value | Source |
|---|---|---|
| `HAVE_YIELD_SYSCALL` | `0` | `:99` symbol `SYS_yield` in `sys/syscall.h`; `use of undeclared identifier 'SYS_yield'`; observed `0` |
| `HAVE_INFTIM` | `0` | `:100` symbol `INFTIM` in `poll.h`; `use of undeclared identifier 'INFTIM'`; observed `0` |
| `HAVE_CHAR_BIT` | `0` | `:101` symbol `CHAR_BIT` in `sys/limits.h`; `fatal error: 'sys/limits.h' file not found` (glibc ships no such header); observed `0` |
| `USER_H_DEFINES_DEBUG` | `0` | `:102` symbol `_DEBUG` in `sys/user.h`; `use of undeclared identifier '_DEBUG'`; observed `0` |
| `HAVE__SC_PHYS_PAGES` | `1` | `:103` symbol `_SC_PHYS_PAGES` in `unistd.h`; cache `1`; observed `1` |
| `HAVE__SC_AVPHYS_PAGES` | `1` | `:104` symbol `_SC_AVPHYS_PAGES` in `unistd.h`; cache `1`; observed `1` |

### 2.3 NATIVE-RUNTIME-VERIFIED (32) — verify at build time

Behavioral probes (`check_cxx_source_runs`, all C++ with `-Werror`).
The variable is defined iff the probe compiles, links, and exits 0;
otherwise `#cmakedefine01` emits `0`. "How" below names the run-time
behavior each probe exercises (probe bodies at the cited lines) —
except L1-L9, which never executed: those cells quote the
compile-failure diagnostic from `CMakeConfigureLog.yaml` instead.
Every exit code below ran under qemu-user emulation (see S1).

| Setting | Value | Source and how |
|---|---|---|
| `REALPATH_SUPPORTS_NONEXISTENT_FILES` | `0` | `:121-139` run: `realpath("a_nonexistent_file", …)` must succeed; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `SSCANF_SUPPORT_ll` | `1` | `:140-148` run: `sscanf("5000000000", "%qu", …)` round-trips; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `SSCANF_CANNOT_HANDLE_MISSING_EXPONENT` | `0` | `:149-163` run: never executes — the probe fails to COMPILE (`:152` initializes `char *strin` with a string literal; `src.cxx:9:18: error: ISO C++11 does not allow conversion from string literal to 'char *' [-Werror,…]`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L2) |
| `HAVE_LARGE_SNPRINTF_SUPPORT` | `0` | `:164-175` run: never executes — the probe fails to COMPILE (`src.cxx:7:3: error: 'snprintf' size argument is too large; destination buffer has size 256, but size argument is 2147483647 [-Werror,-Wfortify-source]`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L1) |
| `HAVE_BROKEN_FIFO_SELECT` | `0` | `:176-228` run: never executes — the probe fails to COMPILE (`:227` exits on `numFD`, declared as `numFDs` at `:187`; `src.cxx:52:10: error: use of undeclared identifier 'numFD'; did you mean 'numFDs'?`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L4) |
| `HAVE_BROKEN_FIFO_KEVENT` | `0` | `:229-286` run: never executes — the probe fails to COMPILE (`src.cxx:9:10: fatal error: 'sys/event.h' file not found`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L8) |
| `HAVE_SCHED_GET_PRIORITY` | `1` | `:288-310` run (links `pthread`): `sched_get_priority_min/max` succeed; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_SCHED_GETCPU` | `1` | `:311-323` run (links `pthread`): `sched_getcpu()` returns `>= 0`; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_WORKING_GETTIMEOFDAY` | `1` | `:325-337` run: `gettimeofday` returns 0; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_WORKING_CLOCK_GETTIME` | `1` | `:338-353` run: `clock_gettime(CLOCK_REALTIME, …)` succeeds (no `__APPLE__` forcing); cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_CLOCK_MONOTONIC` | `1` | `:354-369` run: `clock_gettime(CLOCK_MONOTONIC, …)` succeeds; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_CLOCK_MONOTONIC_COARSE` | `1` | `:370-385` run: `clock_gettime(CLOCK_MONOTONIC_COARSE, …)` succeeds; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_MACH_ABSOLUTE_TIME` | `0` | `:386-397` run: never executes — the probe fails to COMPILE (`src.cxx:3:10: fatal error: 'mach/mach_time.h' file not found`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L9) |
| `HAVE_CLOCK_THREAD_CPUTIME` | `1` | `:398-413` run: `clock_gettime(CLOCK_THREAD_CPUTIME_ID, …)` succeeds; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_MMAP_DEV_ZERO` | `1` | `:414-433` run: `mmap` of `/dev/zero` succeeds; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `MMAP_IGNORES_HINT` | `0` | `:434-474` run: `MAP_FIXED` mapping at a freed hint lands on the hint (Linux honors it), so the probe exits 1; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `MMAP_ANON_IGNORES_PROTECTION` | `0` | `:475-514` run: never executes — the probe fails to COMPILE (`:486` declares `void *handle_signal(int)`, assigned to `void (*)(int)` at `:501`; `src.cxx:27:23: error: incompatible function pointer types assigning to '__sighandler_t' (aka 'void …`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L5) |
| `MMAP_DOESNOT_ALLOW_REMAP` | `0` | `:515-542` run: `MAP_FIXED` remap of an anon page succeeds, so the probe exits 1; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `ONE_SHARED_MAPPING_PER_FILEREGION_PER_PROCESS` | `0` | `:543-596` run: two shared mappings of one file region both succeed, so the probe exits 1; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `PTHREAD_CREATE_MODIFIES_ERRNO` | `0` | `:597-618` run (links `pthread`): `pthread_create` leaves `errno` zero, so the probe exits 1; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `SEM_INIT_MODIFIES_ERRNO` | `0` | `:620-641` run (links `pthread`): `sem_init` leaves `errno` at 50, so the probe exits 1; cache `COMPILED=TRUE EXITCODE=1`; observed `0` |
| `HAVE_PROCFS_CTL` | `0` | `:643-665` run: never executes — the probe fails to COMPILE (`:660` passes the undeclared placeholder `$2` to `open`; `src.cxx:18:19: error: use of undeclared identifier '$2'`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L6) |
| `HAVE_COMPATIBLE_ACOS` | `1` | `:666-676` run (links `m`): `acos(10)` is NaN; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_COMPATIBLE_ASIN` | `1` | `:678-688` run (links `m`): `asin(10)` is NaN; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_COMPATIBLE_ATAN2` | `1` | `:690-720` run (links `m`): signed-zero `atan2` quadrant behavior; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_COMPATIBLE_EXP` | `0` | `:722-740` run (links `m`): never executes — the probe fails to COMPILE (`memcmp` used without `<string.h>`; `src.cxx:14:7: error: use of undeclared identifier 'memcmp'`; cache `COMPILED=FALSE`); observed `0` (see §3 L7) |
| `HAVE_COMPATIBLE_LOG` | `1` | `:742-752` run (links `m`): `log(-10000)` is NaN; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAVE_COMPATIBLE_LOG10` | `1` | `:754-764` run (links `m`): `log10(-10000)` is NaN; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `UNGETC_NOT_RETURN_EOF` | `1` | `:766-791` run: `ungetc` on an append-mode stream returns the char (glibc), so the probe exits 0; cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `HAS_POSIX_SEMAPHORES` | `1` | `:792-804` run (links `pthread`): `sem_init` succeeds (glibc supports unnamed semaphores); cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `GETPWUID_R_SETS_ERRNO` | `1` | `:806-833` run: `getpwuid_r` with a 1-byte buffer sets `errno` to `ERANGE` (glibc); cache `COMPILED=TRUE EXITCODE=0`; observed `1` |
| `FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL` | `0` | `:834-868` run: never executes — the probe fails to COMPILE (`src.cxx:8:20: error: ISO C++11 does not allow conversion from string literal to 'char *' [-Werror,…]`; cache `COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 L3) |

## 3. Reference quirks (recorded as-is)

- **L1** — `HAVE_LARGE_SNPRINTF_SUPPORT=0` records the Linux
  `-Werror`, not `snprintf()` behavior: the probe at
  `configure.cmake:164-175` passes a 2 GiB size for a 256-byte
  buffer, and clang 18 rejects it under `-Wfortify-source` with the
  inherited `-Werror` (`COMPILED=FALSE` in cache; diagnostic quoted
  in §2.3). Mac records `1` for the same source (no `-Werror` there).
  The value is certain as observed; a future flag or probe-source fix
  would flip it, which is exactly the drift §5 treats as a
  fail-clearly event.
- **L2** — `SSCANF_CANNOT_HANDLE_MISSING_EXPONENT=0` records the
  Linux `-Werror`, not `sscanf()` behavior: the probe at
  `configure.cmake:149-163` initializes `char *strin` with
  `"12.34e"`, and clang 18 rejects the conversion with the inherited
  `-Werror` (`COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in
  §2.3). Mac records `0` too, but by execution (the probe ran and did
  not exit 0) — same value, different mechanism. Certain as observed;
  drift on any change.
- **L3** — `FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL=0` records the
  Linux `-Werror`, not `fclose()` behavior: same string-literal
  conversion rejection as L2 at `configure.cmake:834-868`
  (`COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in §2.3). Mac
  records `0` by execution (ran, did not exit 0). Certain as
  observed; drift on any change.
- **L4** — `HAVE_BROKEN_FIFO_SELECT=0` records probe-source
  staleness, not `select()`-on-fifo behavior: same `numFD`/`numFDs`
  defect as Mac Q6 (`configure.cmake:176-228`;
  `COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in §2.3).
  Certain as observed; drift on any probe-source fix.
- **L5** — `MMAP_ANON_IGNORES_PROTECTION=0` records probe-source
  staleness, not `PROT_NONE` behavior: same `void *handle_signal`
  function-pointer defect as Mac Q7 (`configure.cmake:475-514`;
  `COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in §2.3).
  Certain as observed; drift on any probe-source fix.
- **L6** — `HAVE_PROCFS_CTL=0` records probe-source staleness, not
  `/proc` availability: same undeclared `$2` placeholder as Mac Q8
  (`configure.cmake:643-665`; `COMPILED=FALSE`, no `EXITCODE`;
  diagnostic quoted in §2.3). Certain as observed; drift on any
  probe-source fix.
- **L7** — `HAVE_COMPATIBLE_EXP=0` records probe-source staleness,
  not `exp()` behavior: same missing-`<string.h>` `memcmp` defect as
  Mac Q3 (`configure.cmake:722-740`; `COMPILED=FALSE`; diagnostic
  quoted in §2.3). Certain as observed; drift on any probe-source
  fix.
- **L8** — `HAVE_BROKEN_FIFO_KEVENT=0` records a missing Linux
  header, not `kevent()` behavior: the probe includes
  `<sys/event.h>`, which does not exist on glibc
  (`COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in §2.3). Mac
  records `0` with no compile-failure note (executed, did not exit
  0). Certain as observed; drift on any change.
- **L9** — `HAVE_MACH_ABSOLUTE_TIME=0` records a missing Linux
  header, not timer behavior: the probe includes
  `<mach/mach_time.h>`, which does not exist on glibc
  (`COMPILED=FALSE`, no `EXITCODE`; diagnostic quoted in §2.3).
  Certain as observed; drift on any change.
- **L10** — `HAVE_CHAR_BIT=0` records a missing Linux header, not
  the `CHAR_BIT` symbol: the probe includes `<sys/limits.h>`, which
  glibc does not ship (`fatal error: 'sys/limits.h' file not found`;
  diagnostic in the log). Certain as observed; drift on any change.
- **L11** — `HAVE_BSD_REGS_T=0` records a missing Linux header: the
  probe includes `"machine/reg.h"`, which does not exist on glibc
  (`fatal error: 'machine/reg.h' file not found`). Certain as
  observed; drift on any change.
- **L12** — `HAS_FTRUNCATE_LENGTH_ISSUE` appears twice in
  `config.h.in` (`:85` and `:127`) from the single variable set at
  `configure.cmake:914`; both emitted lines are `0`. The Linux build
  reproduces both lines.
- **L13** — `configure.cmake:94` sets the singular
  `CMAKE_EXTRA_INCLUDE_FILE` (vs. `CMAKE_EXTRA_INCLUDE_FILES` used at
  `:79-93`), so the `prwatch_t` probe at `:95` runs with no extra
  include; observed `HAVE_PRWATCH_T=0` (cache `FALSE`; `use of
  undeclared identifier 'prwatch_t'`). Recorded as-is (same quirk as
  Mac Q4); the check reproduces the check as run, not as possibly
  intended.
- **L14** — `HAVE_SYSCTL=0` records a glibc symbol-version
  retirement, and reproducing it constrains the probe LINK, not just
  its source: `sysctl@GLIBC_2.2.5` survives in `libc.so.6` only as a
  non-default (single-`@`) version, which no new link may bind — the
  reference try_compile fails at link (`undefined reference to
  'sysctl'`, GNU ld; `CMakeConfigureLog.yaml` "Looking for sysctl"
  section, exit 1). `zig cc`'s bundled glibc stubs over-export the
  symbol (no version info), so a naive `zig cc` probe links fine and
  observes 1. Every probe link therefore prepends the real system
  objects (`/lib/x86_64-linux-gnu/libc.so.6`, plus
  `libpthread.so.0`/`libm.so.6` when the probe requests `-lpthread`/
  `-lm`, plus `/lib64/ld-linux-x86-64.so.2` so libc's GLIBC_PRIVATE
  references satisfy `--no-allow-shlib-undefined`): LLD consults
  them first and reports version-unusable symbols as undefined,
  exactly like the reference failure. Discovered on the first online
  run (B4.3, 94/95 with only this deviation; the fix changed no
  contract value). The reference `-l` flags stay on the probe lines
  as fallback, and any residual stub/system divergence trips the
  fail-clearly gate. Certain as observed; drift on any glibc
  symbol-version change.

## 4. Out of scope for this file

Probed by `configure.cmake` but not `config.h.in` settings, hence not
classified above:

- `HAVE_LIBICU_UCHAR_H` (`:36`, resolved by running the probe here —
  cache `INTERNAL=1` — because no `-DICU_INCLUDE_PATH` was passed;
  gate at `:917-922`) — a configure-time ICU gate, never emitted.
  The Linux build needs its own ICU-presence gate with a clear error,
  mirroring the `:920` message.
- `HAVE_DIRECTIO` (`:69`, cache empty) and
  `PLATFORM_ACCEPTS_ABS_OVERLOAD` (`:106-120`, `COMPILED=FALSE` with
  `error: declaration conflicts with target of using declaration
  already in scope`) — probed, consumed nowhere in `config.h.in`.
- `CLANG_HAS_DISABLE_TAIL_CALLS_CFG`
  (`Build/CMakeFeatureDetect.cmake:17-27`; gate `:29`, `-D` emit
  `:30`) — a command-line `-D`, covered as check #95 of the Zig
  contract (`tools/pal-config-linux/contract.zig`), not a PAL
  setting.

Also out of scope: `HAVE_SNPRINTF` and `HAVE_STRCASECMP` in
`CMakeCache.txt` come from WABT's own feature checks
(`lib/wabt/CMakeLists.txt:29-31`), not from PAL configuration. The
remaining non-PAL entries (`HAVE_STDDEF_H`, `HAVE_UNISTD_H`,
`HAVE_SYS_TYPES_H`, `HAVE_STDINT_H`, `HAVE_SSIZE_T`,
`HAVE_SIZEOF_SIZE_T`) were not traced to a source line; they are
absent from `pal/src/configure.cmake`'s probe list, so they are not
PAL settings either.

## 5. Fail-clearly specification for the Linux build

The Zig build owns `pal/src/config.h` generation for this profile. It
MUST NOT copy the reference-generated header
([modernization-plan.md](modernization-plan.md) §B1). Required
behavior:

1. **Target gate (first, before any check).** Accept only the declared
   profile: `x86_64-linux-gnu` on the Ubuntu 24.04 floor (glibc 2.39,
   libicu-dev 74.2), JIT enabled, RelWithDebInfo. Any other
   arch/OS/libc/ICU-mode/JIT-mode combination fails before probing
   with a message naming the requested combination and the single
   supported profile. No silent fallback to another profile.
2. **Missing inputs fail before probing.** Absent compiler, missing
   ICU headers/libs, or a missing native-execution environment each
   fail with the missing input named (ICU message mirrors
   `configure.cmake:920`).
3. **Emit-then-verify.** The build emits `config.h` from §2
   (literals directly; COMPILE-CHECKED via its own compile/link
   checks; NATIVE-RUNTIME-VERIFIED via native execution) and diffs
   the result against every value recorded in §2. Any deviation —
   including the drift cases in L1-L11 and the S1/S2 sensitivities —
   fails the build naming the setting, the contract value with its
   source line, and the observed value. A deviation is resolved by a
   contract update with fresh evidence, never by silently accepting
   the new value. General rule: reproduce the check as run, not as
   possibly intended — the check replicates the reference probe
   sources verbatim, defects included (L4-L7/L13), under the
   reference probe command lines, `-Werror` included (L1-L3), so any
   upstream probe-source or flag change surfaces as a fail-clearly
   deviation rather than silent drift.
4. **No cross execution.** Runtime probes run only when build machine
   equals target. A cross configuration (none supported in this
   profile) fails at the target gate instead of skipping or faking
   run probes.
5. **No generic engine.** One profile, explicit checks. Additional
   targets get their own contract and gate through an expansion
   milestone; shared probe helpers may be factored only when a second
   target needs them. (The run-probe *sources* are already shared
   with Mac by re-export in `tools/pal-config-linux/probes.zig`,
   since they are the same `configure.cmake` bodies; only values and
   command lines differ.)
6. **Offline comparison where execution is unavailable.** The check
   exposes a no-execution render path (`renderExpected` /
   `checkExpectedAgainst` in `tools/pal-config-linux/check.zig`)
   that resolves the header purely from the contract tables and
   diffs it against reference bytes, failing loudly on the first
   differing line. This verifies transcription and layout without
   running any probe; it never substitutes for online
   emit-then-verify on an execution-capable host (item 3), which
   remains the gate.

## 6. Validation performed and open uncertainties

Validation (read-only against the B4.1 tree; the only new files are
this one and `tools/pal-config-linux/`):

- Read `config.h.in` in full, the whole probe range of
  `configure.cmake` (`:1-120`, `:871-923`, plus the cited run bodies),
  all of `Build/CMakeFeatureDetect.cmake`, the B4 environment record,
  and the full reference `config.h` (MD5
  `f73cd5b2968beaad6b6266a2cb61b06a`).
- Verified no `-D` overrides for any contract variable exist in
  `CMakeCache.txt` (no non-`INTERNAL` entries under the
  `HAVE_*`/`HAS_*`/`SIZEOF_*`/`MMAP_*`/`PLATFORM_*` prefixes).
- Every one of the 128 emitted lines maps to exactly one classified
  setting (127 unique; `HAS_FTRUNCATE_LENGTH_ISSUE` twice): 33
  FIXED-BY-TARGET, 62 COMPILE-CHECKED, 32 NATIVE-RUNTIME-VERIFIED.
  No `config.h.in` line is unaccounted for.
- Corroborated surprising values by a second route
  (`CMakeCache.txt` `INTERNAL` entries and `CMakeConfigureLog.yaml`
  diagnostics/exit codes): the three `-Werror` flips L1-L3
  (`COMPILED=FALSE`, quoted diagnostics), the four shared-staleness
  cases L4-L7 (`COMPILED=FALSE`, quoted diagnostics), the three
  missing-header cases L8/L10-L11 (`COMPILED=FALSE`, quoted
  fatals), `HAVE_MACH_ABSOLUTE_TIME` (missing header), the
  `MAP_FIXED`/shared-mapping/`errno` exit-1 outcomes
  (`COMPILED=TRUE EXITCODE=1`), the genuine glibc-1 outcomes
  (`COMPILED=TRUE EXITCODE=0`: four `clock_gettime` probes,
  `sched_getcpu`, `mmap_dev_zero`, `ungetc`, `posix_semaphores`,
  `getpwuid_r_sets_errno`), `HAVE_SIGRETURN=1` (links),
  `HAVE_SYSCTL` empty (glibc removed it), the `HAVE_GREGSET_T`/
  `HAVE_UCONTEXT_T`/`HAVE_PT_REGS` `TRUE` entries, the probed
  `HAVE_LIBICU_UCHAR_H=1`, and the tail-calls `1/TRUE/0` triple.
- Recorded live toolchain facts (`clang++ --version`, `ldd
  --version`, cache `CMAKE_BUILD_TYPE`/`DISABLE_JIT`) in §0.
- Offline verification (no execution of built binaries): an
  independent text parse of `tools/pal-config-linux/contract.zig`
  renders the header and compares it byte-for-byte with the
  reference — **byte-identical, 4839 bytes** — with a coverage audit
  (every setting emitted exactly once, no fixed/checked overlap).
  Method: `python3 verify_contract.py` (B4.2 scratch, kept at
  `/Users/ascotti/b4-pilot-tmp/verify_contract.py`; rerun on the
  lima VM, which sees both the repo and the reference tree). This
  byte-exact comparison is an offline/VM procedure, not in-repo
  enforcement: the in-repo wired test (the `offline render` test in
  `tools/pal-config-linux/check.zig`, run by the Linux
  `check-selftest` step via `tools/linux-gate-tests.zig`) asserts
  that the rendered header carries the contract values, and
  byte-exact equality with the reference header remains the
  offline/VM procedure above until a native host exists.
- Compile verification: B4.2 verified `contract.zig` and
  `check.zig` via standalone `zig build-obj` with a `mac_probes`
  module import (rc=0 both files, 0-byte logs; review-validated argv
  kept below for the record). B4.3 replaced the module import with a
  plain relative import (`../pal-config/probes.zig`), which the
  `@build` graph resolves (build root = module root) but standalone
  `build-obj` cannot express (0.17 roots the main module at the
  file's own dir and fails `..` with `error: import of file outside
  module path` — observed). Compile verification therefore goes
  through `zig build` configure, which compiles this exact graph:
  the B4.3 Linux build's configure compiles it clean (see
  [b4-linux.md](b4-linux.md) §8), which also evaluates the
  `62/32/33` comptime asserts.
- B4.2 standalone recipe (superseded — no longer runnable against
  the relative import; kept for the record):

  ```sh
  cd <repo-root>
  zig build-obj -Mmain=tools/pal-config-linux/contract.zig --dep mac_probes -Mmac_probes=tools/pal-config/probes.zig
  zig build-obj -Mmain=tools/pal-config-linux/check.zig --dep mac_probes -Mmac_probes=tools/pal-config/probes.zig
  ```

Open uncertainties:

- **S1 (run probes executed under emulation):** every §2.3 exit code
  was produced by the B4.1 configure inside the qemu-user-emulated
  container. glibc-behavior outcomes (clocks, `getpwuid_r`,
  `ungetc`, semaphores) are emulation-faithful by construction, but
  the `mmap` outcomes (`MMAP_IGNORES_HINT`,
  `MMAP_DOESNOT_ALLOW_REMAP`,
  `ONE_SHARED_MAPPING_PER_FILEREGION_PER_PROCESS`,
  `HAVE_MMAP_DEV_ZERO`) exercise emulated memory mapping. The first
  native x64 execution MUST re-run the online check (§5 item 3) and
  treat any deviation as a contract update with fresh evidence, not
  as a failure to suppress. Update (B4.3): all 32 run probes re-ran
  under qemu-user 10.2.3 during the first online execution and
  reproduced every recorded exit code including the four mmap
  outcomes — same-emulation-family corroboration, still not native
  x64; the MUST above stands.
- **S2 (online check unexecuted):** RESOLVED by the first online
  execution (B4.3, 2026-10-12, qemu-user 10.2.3): `generate()` ran
  all 95 checks in-container under qemu-user emulation (not
  natively; S1) — 95/95 match after the L14
  probe-link fix (the first run was 94/95 with only HAVE_SYSCTL
  deviating; the fix changed no contract value). See
  [b4-linux.md](b4-linux.md) §8.
- No per-setting value is uncertain: every value is either a source
  line literal/branch or an observed generated value, with the
  surprising mechanisms pinned to log evidence.
