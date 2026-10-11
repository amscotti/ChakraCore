# PAL configuration contract (Mac reference profile)

B1 step 2 — the per-setting PAL configuration contract for exactly ONE
profile: native ARM64 macOS, shared/no-JIT, RelWithDebInfo Test, system
ICU. Source of truth: `pal/src/config.h.in` (149 lines),
`pal/src/configure.cmake`, and `Build/CMakeFeatureDetect.cmake` at
baseline `97e68c917` on branch `modernize/baseline`, cross-checked
against the reference build's generated `config.h`.

Reference generated header used: `build-zigpilot/pal/src/config.h`
(MD5 `855152b0a1b6da1d8abfef52e1f45f4e`). `build-xplat/pal/src/config.h`
is byte-identical (`cmp` clean, same MD5). `build-zigpilot/` is cited
because it also holds the compile DB used by
[build-contract-mac.md](build-contract-mac.md); both paths are validation
evidence, not contract inputs.

Conventions: every value cites a source line or an observed generated
value. Items that could not be pinned down are marked **UNCERTAIN** with
the reason. Nothing here is invented: machine-local values (SDK path,
Homebrew prefix) are recorded as observed.

## 0. Profile and assumptions

Reference profile (same as [build-contract-mac.md](build-contract-mac.md)
and [build.md](build.md) §13):

- Target: native ARM64 macOS (`CMAKE_SYSTEM_NAME=Darwin`,
  `CMAKE_SYSTEM_PROCESSOR=arm64`). No cross-compilation, no Android
  branch (`pal/src/configure.cmake:3` takes the probe path).
- Configure: direct CMake, `-GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DDISABLE_JIT=ON -DICU_INCLUDE_PATH=/opt/homebrew/opt/icu4c/include
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`.
  No `-D` overrides for any `HAVE_*`/`SIZEOF_*`/`PAL_*` variable were
  passed (no such entries in `CMakeCache.txt` beyond probe `INTERNAL`
  results).
- Toolchain (observed): AppleClang 21.0.0 (`/usr/bin/clang{,++}`,
  `clang-2100.1.1.101`, target `arm64-apple-darwin25.6.0`), CMake 4.4.0,
  Ninja 1.13.2, host macOS 26.6 (build 25G72).
- SDK (observed now via `xcrun`): Xcode
  `MacOSX.sdk`, version **26.5**, at
  `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`.
  `CMAKE_OSX_SYSROOT` and `CMAKE_OSX_DEPLOYMENT_TARGET` are both empty
  in `CMakeCache.txt`, so the reference configure used compiler-default
  SDK resolution. **UNCERTAIN (S1):** the SDK version *at configure
  time* (Oct 10) was not recorded in any build artifact; 26.5 is the
  version observed when this contract was written. The configure log
  confirms probes compiled against the Xcode `MacOSX.sdk` path above.
- Deployment target: probe compile lines carried the inherited
  `-mmacosx-version-min=10.9` (`CMakeLists.txt:347-351`,
  `MACOSX_DEPLOYMENT_TARGET` unset; visible in
  `CMakeConfigureLog.yaml` probe invocations). **UNCERTAIN (S2):**
  whether any probe result below is sensitive to the
  `-mmacosx-version-min` value is unknown — the values are recorded
  as observed under 10.9, while the B1.3 target contract is macOS
  26.0 ([build.md](build.md) §14). B1.3 MUST re-run every
  COMPILE-CHECKED and NATIVE-RUNTIME-VERIFIED check under the 26.0
  target and fail clearly on any deviation (see §5).
- ICU: Homebrew ICU 78.3 (`U_ICU_VERSION "78.3"` in
  `/opt/homebrew/opt/icu4c/include/unicode/uvernum.h`).
  `HAVE_LIBICU_UCHAR_H` is pre-set as a plain CMake variable at
  `CMakeLists.txt:598-602`, which short-circuits the
  `check_include_files(unicode/uchar.h …)` probe at
  `pal/src/configure.cmake:36` (confirmed: no `HAVE_LIBICU_UCHAR_H`
  entry exists in `CMakeCache.txt`, and the `:917-922` gate passed).
  It is not a `config.h` setting (see §4).
- `Build/CMakeFeatureDetect.cmake` contributes no `config.h` setting:
  it only probes `CLANG_HAS_DISABLE_TAIL_CALLS_CFG` (`:17-27`) and,
  gated by `:29`, emits `-DCLANG_HAS_DISABLE_TAIL_CALLS=1` (`:30`)
  with empty `CXX_DO_NOT_OPTIMIZE_SIBLING_CALLS` (`:31`; `:32-33`
  otherwise).
