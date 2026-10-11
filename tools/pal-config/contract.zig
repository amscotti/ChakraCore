//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! B1.3 PAL configuration contract, transcribed from doc/pal-config-contract-mac.md section 2.
//!
//! `checks` holds every COMPILE-CHECKED (62) and NATIVE-RUNTIME-VERIFIED (32) setting plus the
//! `disable_tail_calls` compiler-attribute decision (1): 95 real checks. FIXED-BY-TARGET settings
//! live in `fixed01` / the `layout` literals and are emitted without probing (the target gate in
//! build.zig is their guard). `layout` reproduces pal/src/config.h.in line order so the generated
//! pal/src/config.h is comparable with the reference build-zigpilot/pal/src/config.h.

const std = @import("std");

const probes = @import("probes.zig");

pub const Kind = enum {
    include,
    func,
    libfunc,
    @"struct",
    typesize,
    sizeof_eq,
    symbol,
    run,
    run_werror,
};

pub const Check = struct {
    name: []const u8,
    cite: []const u8,
    expected: bool,
    params: Params,

    pub const Params = union(Kind) {
        include: []const u8,
        func: []const u8,
        libfunc: struct { lib: []const u8, func: []const u8 },
        @"struct": struct {
            st: []const u8,
            member: []const u8,
            headers: []const []const u8,
        },
        typesize: struct {
            typ: []const u8,
            extras: []const []const u8,
        },
        sizeof_eq: struct {
            typ: []const u8,
            size: u8,
        },
        symbol: struct {
            sym: []const u8,
            header: []const u8,
        },
        run: struct {
            body: []const u8,
            libs: []const []const u8,
        },
        run_werror: []const u8,
    };
};