- Global probe input: `pal/src/configure.cmake:23` appends
  `-D_FILE_OFFSET_BITS=64` to `CMAKE_REQUIRED_DEFINITIONS`, so every
  probe below compiled with it (visible on all probe compile lines
  in `CMakeConfigureLog.yaml`).

## 1. Classification and B1.3 consequence

| Classification | Meaning | B1.3 consequence |
|---|---|---|
| FIXED-BY-TARGET | Value follows from the declared target (Darwin branch, unconditional non-Android set, or template literal); no probing involved | emit literal |
| COMPILE-CHECKED | Value comes from a compile/link-only probe (`check_include_files`, `check_function_exists`, `check_library_exists`, `check_struct_has_member`, `check_type_size`, `check_cxx_symbol_exists`) | emit via Zig check |
| NATIVE-RUNTIME-VERIFIED | Value comes from `check_cxx_source_runs`: the probe must be executed natively | verify at build time |

Semantics for B1.3 (see §5 for the fail-clearly spec):

- **emit literal**: the Zig build writes the recorded value with no
  check. The guard is the target gate: these literals are valid only
  for the declared `aarch64-macos` profile.
- **emit via Zig check**: the Zig build performs an equivalent
  compile/link check at build time and emits its result. Any deviation
  from the recorded value fails the build with the diff (fail-clearly,
  not silent drift).
- **verify at build time**: the Zig build compiles, runs natively, and
  requires the probe outcome to equal the recorded value; any
  difference fails the build with the probe identity, expected value,
  and observed value. This profile is native-only, so build machine
  equals target; cross builds are rejected (see §5).

Counts: FIXED-BY-TARGET 33, COMPILE-CHECKED 62,
NATIVE-RUNTIME-VERIFIED 32 — 127 unique settings covering all 128
emitted `#define`/`#undef` lines (`HAS_FTRUNCATE_LENGTH_ISSUE` is
emitted twice from one variable, §3 quirk Q1).

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

Darwin branch (`configure.cmake:874-886`; justification: the reference
profile satisfies `CMAKE_SYSTEM_NAME STREQUAL Darwin`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_COREFOUNDATION` | `1` | `configure.cmake:875` Darwin branch |
| `HAVE__NSGETENVIRON` | `1` | `configure.cmake:876` Darwin branch |
| `DEADLOCK_WHEN_THREAD_IS_SUSPENDED_WHILE_BLOCKED_ON_MUTEX` | `1` | `configure.cmake:877` Darwin branch |
| `PAL_PTRACE` | `ptrace((cmd), (pid), (caddr_t)(addr), (data))` | `configure.cmake:878` Darwin branch |
| `PAL_PT_ATTACH` | `PT_ATTACH` | `configure.cmake:879` Darwin branch |
| `PAL_PT_DETACH` | `PT_DETACH` | `configure.cmake:880` Darwin branch |
| `PAL_PT_READ_D` | `PT_READ_D` | `configure.cmake:881` Darwin branch |
| `PAL_PT_WRITE_D` | `PT_WRITE_D` | `configure.cmake:882` Darwin branch |
| `JA_JP_LOCALE_NAME` | `"ja_JP.SJIS"` | `configure.cmake:883` Darwin branch |
| `KO_KR_LOCALE_NAME` | `"ko_KR.eucKR"` | `configure.cmake:884` Darwin branch |
| `ZH_TW_LOCALE_NAME` | `"zh_TG.BIG5"` | `configure.cmake:885` Darwin branch, spelling as-is (see §3 Q2) |
| `HAS_FTRUNCATE_LENGTH_ISSUE` | `1` | `configure.cmake:886` Darwin branch; emitted twice (`config.h.in:85,127`, see §3 Q1) |

Never set on the Darwin path (justification: no `set` or probe writes
the variable on any non-Android path, so plain `#cmakedefine`
emits `/* #undef X */` and `#cmakedefine01` emits `0` (per the
template above); the reference configure passed no overriding
`-D`):

| Setting | Value | Source |
|---|---|---|
| `FREEBSD_LIBC` | `/* #undef FREEBSD_LIBC */` | set only on FreeBSD (`configure.cmake:899-903`); observed verbatim at generated line 128 |
| `STATVFS64_PROTOTYPE_BROKEN` | `0` | no setter anywhere (only an Android-branch comment, `configure.cmake:993`); observed `0` at generated line 94 |

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