/// All 95 checks. Order follows pal/src/configure.cmake (then CMakeFeatureDetect).
pub const checks: []const Check = &.{
    // --- check_include_files (configure.cmake:25-35) ---
    .{ .name = "HAVE_IEEEFP_H", .cite = "pal/src/configure.cmake:25", .expected = false, .params = .{ .include = "ieeefp.h" } },
    .{ .name = "HAVE_ALLOCA_H", .cite = "pal/src/configure.cmake:26", .expected = true, .params = .{ .include = "alloca.h" } },
    .{ .name = "HAVE_SYS_VMPARAM_H", .cite = "pal/src/configure.cmake:27", .expected = true, .params = .{ .include = "sys/vmparam.h" } },
    .{ .name = "HAVE_MACH_VM_TYPES_H", .cite = "pal/src/configure.cmake:28", .expected = true, .params = .{ .include = "mach/vm_types.h" } },
    .{ .name = "HAVE_MACH_VM_PARAM_H", .cite = "pal/src/configure.cmake:29", .expected = true, .params = .{ .include = "mach/vm_param.h" } },
    .{ .name = "HAVE_PROCFS_H", .cite = "pal/src/configure.cmake:30", .expected = false, .params = .{ .include = "procfs.h" } },
    .{ .name = "HAVE_CRT_EXTERNS_H", .cite = "pal/src/configure.cmake:31", .expected = true, .params = .{ .include = "crt_externs.h" } },
    .{ .name = "HAVE_SYS_TIME_H", .cite = "pal/src/configure.cmake:32", .expected = true, .params = .{ .include = "sys/time.h" } },
    .{ .name = "HAVE_PTHREAD_NP_H", .cite = "pal/src/configure.cmake:33", .expected = false, .params = .{ .include = "pthread_np.h" } },
    .{ .name = "HAVE_SYS_LWP_H", .cite = "pal/src/configure.cmake:34", .expected = false, .params = .{ .include = "sys/lwp.h" } },
    .{ .name = "HAVE_RUNETYPE_H", .cite = "pal/src/configure.cmake:35", .expected = true, .params = .{ .include = "runetype.h" } },
    // --- check_function_exists (configure.cmake:38-72) ---
    .{ .name = "HAVE_KQUEUE", .cite = "pal/src/configure.cmake:38", .expected = true, .params = .{ .func = "kqueue" } },
    .{ .name = "HAVE_GETPWUID_R", .cite = "pal/src/configure.cmake:39", .expected = true, .params = .{ .func = "getpwuid_r" } },
    .{ .name = "HAVE_SIGRETURN", .cite = "pal/src/configure.cmake:49", .expected = false, .params = .{ .func = "sigreturn" } },
    .{ .name = "HAVE__THREAD_SYS_SIGRETURN", .cite = "pal/src/configure.cmake:50", .expected = false, .params = .{ .func = "_thread_sys_sigreturn" } },
    .{ .name = "HAVE_COPYSIGN", .cite = "pal/src/configure.cmake:51", .expected = true, .params = .{ .func = "copysign" } },
    .{ .name = "HAVE_FSYNC", .cite = "pal/src/configure.cmake:52", .expected = true, .params = .{ .func = "fsync" } },
    .{ .name = "HAVE_FUTIMES", .cite = "pal/src/configure.cmake:53", .expected = true, .params = .{ .func = "futimes" } },
    .{ .name = "HAVE_UTIMES", .cite = "pal/src/configure.cmake:54", .expected = true, .params = .{ .func = "utimes" } },
    .{ .name = "HAVE_SYSCTL", .cite = "pal/src/configure.cmake:55", .expected = true, .params = .{ .func = "sysctl" } },
    .{ .name = "HAVE_SYSCONF", .cite = "pal/src/configure.cmake:56", .expected = true, .params = .{ .func = "sysconf" } },
    .{ .name = "HAVE_LOCALTIME_R", .cite = "pal/src/configure.cmake:57", .expected = true, .params = .{ .func = "localtime_r" } },
    .{ .name = "HAVE_GMTIME_R", .cite = "pal/src/configure.cmake:58", .expected = true, .params = .{ .func = "gmtime_r" } },
    .{ .name = "HAVE_TIMEGM", .cite = "pal/src/configure.cmake:59", .expected = true, .params = .{ .func = "timegm" } },
    .{ .name = "HAVE__SNWPRINTF", .cite = "pal/src/configure.cmake:60", .expected = false, .params = .{ .func = "_snwprintf" } },
    .{ .name = "HAVE_POLL", .cite = "pal/src/configure.cmake:61", .expected = true, .params = .{ .func = "poll" } },
    .{ .name = "HAVE_STATVFS", .cite = "pal/src/configure.cmake:62", .expected = true, .params = .{ .func = "statvfs" } },
    .{ .name = "HAVE_THREAD_SELF", .cite = "pal/src/configure.cmake:63", .expected = false, .params = .{ .func = "thread_self" } },
    .{ .name = "HAVE__LWP_SELF", .cite = "pal/src/configure.cmake:64", .expected = false, .params = .{ .func = "_lwp_self" } },
    .{ .name = "HAVE_MACH_THREADS", .cite = "pal/src/configure.cmake:65", .expected = true, .params = .{ .func = "pthread_mach_thread_np" } },
    .{ .name = "HAVE_MACH_EXCEPTIONS", .cite = "pal/src/configure.cmake:66", .expected = true, .params = .{ .func = "thread_set_exception_ports" } },
    .{ .name = "HAVE_VM_ALLOCATE", .cite = "pal/src/configure.cmake:67", .expected = true, .params = .{ .func = "vm_allocate" } },
    .{ .name = "HAVE_VM_READ", .cite = "pal/src/configure.cmake:68", .expected = true, .params = .{ .func = "vm_read" } },
    .{ .name = "HAS_SYSV_SEMAPHORES", .cite = "pal/src/configure.cmake:70", .expected = true, .params = .{ .func = "semget" } },
    .{ .name = "HAS_PTHREAD_MUTEXES", .cite = "pal/src/configure.cmake:71", .expected = true, .params = .{ .func = "pthread_mutex_init" } },
    .{ .name = "HAVE_TTRACE", .cite = "pal/src/configure.cmake:72", .expected = false, .params = .{ .func = "ttrace" } },
    // --- check_library_exists(pthread ...) (configure.cmake:40-48) ---
    .{ .name = "HAVE_PTHREAD_SUSPEND", .cite = "pal/src/configure.cmake:40", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_suspend" } } },
    .{ .name = "HAVE_PTHREAD_SUSPEND_NP", .cite = "pal/src/configure.cmake:41", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_suspend_np" } } },
    .{ .name = "HAVE_PTHREAD_CONTINUE", .cite = "pal/src/configure.cmake:42", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_continue" } } },
    .{ .name = "HAVE_PTHREAD_CONTINUE_NP", .cite = "pal/src/configure.cmake:43", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_continue_np" } } },
    .{ .name = "HAVE_PTHREAD_RESUME_NP", .cite = "pal/src/configure.cmake:44", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_resume_np" } } },
    .{ .name = "HAVE_PTHREAD_ATTR_GET_NP", .cite = "pal/src/configure.cmake:45", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_attr_get_np" } } },
    .{ .name = "HAVE_PTHREAD_GETATTR_NP", .cite = "pal/src/configure.cmake:46", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_getattr_np" } } },
    .{ .name = "HAVE_PTHREAD_GETCPUCLOCKID", .cite = "pal/src/configure.cmake:47", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_getcpuclockid" } } },
    .{ .name = "HAVE_PTHREAD_SIGQUEUE", .cite = "pal/src/configure.cmake:48", .expected = false, .params = .{ .libfunc = .{ .lib = "pthread", .func = "pthread_sigqueue" } } },
    // --- check_struct_has_member (configure.cmake:74-77) ---
    .{ .name = "HAVE_STAT_TIMESPEC", .cite = "pal/src/configure.cmake:74", .expected = true, .params = .{ .@"struct" = .{ .st = "struct stat", .member = "st_atimespec", .headers = &.{ "sys/types.h", "sys/stat.h" } } } },
    .{ .name = "HAVE_STAT_NSEC", .cite = "pal/src/configure.cmake:75", .expected = false, .params = .{ .@"struct" = .{ .st = "struct stat", .member = "st_atimensec", .headers = &.{ "sys/types.h", "sys/stat.h" } } } },
    .{ .name = "HAVE_TM_GMTOFF", .cite = "pal/src/configure.cmake:76", .expected = true, .params = .{ .@"struct" = .{ .st = "struct tm", .member = "tm_gmtoff", .headers = &.{"time.h"} } } },
    .{ .name = "HAVE_GREGSET_T", .cite = "pal/src/configure.cmake:77", .expected = false, .params = .{ .@"struct" = .{ .st = "ucontext_t", .member = "uc_mcontext.gregs[0]", .headers = &.{"ucontext.h"} } } },
    // --- check_type_size existence (configure.cmake:80-95) ---
    .{ .name = "HAVE_BSD_REGS_T", .cite = "pal/src/configure.cmake:80", .expected = false, .params = .{ .typesize = .{ .typ = "struct reg", .extras = &.{"machine/reg.h"} } } },
    .{ .name = "HAVE_PT_REGS", .cite = "pal/src/configure.cmake:83", .expected = false, .params = .{ .typesize = .{ .typ = "struct pt_regs", .extras = &.{"asm/ptrace.h"} } } },
    .{ .name = "HAVE_SIGINFO_T", .cite = "pal/src/configure.cmake:86", .expected = true, .params = .{ .typesize = .{ .typ = "siginfo_t", .extras = &.{"signal.h"} } } },
    .{ .name = "HAVE_UCONTEXT_T", .cite = "pal/src/configure.cmake:89", .expected = false, .params = .{ .typesize = .{ .typ = "ucontext_t", .extras = &.{"ucontext.h"} } } },
    .{ .name = "HAVE_PTHREAD_RWLOCK_T", .cite = "pal/src/configure.cmake:92", .expected = true, .params = .{ .typesize = .{ .typ = "pthread_rwlock_t", .extras = &.{"pthread.h"} } } },
    // Q4: configure.cmake:94 sets the singular CMAKE_EXTRA_INCLUDE_FILE, so no
    // extra include is in effect for this probe. Reproduce as run: no extras.
    .{ .name = "HAVE_PRWATCH_T", .cite = "pal/src/configure.cmake:95", .expected = false, .params = .{ .typesize = .{ .typ = "prwatch_t", .extras = &.{} } } },
    .{ .name = "SIZEOF_OFF_T", .cite = "pal/src/configure.cmake:97", .expected = true, .params = .{ .sizeof_eq = .{ .typ = "off_t", .size = 8 } } },
    // --- check_cxx_symbol_exists (configure.cmake:99-104) ---
    .{ .name = "HAVE_YIELD_SYSCALL", .cite = "pal/src/configure.cmake:99", .expected = false, .params = .{ .symbol = .{ .sym = "SYS_yield", .header = "sys/syscall.h" } } },
    .{ .name = "HAVE_INFTIM", .cite = "pal/src/configure.cmake:100", .expected = false, .params = .{ .symbol = .{ .sym = "INFTIM", .header = "poll.h" } } },
    .{ .name = "HAVE_CHAR_BIT", .cite = "pal/src/configure.cmake:101", .expected = false, .params = .{ .symbol = .{ .sym = "CHAR_BIT", .header = "sys/limits.h" } } },
    .{ .name = "USER_H_DEFINES_DEBUG", .cite = "pal/src/configure.cmake:102", .expected = false, .params = .{ .symbol = .{ .sym = "_DEBUG", .header = "sys/user.h" } } },
    .{ .name = "HAVE__SC_PHYS_PAGES", .cite = "pal/src/configure.cmake:103", .expected = true, .params = .{ .symbol = .{ .sym = "_SC_PHYS_PAGES", .header = "unistd.h" } } },
    .{ .name = "HAVE__SC_AVPHYS_PAGES", .cite = "pal/src/configure.cmake:104", .expected = false, .params = .{ .symbol = .{ .sym = "_SC_AVPHYS_PAGES", .header = "unistd.h" } } },
    // --- check_cxx_source_runs (configure.cmake:121-868) ---
    .{ .name = "REALPATH_SUPPORTS_NONEXISTENT_FILES", .cite = "pal/src/configure.cmake:121-139", .expected = false, .params = .{ .run = .{ .body = probes.run_realpath_supports_nonexistent_files, .libs = &.{} } } },
    .{ .name = "SSCANF_SUPPORT_ll", .cite = "pal/src/configure.cmake:140-148", .expected = true, .params = .{ .run = .{ .body = probes.run_sscanf_support_ll, .libs = &.{} } } },
    .{ .name = "SSCANF_CANNOT_HANDLE_MISSING_EXPONENT", .cite = "pal/src/configure.cmake:149-163", .expected = false, .params = .{ .run = .{ .body = probes.run_sscanf_cannot_handle_missing_exponent, .libs = &.{} } } },
    .{ .name = "HAVE_LARGE_SNPRINTF_SUPPORT", .cite = "pal/src/configure.cmake:164-175", .expected = true, .params = .{ .run = .{ .body = probes.run_have_large_snprintf_support, .libs = &.{} } } },
    .{ .name = "HAVE_BROKEN_FIFO_SELECT", .cite = "pal/src/configure.cmake:176-228", .expected = false, .params = .{ .run = .{ .body = probes.run_have_broken_fifo_select, .libs = &.{} } } },
    .{ .name = "HAVE_BROKEN_FIFO_KEVENT", .cite = "pal/src/configure.cmake:229-286", .expected = false, .params = .{ .run = .{ .body = probes.run_have_broken_fifo_kevent, .libs = &.{} } } },
    .{ .name = "HAVE_SCHED_GET_PRIORITY", .cite = "pal/src/configure.cmake:288-310", .expected = true, .params = .{ .run = .{ .body = probes.run_have_sched_get_priority, .libs = &.{"pthread"} } } },
    .{ .name = "HAVE_SCHED_GETCPU", .cite = "pal/src/configure.cmake:311-323", .expected = false, .params = .{ .run = .{ .body = probes.run_have_sched_getcpu, .libs = &.{"pthread"} } } },
    .{ .name = "HAVE_WORKING_GETTIMEOFDAY", .cite = "pal/src/configure.cmake:325-337", .expected = true, .params = .{ .run = .{ .body = probes.run_have_working_gettimeofday, .libs = &.{} } } },
    .{ .name = "HAVE_WORKING_CLOCK_GETTIME", .cite = "pal/src/configure.cmake:338-353", .expected = false, .params = .{ .run = .{ .body = probes.run_have_working_clock_gettime, .libs = &.{} } } },
    .{ .name = "HAVE_CLOCK_MONOTONIC", .cite = "pal/src/configure.cmake:354-369", .expected = false, .params = .{ .run = .{ .body = probes.run_have_clock_monotonic, .libs = &.{} } } },
    .{ .name = "HAVE_CLOCK_MONOTONIC_COARSE", .cite = "pal/src/configure.cmake:370-385", .expected = false, .params = .{ .run = .{ .body = probes.run_have_clock_monotonic_coarse, .libs = &.{} } } },
    .{ .name = "HAVE_MACH_ABSOLUTE_TIME", .cite = "pal/src/configure.cmake:386-397", .expected = true, .params = .{ .run = .{ .body = probes.run_have_mach_absolute_time, .libs = &.{} } } },
    .{ .name = "HAVE_CLOCK_THREAD_CPUTIME", .cite = "pal/src/configure.cmake:398-413", .expected = false, .params = .{ .run = .{ .body = probes.run_have_clock_thread_cputime, .libs = &.{} } } },
    .{ .name = "HAVE_MMAP_DEV_ZERO", .cite = "pal/src/configure.cmake:414-433", .expected = false, .params = .{ .run = .{ .body = probes.run_have_mmap_dev_zero, .libs = &.{} } } },
    .{ .name = "MMAP_IGNORES_HINT", .cite = "pal/src/configure.cmake:434-474", .expected = false, .params = .{ .run = .{ .body = probes.run_mmap_ignores_hint, .libs = &.{} } } },
    .{ .name = "MMAP_ANON_IGNORES_PROTECTION", .cite = "pal/src/configure.cmake:475-514", .expected = false, .params = .{ .run = .{ .body = probes.run_mmap_anon_ignores_protection, .libs = &.{} } } },
    .{ .name = "MMAP_DOESNOT_ALLOW_REMAP", .cite = "pal/src/configure.cmake:515-542", .expected = false, .params = .{ .run = .{ .body = probes.run_mmap_doesnot_allow_remap, .libs = &.{} } } },
    .{ .name = "ONE_SHARED_MAPPING_PER_FILEREGION_PER_PROCESS", .cite = "pal/src/configure.cmake:543-596", .expected = false, .params = .{ .run = .{ .body = probes.run_one_shared_mapping_per_fileregion_per_process, .libs = &.{} } } },
    .{ .name = "PTHREAD_CREATE_MODIFIES_ERRNO", .cite = "pal/src/configure.cmake:597-618", .expected = false, .params = .{ .run = .{ .body = probes.run_pthread_create_modifies_errno, .libs = &.{"pthread"} } } },
    .{ .name = "SEM_INIT_MODIFIES_ERRNO", .cite = "pal/src/configure.cmake:620-641", .expected = false, .params = .{ .run = .{ .body = probes.run_sem_init_modifies_errno, .libs = &.{"pthread"} } } },
    .{ .name = "HAVE_PROCFS_CTL", .cite = "pal/src/configure.cmake:643-665", .expected = false, .params = .{ .run = .{ .body = probes.run_have_procfs_ctl, .libs = &.{} } } },
    .{ .name = "HAVE_COMPATIBLE_ACOS", .cite = "pal/src/configure.cmake:666-676", .expected = true, .params = .{ .run = .{ .body = probes.run_have_compatible_acos, .libs = &.{"m"} } } },
    .{ .name = "HAVE_COMPATIBLE_ASIN", .cite = "pal/src/configure.cmake:678-688", .expected = true, .params = .{ .run = .{ .body = probes.run_have_compatible_asin, .libs = &.{"m"} } } },
    .{ .name = "HAVE_COMPATIBLE_ATAN2", .cite = "pal/src/configure.cmake:690-720", .expected = true, .params = .{ .run = .{ .body = probes.run_have_compatible_atan2, .libs = &.{"m"} } } },
    .{ .name = "HAVE_COMPATIBLE_EXP", .cite = "pal/src/configure.cmake:722-740", .expected = false, .params = .{ .run = .{ .body = probes.run_have_compatible_exp, .libs = &.{"m"} } } },
    .{ .name = "HAVE_COMPATIBLE_LOG", .cite = "pal/src/configure.cmake:742-752", .expected = true, .params = .{ .run = .{ .body = probes.run_have_compatible_log, .libs = &.{"m"} } } },
    .{ .name = "HAVE_COMPATIBLE_LOG10", .cite = "pal/src/configure.cmake:754-764", .expected = true, .params = .{ .run = .{ .body = probes.run_have_compatible_log10, .libs = &.{"m"} } } },
    .{ .name = "UNGETC_NOT_RETURN_EOF", .cite = "pal/src/configure.cmake:766-791", .expected = false, .params = .{ .run = .{ .body = probes.run_ungetc_not_return_eof, .libs = &.{} } } },
    .{ .name = "HAS_POSIX_SEMAPHORES", .cite = "pal/src/configure.cmake:792-804", .expected = false, .params = .{ .run = .{ .body = probes.run_has_posix_semaphores, .libs = &.{"pthread"} } } },
    .{ .name = "GETPWUID_R_SETS_ERRNO", .cite = "pal/src/configure.cmake:806-833", .expected = false, .params = .{ .run = .{ .body = probes.run_getpwuid_r_sets_errno, .libs = &.{} } } },
    .{ .name = "FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL", .cite = "pal/src/configure.cmake:834-868", .expected = false, .params = .{ .run = .{ .body = probes.run_file_ops_check_ferror_of_previous_call, .libs = &.{} } } },
    // --- disable_tail_calls compiler-attribute decision (Build/CMakeFeatureDetect.cmake:17-27) ---
    .{ .name = "CLANG_HAS_DISABLE_TAIL_CALLS", .cite = "Build/CMakeFeatureDetect.cmake:17-27", .expected = true, .params = .{ .run_werror = probes.run_clang_has_disable_tail_calls } },
};

/// FIXED-BY-TARGET 0/1 settings (emitted literally; guarded by the target gate).
pub const fixed01: []const struct { name: []const u8, value: bool } = &.{
    .{ .name = "SYNCHMGR_SUSPENSION_SAFE_CONDITION_SIGNALING", .value = true },
    .{ .name = "ERROR_FUNC_FOR_GLOB_HAS_FIXED_PARAMS", .value = true },
    .{ .name = "HAVE_COREFOUNDATION", .value = true },
    .{ .name = "HAVE__NSGETENVIRON", .value = true },
    .{ .name = "DEADLOCK_WHEN_THREAD_IS_SUSPENDED_WHILE_BLOCKED_ON_MUTEX", .value = true },
    .{ .name = "HAS_FTRUNCATE_LENGTH_ISSUE", .value = true },
    .{ .name = "STATVFS64_PROTOTYPE_BROKEN", .value = false },
};

pub const Item = union(enum) {
    lit: []const u8,
    b01: []const u8,
    off_t,
    val: struct { name: []const u8, value: []const u8 },
    undef: []const u8,
    ptrace,
};

/// config.h emission order, mirroring pal/src/config.h.in (149 lines).
pub const layout: []const Item = &.{
    .{ .lit = "//-------------------------------------------------------------------------------------------------------" },
    .{ .lit = "// ChakraCore/Pal" },
    .{ .lit = "// Contains portions (c) copyright Microsoft, portions copyright (c) the .NET Foundation and Contributors" },
    .{ .lit = "// and edits (c) copyright the ChakraCore Contributors." },
    .{ .lit = "// See THIRD-PARTY-NOTICES.txt in the project root for .NET Foundation license" },
    .{ .lit = "// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information." },
    .{ .lit = "//-------------------------------------------------------------------------------------------------------" },
    .{ .lit = "" },
    .{ .lit = "#ifndef _PAL_CONFIG_H_INCLUDED" },
    .{ .lit = "#define _PAL_CONFIG_H_INCLUDED 1" },
    .{ .lit = "" },
    .{ .b01 = "HAVE_IEEEFP_H" },
    .{ .b01 = "HAVE_ALLOCA_H" },
    .{ .b01 = "HAVE_SYS_VMPARAM_H" },
    .{ .b01 = "HAVE_MACH_VM_TYPES_H" },
    .{ .b01 = "HAVE_MACH_VM_PARAM_H" },
    .{ .b01 = "HAVE_PROCFS_H" },
    .{ .b01 = "HAVE_CRT_EXTERNS_H" },
    .{ .b01 = "HAVE_SYS_TIME_H" },
    .{ .b01 = "HAVE_PTHREAD_NP_H" },
    .{ .b01 = "HAVE_SYS_LWP_H" },
    .{ .b01 = "HAVE_RUNETYPE_H" },
    .{ .lit = "" },
    .{ .b01 = "HAVE_KQUEUE" },
    .{ .b01 = "HAVE_GETPWUID_R" },
    .{ .b01 = "HAVE_PTHREAD_SUSPEND" },
    .{ .b01 = "HAVE_PTHREAD_SUSPEND_NP" },
    .{ .b01 = "HAVE_PTHREAD_CONTINUE" },
    .{ .b01 = "HAVE_PTHREAD_RESUME_NP" },
    .{ .b01 = "HAVE_PTHREAD_CONTINUE_NP" },
    .{ .b01 = "HAVE_PTHREAD_ATTR_GET_NP" },
    .{ .b01 = "HAVE_PTHREAD_GETATTR_NP" },
    .{ .b01 = "HAVE_PTHREAD_GETCPUCLOCKID" },
    .{ .b01 = "HAVE_PTHREAD_SIGQUEUE" },
    .{ .b01 = "HAVE_SIGRETURN" },
    .{ .b01 = "HAVE__THREAD_SYS_SIGRETURN" },
    .{ .b01 = "HAVE_COPYSIGN" },
    .{ .b01 = "HAVE_FSYNC" },
    .{ .b01 = "HAVE_FUTIMES" },
    .{ .b01 = "HAVE_UTIMES" },
    .{ .b01 = "HAVE_SYSCTL" },
    .{ .b01 = "HAVE_SYSCONF" },
    .{ .b01 = "HAVE_LOCALTIME_R" },
    .{ .b01 = "HAVE_GMTIME_R" },
    .{ .b01 = "HAVE_TIMEGM" },
    .{ .b01 = "HAVE__SNWPRINTF" },
    .{ .b01 = "HAVE_POLL" },
    .{ .b01 = "HAVE_STATVFS" },
    .{ .b01 = "HAVE_THREAD_SELF" },
    .{ .b01 = "HAVE__LWP_SELF" },
    .{ .b01 = "HAVE_MACH_THREADS" },
    .{ .b01 = "HAVE_MACH_EXCEPTIONS" },
    .{ .b01 = "HAVE_VM_ALLOCATE" },
    .{ .b01 = "HAVE_VM_READ" },
    .{ .b01 = "HAS_SYSV_SEMAPHORES" },
    .{ .b01 = "HAS_PTHREAD_MUTEXES" },
    .{ .b01 = "HAVE_TTRACE" },
    .{ .lit = "" },
    .{ .b01 = "HAVE_STAT_TIMESPEC" },
    .{ .b01 = "HAVE_STAT_NSEC" },
    .{ .b01 = "HAVE_TM_GMTOFF" },
    .{ .lit = "" },
    .{ .b01 = "HAVE_BSD_REGS_T" },
    .{ .b01 = "HAVE_PT_REGS" },
    .{ .b01 = "HAVE_GREGSET_T" },
    .{ .b01 = "HAVE_SIGINFO_T" },
    .{ .b01 = "HAVE_UCONTEXT_T" },
    .{ .b01 = "HAVE_PTHREAD_RWLOCK_T" },
    .{ .b01 = "HAVE_PRWATCH_T" },
    .off_t,
    .{ .lit = "" },
    .{ .b01 = "HAVE_YIELD_SYSCALL" },
    .{ .b01 = "HAVE_INFTIM" },
    .{ .b01 = "HAVE_CHAR_BIT" },
    .{ .b01 = "USER_H_DEFINES_DEBUG" },
    .{ .b01 = "HAVE__SC_PHYS_PAGES" },
    .{ .b01 = "HAVE__SC_AVPHYS_PAGES" },
    .{ .lit = "" },
    .{ .b01 = "REALPATH_SUPPORTS_NONEXISTENT_FILES" },
    .{ .b01 = "SSCANF_CANNOT_HANDLE_MISSING_EXPONENT" },
    .{ .b01 = "SSCANF_SUPPORT_ll" },
    .{ .b01 = "HAVE_LARGE_SNPRINTF_SUPPORT" },
    .{ .b01 = "HAVE_BROKEN_FIFO_SELECT" },
    .{ .b01 = "HAVE_BROKEN_FIFO_KEVENT" },
    .{ .b01 = "HAS_FTRUNCATE_LENGTH_ISSUE" },
    .{ .b01 = "HAVE_SCHED_GET_PRIORITY" },
    .{ .b01 = "HAVE_SCHED_GETCPU" },
    .{ .b01 = "HAVE_WORKING_GETTIMEOFDAY" },
    .{ .b01 = "HAVE_WORKING_CLOCK_GETTIME" },
    .{ .b01 = "HAVE_CLOCK_MONOTONIC" },
    .{ .b01 = "HAVE_CLOCK_MONOTONIC_COARSE" },
    .{ .b01 = "HAVE_MACH_ABSOLUTE_TIME" },
    .{ .b01 = "HAVE_CLOCK_THREAD_CPUTIME" },
    .{ .b01 = "STATVFS64_PROTOTYPE_BROKEN" },
    .{ .b01 = "HAVE_MMAP_DEV_ZERO" },
    .{ .b01 = "MMAP_IGNORES_HINT" },
    .{ .b01 = "MMAP_ANON_IGNORES_PROTECTION" },
    .{ .b01 = "MMAP_DOESNOT_ALLOW_REMAP" },
    .{ .b01 = "ONE_SHARED_MAPPING_PER_FILEREGION_PER_PROCESS" },
    .{ .b01 = "PTHREAD_CREATE_MODIFIES_ERRNO" },
    .{ .b01 = "SEM_INIT_MODIFIES_ERRNO" },
    .{ .b01 = "HAVE_PROCFS_CTL" },
    .{ .b01 = "HAVE_COMPATIBLE_ACOS" },
    .{ .b01 = "HAVE_COMPATIBLE_ASIN" },
    .{ .b01 = "HAVE_COMPATIBLE_ATAN2" },
    .{ .b01 = "HAVE_COMPATIBLE_EXP" },
    .{ .b01 = "HAVE_COMPATIBLE_LOG" },
    .{ .b01 = "HAVE_COMPATIBLE_LOG10" },
    .{ .b01 = "UNGETC_NOT_RETURN_EOF" },
    .{ .b01 = "HAS_POSIX_SEMAPHORES" },
    .{ .b01 = "GETPWUID_R_SETS_ERRNO" },
    .{ .b01 = "FILE_OPS_CHECK_FERROR_OF_PREVIOUS_CALL" },
    .{ .lit = "" },
    .{ .lit = "#define PAL_THREAD_PRIORITY_MIN 0" },
    .{ .lit = "#define PAL_THREAD_PRIORITY_MAX 0" },
    .{ .lit = "" },
    .{ .b01 = "HAVE_COREFOUNDATION" },
    .{ .b01 = "HAVE__NSGETENVIRON" },
    .{ .b01 = "DEADLOCK_WHEN_THREAD_IS_SUSPENDED_WHILE_BLOCKED_ON_MUTEX" },
    .ptrace,
    .{ .val = .{ .name = "PAL_PT_ATTACH", .value = "PT_ATTACH" } },
    .{ .val = .{ .name = "PAL_PT_DETACH", .value = "PT_DETACH" } },
    .{ .val = .{ .name = "PAL_PT_READ_D", .value = "PT_READ_D" } },
    .{ .val = .{ .name = "PAL_PT_WRITE_D", .value = "PT_WRITE_D" } },
    .{ .b01 = "SYNCHMGR_SUSPENSION_SAFE_CONDITION_SIGNALING" },
    .{ .b01 = "ERROR_FUNC_FOR_GLOB_HAS_FIXED_PARAMS" },
    .{ .b01 = "HAS_FTRUNCATE_LENGTH_ISSUE" },
    .{ .undef = "FREEBSD_LIBC" },
    .{ .lit = "" },
    .{ .val = .{ .name = "JA_JP_LOCALE_NAME", .value = "\"ja_JP.SJIS\"" } },
    .{ .val = .{ .name = "KO_KR_LOCALE_NAME", .value = "\"ko_KR.eucKR\"" } },
    .{ .val = .{ .name = "ZH_TW_LOCALE_NAME", .value = "\"zh_TG.BIG5\"" } },
    .{ .lit = "" },
    .{ .lit = "#define CHECK_TRACE_SPECIFIERS 0" },
    .{ .lit = "#define PROCFS_MEM_NAME \"\"" },
    .{ .lit = "#define HAVE_GETHRTIME 0" },
    .{ .lit = "#define HAVE_LOWERCASE_ISO_NAME 0" },
    .{ .lit = "#define HAVE_READ_REAL_TIME 0" },
    .{ .lit = "#define HAVE_UNDERSCORE_ISO_NAME 0" },
    .{ .lit = "#define MKSTEMP64_IS_USED_INSTEAD_OF_MKSTEMP 0" },
    .{ .lit = "#define NEED_DLCOMPAT 0" },
    .{ .lit = "#define OPEN64_IS_USED_INSTEAD_OF_OPEN 0" },
    .{ .lit = "#define SELF_SUSPEND_FAILS_WITH_NATIVE_SUSPENSION 0" },
    .{ .lit = "#define SET_SCHEDPARAM_NEEDS_PRIVS 0" },
    .{ .lit = "#define SIGWAIT_FAILS_WHEN_PASSED_FULL_SIGSET 0" },
    .{ .lit = "#define SYNCHMGR_PIPE_BASED_THREAD_BLOCKING 0" },
    .{ .lit = "#define WRITE_0_BYTES_HANGS_TTY 0" },
    .{ .lit = "#define HAVE_FTRUNCATE_LARGE_LENGTH_SUPPORT 1" },
    .{ .lit = "#endif" },
};

pub const ptrace_line = "#define PAL_PTRACE(cmd, pid, addr, data) ptrace((cmd), (pid), (caddr_t)(addr), (data))";

/// Comptime classification counts, derived from the tables above so the
/// summary line cannot drift from the contract.
pub const num_compile_checked: usize = blk: {
    var n: usize = 0;
    for (checks) |c| {
        switch (c.params) {
            .run, .run_werror => {},
            else => n += 1,
        }
    }
    break :blk n;
};

pub const num_runtime_verified: usize = blk: {
    var n: usize = 0;
    for (checks) |c| {
        if (c.params == .run) n += 1;
    }
    break :blk n;
};

/// Fixed-by-target settings: fixed01 entries plus every literal-valued layout
/// item (.val, .undef, .ptrace, and #define .lit lines other than the header
/// guard). Checked (.b01/.off_t) layout items are counted above, not here.
pub const num_fixed_by_target: usize = blk: {
    var n: usize = fixed01.len;
    for (layout) |item| {
        switch (item) {
            .val, .undef, .ptrace => n += 1,
            .lit => |s| {
                if (std.mem.startsWith(u8, s, "#define ") and
                    !std.mem.startsWith(u8, s, "#define _PAL_CONFIG_H_INCLUDED"))
                    n += 1;
            },
            else => {},
        }
    }
    break :blk n;
};

comptime {
    std.debug.assert(num_compile_checked == 62);
    std.debug.assert(num_runtime_verified == 32);
    std.debug.assert(num_fixed_by_target == 33);
    std.debug.assert(checks.len == num_compile_checked + num_runtime_verified + 1); // + tail-calls attribute check
}