Note: whether the `*_LOCALE_NAME` values name locales actually
available on the reference host was NOT verified — no probe checks
them, and host locale availability is out of scope for this contract.
B1.3 emits them literally.

### 2.2 COMPILE-CHECKED (62) — emit via Zig check

Header existence (`check_include_files`, `configure.cmake:25-35`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_IEEEFP_H` | `0` | `:25` `check_include_files(ieeefp.h …)`; observed `0` |
| `HAVE_ALLOCA_H` | `1` | `:26` `check_include_files(alloca.h …)`; observed `1` |
| `HAVE_SYS_VMPARAM_H` | `1` | `:27` `check_include_files(sys/vmparam.h …)`; observed `1` |
| `HAVE_MACH_VM_TYPES_H` | `1` | `:28` `check_include_files(mach/vm_types.h …)`; observed `1` |
| `HAVE_MACH_VM_PARAM_H` | `1` | `:29` `check_include_files(mach/vm_param.h …)`; observed `1` |
| `HAVE_PROCFS_H` | `0` | `:30` `check_include_files(procfs.h …)`; observed `0` |
| `HAVE_CRT_EXTERNS_H` | `1` | `:31` `check_include_files(crt_externs.h …)`; observed `1` |
| `HAVE_SYS_TIME_H` | `1` | `:32` `check_include_files(sys/time.h …)`; observed `1` |
| `HAVE_PTHREAD_NP_H` | `0` | `:33` `check_include_files(pthread_np.h …)`; observed `0` |
| `HAVE_SYS_LWP_H` | `0` | `:34` `check_include_files(sys/lwp.h …)`; observed `0` |
| `HAVE_RUNETYPE_H` | `1` | `:35` `check_include_files(runetype.h …)`; observed `1` |

Function/link existence (`check_function_exists`,
`configure.cmake:38-39,49-72`; `check_library_exists(pthread …)`,
`:40-48` — compile-plus-link checks, no execution):

| Setting | Value | Source |
|---|---|---|
| `HAVE_KQUEUE` | `1` | `:38` `check_function_exists(kqueue …)`; observed `1` |
| `HAVE_GETPWUID_R` | `1` | `:39` `check_function_exists(getpwuid_r …)`; observed `1` |
| `HAVE_PTHREAD_SUSPEND` | `0` | `:40` `check_library_exists(pthread pthread_suspend …)`; cache `INTERNAL=` (empty), observed `0` |
| `HAVE_PTHREAD_SUSPEND_NP` | `0` | `:41` `check_library_exists(pthread pthread_suspend_np …)`; cache empty, observed `0` |
| `HAVE_PTHREAD_CONTINUE` | `0` | `:42` `check_library_exists(pthread pthread_continue …)`; observed `0` |
| `HAVE_PTHREAD_CONTINUE_NP` | `0` | `:43` `check_library_exists(pthread pthread_continue_np …)`; observed `0` |
| `HAVE_PTHREAD_RESUME_NP` | `0` | `:44` `check_library_exists(pthread pthread_resume_np …)`; observed `0` |
| `HAVE_PTHREAD_ATTR_GET_NP` | `0` | `:45` `check_library_exists(pthread pthread_attr_get_np …)`; observed `0` |
| `HAVE_PTHREAD_GETATTR_NP` | `0` | `:46` `check_library_exists(pthread pthread_getattr_np …)`; observed `0` |
| `HAVE_PTHREAD_GETCPUCLOCKID` | `0` | `:47` `check_library_exists(pthread pthread_getcpuclockid …)`; observed `0` |
| `HAVE_PTHREAD_SIGQUEUE` | `0` | `:48` `check_library_exists(pthread pthread_sigqueue …)`; observed `0` |
| `HAVE_SIGRETURN` | `0` | `:49` `check_function_exists(sigreturn …)`; observed `0` |
| `HAVE__THREAD_SYS_SIGRETURN` | `0` | `:50` `check_function_exists(_thread_sys_sigreturn …)`; observed `0` |
| `HAVE_COPYSIGN` | `1` | `:51` `check_function_exists(copysign …)`; observed `1` |
| `HAVE_FSYNC` | `1` | `:52` `check_function_exists(fsync …)`; observed `1` |
| `HAVE_FUTIMES` | `1` | `:53` `check_function_exists(futimes …)`; observed `1` |
| `HAVE_UTIMES` | `1` | `:54` `check_function_exists(utimes …)`; observed `1` |
| `HAVE_SYSCTL` | `1` | `:55` `check_function_exists(sysctl …)`; observed `1` |
| `HAVE_SYSCONF` | `1` | `:56` `check_function_exists(sysconf …)`; observed `1` |
| `HAVE_LOCALTIME_R` | `1` | `:57` `check_function_exists(localtime_r …)`; observed `1` |
| `HAVE_GMTIME_R` | `1` | `:58` `check_function_exists(gmtime_r …)`; observed `1` |
| `HAVE_TIMEGM` | `1` | `:59` `check_function_exists(timegm …)`; observed `1` |
| `HAVE__SNWPRINTF` | `0` | `:60` `check_function_exists(_snwprintf …)`; observed `0` |
| `HAVE_POLL` | `1` | `:61` `check_function_exists(poll …)`; observed `1` |
| `HAVE_STATVFS` | `1` | `:62` `check_function_exists(statvfs …)`; observed `1` |
| `HAVE_THREAD_SELF` | `0` | `:63` `check_function_exists(thread_self …)`; observed `0` |
| `HAVE__LWP_SELF` | `0` | `:64` `check_function_exists(_lwp_self …)`; observed `0` |
| `HAVE_MACH_THREADS` | `1` | `:65` `check_function_exists(pthread_mach_thread_np …)`; observed `1` |
| `HAVE_MACH_EXCEPTIONS` | `1` | `:66` `check_function_exists(thread_set_exception_ports …)`; observed `1` |
| `HAVE_VM_ALLOCATE` | `1` | `:67` `check_function_exists(vm_allocate …)`; observed `1` |
| `HAVE_VM_READ` | `1` | `:68` `check_function_exists(vm_read …)`; observed `1` |
| `HAS_SYSV_SEMAPHORES` | `1` | `:70` `check_function_exists(semget …)`; observed `1` |
| `HAS_PTHREAD_MUTEXES` | `1` | `:71` `check_function_exists(pthread_mutex_init …)`; observed `1` |
| `HAVE_TTRACE` | `0` | `:72` `check_function_exists(ttrace …)`; observed `0` |

Struct members (`check_struct_has_member`, `configure.cmake:74-77`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_STAT_TIMESPEC` | `1` | `:74` member `st_atimespec` of `struct stat`; observed `1` |
| `HAVE_STAT_NSEC` | `0` | `:75` member `st_atimensec` of `struct stat`; observed `0` |
| `HAVE_TM_GMTOFF` | `1` | `:76` member `tm_gmtoff` of `struct tm`; observed `1` |
| `HAVE_GREGSET_T` | `0` | `:77` member `uc_mcontext.gregs[0]` of `ucontext_t`; cache empty, observed `0`. Mechanism: the probe fails to compile — SDK `ucontext.h:51` errors without `_XOPEN_SOURCE` (see §3 Q5) |

Type sizes (`check_type_size`, `configure.cmake:79-97`). Note the
naming indirection: `check_type_size(<type> <VAR>)` writes the size to
`<VAR>` and the existence flag to the implicit `HAVE_<VAR>` variable,
which is what `config.h.in` consumes (corroborated by
`HAVE_BSD_REGS_T:INTERNAL=FALSE` etc. in `CMakeCache.txt`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_BSD_REGS_T` | `0` | `:80` `check_type_size("struct reg" BSD_REGS_T)` (+`machine/reg.h`); cache `FALSE`, observed `0` |
| `HAVE_PT_REGS` | `0` | `:83` `check_type_size("struct pt_regs" PT_REGS)` (+`asm/ptrace.h`); cache `FALSE`, observed `0` |
| `HAVE_SIGINFO_T` | `1` | `:86` `check_type_size(siginfo_t SIGINFO_T)` (+`signal.h`); cache `TRUE`, observed `1` |
| `HAVE_UCONTEXT_T` | `0` | `:89` `check_type_size(ucontext_t UCONTEXT_T)` (+`ucontext.h`); cache `FALSE`, observed `0`. Mechanism: SDK `ucontext.h:51` `#error` without `_XOPEN_SOURCE` (see §3 Q5) |
| `HAVE_PTHREAD_RWLOCK_T` | `1` | `:92` `check_type_size(pthread_rwlock_t PTHREAD_RWLOCK_T)` (+`pthread.h`); observed `1` |
| `HAVE_PRWATCH_T` | `0` | `:95` `check_type_size(prwatch_t PRWATCH_T)`; cache `FALSE`, observed `0` (see §3 Q4: `:94` sets the singular `CMAKE_EXTRA_INCLUDE_FILE`, so no extra include is in effect) |
| `SIZEOF_OFF_T` | `8` | `:97` `check_type_size(off_t SIZEOF_OFF_T)`; the size variable itself, observed `8` |

Symbol existence (`check_cxx_symbol_exists`,
`configure.cmake:99-104`):

| Setting | Value | Source |
|---|---|---|
| `HAVE_YIELD_SYSCALL` | `0` | `:99` symbol `SYS_yield` in `sys/syscall.h`; observed `0` |
| `HAVE_INFTIM` | `0` | `:100` symbol `INFTIM` in `poll.h`; observed `0` |
| `HAVE_CHAR_BIT` | `0` | `:101` symbol `CHAR_BIT` in `sys/limits.h`; observed `0` |
| `USER_H_DEFINES_DEBUG` | `0` | `:102` symbol `_DEBUG` in `sys/user.h`; observed `0` |
| `HAVE__SC_PHYS_PAGES` | `1` | `:103` symbol `_SC_PHYS_PAGES` in `unistd.h`; observed `1` |
| `HAVE__SC_AVPHYS_PAGES` | `0` | `:104` symbol `_SC_AVPHYS_PAGES` in `unistd.h`; observed `0` |

### 2.3 NATIVE-RUNTIME-VERIFIED (32) — verify at build time

Behavioral probes (`check_cxx_source_runs`). The variable is defined
iff the probe compiles, links, and exits 0; otherwise
`#cmakedefine01` emits `0`. "How" below names the run-time behavior
each probe exercises (probe bodies at the cited lines) — except
Q3/Q6/Q7/Q8, which never executed: those cells quote the
compile-failure diagnostic from `CMakeConfigureLog.yaml` instead.

| Setting | Value | Source and how |
|---|---|---|
| `REALPATH_SUPPORTS_NONEXISTENT_FILES` | `0` | `:121-139` run: `realpath("a_nonexistent_file", …)` must succeed; observed `0` |
| `SSCANF_SUPPORT_ll` | `1` | `:140-148` run: `sscanf("5000000000", "%qu", …)` round-trips; observed `1` |
| `SSCANF_CANNOT_HANDLE_MISSING_EXPONENT` | `0` | `:149-163` run: `sscanf("12.34e", "%e", …)` must return `<= 0`; observed `0` |
| `HAVE_LARGE_SNPRINTF_SUPPORT` | `1` | `:164-175` run: huge-size `snprintf` writes; observed `1` |
| `HAVE_BROKEN_FIFO_SELECT` | `0` | `:176-228` run: never executes — the probe fails to COMPILE (`:227` exits on `numFD`, declared as `numFDs` at `:187`; `src.cxx:52:10: error: use of undeclared identifier 'numFD'; did you mean 'numFDs'?`; cache `HAVE_BROKEN_FIFO_SELECT_COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 Q6) |
| `HAVE_BROKEN_FIFO_KEVENT` | `0` | `:229-286` run: `kevent` on an empty fifo must report no events; observed `0` |
| `HAVE_SCHED_GET_PRIORITY` | `1` | `:288-310` run (links `pthread`): `sched_get_priority_min/max` succeed; observed `1` |
| `HAVE_SCHED_GETCPU` | `0` | `:311-323` run (links `pthread`): `sched_getcpu()` must compile, link, and return `>= 0`; observed `0` |
| `HAVE_WORKING_GETTIMEOFDAY` | `1` | `:325-337` run: `gettimeofday` returns 0; observed `1` |
| `HAVE_WORKING_CLOCK_GETTIME` | `0` | `:338-353` run: forced `ret = 1` on `__APPLE__` (backward-compat `#else`, `:349-351`) — never runs `clock_gettime`; observed `0` |
| `HAVE_CLOCK_MONOTONIC` | `0` | `:354-369` run: forced `ret = 1` on `__APPLE__` (`:365-367`); observed `0` |
| `HAVE_CLOCK_MONOTONIC_COARSE` | `0` | `:370-385` run: forced `ret = 1` on `__APPLE__` (`:381-383`); observed `0` |
| `HAVE_MACH_ABSOLUTE_TIME` | `1` | `:386-397` run: `mach_timebase_info` + `mach_absolute_time`; cache `COMPILED=TRUE EXITCODE=0`, observed `1` |
| `HAVE_CLOCK_THREAD_CPUTIME` | `0` | `:398-413` run: forced `ret = 1` on `__APPLE__` (`:409-411`); observed `0` |
| `HAVE_MMAP_DEV_ZERO` | `0` | `:414-433` run: `mmap` of `/dev/zero` must succeed; observed `0` |
| `MMAP_IGNORES_HINT` | `0` | `:434-474` run: `MAP_FIXED` mapping at a freed hint must fail or land elsewhere; observed `0` |
| `MMAP_ANON_IGNORES_PROTECTION` | `0` | `:475-514` run: never executes — the probe fails to COMPILE (`:486` declares `void *handle_signal(int)`, assigned to `void (*)(int)` at `:501`; `src.cxx:27:23: error: incompatible function pointer types assigning to 'void (*)(int)' from 'void *(*)(int)'`; cache `MMAP_ANON_IGNORES_PROTECTION_COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 Q7) |
| `MMAP_DOESNOT_ALLOW_REMAP` | `0` | `:515-542` run: `MAP_FIXED` remap of an anon page must succeed; cache `COMPILED=TRUE EXITCODE=1`, observed `0` |
| `ONE_SHARED_MAPPING_PER_FILEREGION_PER_PROCESS` | `0` | `:543-596` run: two shared mappings of one file region must both succeed; observed `0` |
| `PTHREAD_CREATE_MODIFIES_ERRNO` | `0` | `:597-618` run (links `pthread`): `pthread_create` must leave `errno` zero; observed `0` |
| `SEM_INIT_MODIFIES_ERRNO` | `0` | `:620-641` run (links `pthread`): `sem_init` must leave `errno` at 50; observed `0` |
| `HAVE_PROCFS_CTL` | `0` | `:643-665` run: never executes — the probe fails to COMPILE (`:660` passes the undeclared placeholder `$2` to `open`; `src.cxx:18:19: error: use of undeclared identifier '$2'`; cache `HAVE_PROCFS_CTL_COMPILED=FALSE`, no `EXITCODE`); observed `0` (see §3 Q8) |
| `HAVE_COMPATIBLE_ACOS` | `1` | `:666-676` run (links `m`): `acos(10)` is NaN; observed `1` |
| `HAVE_COMPATIBLE_ASIN` | `1` | `:678-688` run (links `m`): `asin(10)` is NaN; observed `1` |
| `HAVE_COMPATIBLE_ATAN2` | `1` | `:690-720` run (links `m`): signed-zero `atan2` quadrant behavior; observed `1` |
| `HAVE_COMPATIBLE_EXP` | `0` | `:722-740` run (links `m`): never executes — the probe fails to COMPILE (`memcmp` used without `<string.h>`; `src.cxx:14:7: error: use of undeclared identifier 'memcmp'`; cache `HAVE_COMPATIBLE_EXP_COMPILED=FALSE`); observed `0` (see §3 Q3) |
| `HAVE_COMPATIBLE_LOG` | `1` | `:742-752` run (links `m`): `log(-10000)` is NaN; observed `1` |
| `HAVE_COMPATIBLE_LOG10` | `1` | `:754-764` run (links `m`): `log10(-10000)` is NaN; observed `1` |
| `UNGETC_NOT_RETURN_EOF` | `0` | `:766-791` run: `ungetc` on an append-mode stream must return `EOF`; observed `0` |
| `HAS_POSIX_SEMAPHORES` | `0` | `:792-804` run (links `pthread`): `sem_init` must succeed; macOS does not support unnamed semaphores; observed `0` |
| `GETPWUID_R_SETS_ERRNO` | `0` | `:806-833` run: `getpwuid_r` with a 1-byte buffer must set `errno` to `ERANGE`; observed `0` |
| `FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL` | `0` | `:834-868` run: `fclose` after a failed read must fail; observed `0` |

## 3. Reference quirks (recorded as-is)

- **Q1** — `HAS_FTRUNCATE_LENGTH_ISSUE` appears twice in `config.h.in`
  (`:85` and `:127`) from the single variable set at
  `configure.cmake:886`; both emitted lines are `1` (generated lines
  85, 127). B1.3 reproduces both lines.
- **Q2** — `ZH_TW_LOCALE_NAME` is `"zh_TG.BIG5"`
  (`configure.cmake:885`, generated line 132), which reads like a
  `TG`/`TW` typo but is recorded and emitted literally.
- **Q3** — `HAVE_COMPATIBLE_EXP=0` records probe-source staleness, not
  `exp()` behavior: the probe at `configure.cmake:722-740` calls
  `memcmp` with only `<math.h>`/`<stdlib.h>` included, and AppleClang
  21 rejects it (`COMPILED=FALSE` in cache; diagnostic quoted in
  §2.3). The value is certain as observed; a future probe-source fix
  would flip it, which is exactly the drift §5 treats as a
  fail-clearly event.
- **Q4** — `configure.cmake:94` sets the singular
  `CMAKE_EXTRA_INCLUDE_FILE` (vs. `CMAKE_EXTRA_INCLUDE_FILES` used at
  `:79-93`), so the `prwatch_t` probe at `:95` runs with no extra
  include; observed `HAVE_PRWATCH_T=0` (cache `FALSE`). Recorded
  as-is; the B1.3 check reproduces the check as run, not as possibly
  intended.
- **Q5** — `HAVE_UCONTEXT_T=0` and `HAVE_GREGSET_T=0` share one
  mechanism: SDK `ucontext.h:51` (`MacOSX.sdk/usr/include/ucontext.h`)
  carries `#error The deprecated ucontext routines require
  _XOPEN_SOURCE to be defined`, failing both probes at compile time
  (diagnostics quoted from `CMakeConfigureLog.yaml`). Certain as
  observed; re-verification under the 26.0 target falls under S2.
- **Q6** — `HAVE_BROKEN_FIFO_SELECT=0` records probe-source
  staleness, not `select()`-on-fifo behavior: the probe at
  `configure.cmake:176-228` declares `numFDs` (`:187`) but exits on
  `numFD` (`:227`), and AppleClang 21 rejects it
  (`COMPILED=FALSE`, no `EXITCODE` in cache; diagnostic quoted in
  §2.3). The value is certain as observed; a future probe-source fix
  would flip it, which is exactly the drift §5 treats as a
  fail-clearly event.
- **Q7** — `MMAP_ANON_IGNORES_PROTECTION=0` records probe-source
  staleness, not `PROT_NONE` behavior: the probe at
  `configure.cmake:475-514` declares `void *handle_signal(int)`
  (`:486`) and assigns it to `action.sa_handler` (`:501`), a
  `void (*)(int)`, which AppleClang 21 rejects as an incompatible
  function-pointer assignment (`COMPILED=FALSE`, no `EXITCODE`;
  diagnostic quoted in §2.3). The value is certain as observed; a
  future probe-source fix would flip it, which is exactly the drift
  §5 treats as a fail-clearly event.
- **Q8** — `HAVE_PROCFS_CTL=0` records probe-source staleness, not
  `/proc` availability: the probe at `configure.cmake:643-665`
  passes the undeclared placeholder `$2` to `open` (`:660`), which
  AppleClang 21 rejects (`COMPILED=FALSE`, no `EXITCODE`;
  diagnostic quoted in §2.3). The value is certain as observed; a
  future probe-source fix would flip it, which is exactly the drift
  §5 treats as a fail-clearly event.

## 4. Out of scope for this file

Probed by `configure.cmake` but not `config.h.in` settings, hence not
classified above:

- `HAVE_LIBICU_UCHAR_H` (`:36`, short-circuited by
  `CMakeLists.txt:598-602`; gate at `:917-922`) — a configure-time
  ICU gate, never emitted. B1.3 needs its own ICU-presence gate with a
  clear error, mirroring the `:920` message.
- `HAVE_DIRECTIO` (`:69`) and `PLATFORM_ACCEPTS_ABS_OVERLOAD`
  (`:106-120`) — probed, consumed nowhere in `config.h.in`.
- `CLANG_HAS_DISABLE_TAIL_CALLS_CFG`
  (`Build/CMakeFeatureDetect.cmake:17-27`; gate `:29`, `-D` emit
  `:30`) — a command-line `-D`, already covered by
  [build-contract-mac.md](build-contract-mac.md) §1, not a PAL setting.

## 5. Fail-clearly specification for B1.3

The Zig build owns `pal/src/config.h` generation for this profile. It
MUST NOT copy the AppleClang-generated header
([modernization-plan.md](modernization-plan.md) §B1). Required
behavior:

1. **Target gate (first, before any check).** Accept only the declared
   profile: `aarch64-macos` with the pinned macOS minimum (26.0),
   MacOSX SDK 26.5 (the version observed when this contract was
   written; S1 caveat in §0/§6 applies), and system ICU. Any other
   arch/OS/SDK/minimum/ICU-mode/JIT-mode combination fails before
   probing with a message naming the requested combination and the
   single supported profile. No silent fallback to another profile.
2. **Missing inputs fail before probing.** Absent SDK, missing ICU
   headers/libs, or a missing native-execution environment each fail
   with the missing input named (ICU message mirrors
   `configure.cmake:920`).
3. **Emit-then-verify.** The build emits `config.h` from §2
   (literals directly; COMPILE-CHECKED via its own compile/link
   checks; NATIVE-RUNTIME-VERIFIED via native execution) and diffs
   the result against every value recorded in §2. Any deviation —
   including the drift cases in Q3/Q5/Q6/Q7/Q8 and the S1/S2
   sensitivities — fails the build naming the setting, the contract
   value with its source line, and the observed value. A deviation
   is resolved by a contract update with fresh evidence, never by
   silently accepting the new value. General rule: reproduce the
   check as run, not as possibly intended — B1.3 replicates the
   reference probe sources verbatim, defects included (Q3/Q4/Q6/Q7/Q8),
   so any upstream probe-source fix surfaces as a fail-clearly
   deviation rather than silent drift.
4. **No cross execution.** Runtime probes run only when build machine
   equals target. A cross configuration (none supported in this
   profile) fails at the target gate instead of skipping or faking
   run probes.
5. **No generic engine.** One profile, explicit checks. Additional
   targets get their own contract and gate through an expansion
   milestone; shared probe helpers may be factored only when a second
   target needs them.

## 6. Validation performed and open uncertainties

Validation (read-only; nothing was built or modified — the only new
file is this one):

- Read `config.h.in` in full (149 lines, §5.5 cross-check), all of
  `configure.cmake` (`:1-1045`), all of `Build/CMakeFeatureDetect.cmake`,
  the §B1/§5.5/PAL-config sections of the three input docs, and the
  full reference `config.h`.
- `cmp` + MD5: `build-xplat/pal/src/config.h` and
  `build-zigpilot/pal/src/config.h` are byte-identical; the latter is
  cited throughout.
- Every one of the 128 emitted lines maps to exactly one classified
  setting (127 unique; `HAS_FTRUNCATE_LENGTH_ISSUE` twice): 33
  FIXED-BY-TARGET, 62 COMPILE-CHECKED, 32 NATIVE-RUNTIME-VERIFIED.
  No `config.h.in` line is unaccounted for.
- Corroborated surprising values by a second route
  (`CMakeCache.txt` `INTERNAL` entries and `CMakeConfigureLog.yaml`
  diagnostics): `HAVE_UCONTEXT_T`/`HAVE_GREGSET_T` (`ucontext.h:51`
  `#error`), `HAVE_COMPATIBLE_EXP` (`memcmp` undeclared,
  `COMPILED=FALSE`), `HAVE_BROKEN_FIFO_SELECT` (`numFD` undeclared,
  `COMPILED=FALSE`, no `EXITCODE`), `HAVE_PROCFS_CTL` (`$2`
  undeclared, `COMPILED=FALSE`, no `EXITCODE`),
  `MMAP_ANON_IGNORES_PROTECTION` (function-pointer mismatch,
  `COMPILED=FALSE`, no `EXITCODE`), `MMAP_DOESNOT_ALLOW_REMAP`
  (`COMPILED=TRUE EXITCODE=1`), `HAVE_MACH_ABSOLUTE_TIME`
  (`COMPILED=TRUE EXITCODE=0`), `HAVE_SIGINFO_T=TRUE`,
  `HAVE_BSD_REGS_T`/`HAVE_PT_REGS`/`HAVE_PRWATCH_T=FALSE`, and the
  missing `HAVE_LIBICU_UCHAR_H` cache entry confirming the
  short-circuit.
- Recorded live toolchain facts (`sw_vers`, `clang --version`,
  `xcrun`, `uvernum.h`, `cmake --version`, `ninja --version`) in §0.

Open uncertainties:

- **S1 (SDK version at configure time):** 26.5 observed now via
  `xcrun`; no artifact records the SDK version on Oct 10 when the
  reference `config.h` was generated.
- **S2 (deployment-target sensitivity):** reference probes ran with
  inherited `-mmacosx-version-min=10.9`; sensitivity of each result to
  the B1.3 26.0 target is unknown. B1.3 re-verification (§5 item 3)
  closes this.
- No per-setting value is uncertain: every value is either a source
  line literal/branch or an observed generated value, with the
  surprising mechanisms pinned to log evidence.

