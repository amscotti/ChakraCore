//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//! Probe sources for the B4.2 Linux PAL configuration checks.
//!
//! The `check_cxx_source_runs` bodies in `pal/src/configure.cmake` are
//! platform-independent sources: the Linux reference configure ran the
//! byte-identical bodies the Mac contract already transcribed (defects
//! included). This file re-exports them from the Mac transcription instead
//! of duplicating ~1000 lines; `contract.zig` reads exactly as if they were
//! local. See doc/pal-config-contract-linux.md section 5 item 3: the Linux
//! check reproduces the probes as run, which means these same bodies under
//! the Linux probe command lines (notably the inherited `-Werror`).
//!
//! The re-exported per-body `contract expectation:` comments
//! carry Mac-contract values; Linux expectations live in `contract.zig`.
//! Probe-source changes require re-verifying both contracts' expectations:
//! a Mac body fix auto-propagates through this re-export and will trip the
//! Linux fail-clearly gate on the L1-L9 compile-failure expectations.
//!
//! The import is a plain relative path: inside the `@build` module graph
//! (build.zig -> tools/build/linux.zig -> ../pal-config-linux/*) the root
//! is the build root, so `..` resolves relative to the importing file and
//! no module wiring is needed. Standalone `zig build-obj` of these files
//! is not possible (0.17 rejects `..` imports that leave the main file's
//! own dir); compile verification goes through `zig build` configure,
//! which compiles this exact graph.

const mac = @import("../pal-config/probes.zig");

pub const func_tmpl = mac.func_tmpl;

pub const run_realpath_supports_nonexistent_files = mac.run_realpath_supports_nonexistent_files;
pub const run_sscanf_support_ll = mac.run_sscanf_support_ll;
pub const run_sscanf_cannot_handle_missing_exponent = mac.run_sscanf_cannot_handle_missing_exponent;
pub const run_have_large_snprintf_support = mac.run_have_large_snprintf_support;
pub const run_have_broken_fifo_select = mac.run_have_broken_fifo_select;
pub const run_have_broken_fifo_kevent = mac.run_have_broken_fifo_kevent;
pub const run_have_sched_get_priority = mac.run_have_sched_get_priority;
pub const run_have_sched_getcpu = mac.run_have_sched_getcpu;
pub const run_have_working_gettimeofday = mac.run_have_working_gettimeofday;
pub const run_have_working_clock_gettime = mac.run_have_working_clock_gettime;
pub const run_have_clock_monotonic = mac.run_have_clock_monotonic;
pub const run_have_clock_monotonic_coarse = mac.run_have_clock_monotonic_coarse;
pub const run_have_mach_absolute_time = mac.run_have_mach_absolute_time;
pub const run_have_clock_thread_cputime = mac.run_have_clock_thread_cputime;
pub const run_have_mmap_dev_zero = mac.run_have_mmap_dev_zero;
pub const run_mmap_ignores_hint = mac.run_mmap_ignores_hint;
pub const run_mmap_anon_ignores_protection = mac.run_mmap_anon_ignores_protection;
pub const run_mmap_doesnot_allow_remap = mac.run_mmap_doesnot_allow_remap;
pub const run_one_shared_mapping_per_fileregion_per_process = mac.run_one_shared_mapping_per_fileregion_per_process;
pub const run_pthread_create_modifies_errno = mac.run_pthread_create_modifies_errno;
pub const run_sem_init_modifies_errno = mac.run_sem_init_modifies_errno;
pub const run_have_procfs_ctl = mac.run_have_procfs_ctl;
pub const run_have_compatible_acos = mac.run_have_compatible_acos;
pub const run_have_compatible_asin = mac.run_have_compatible_asin;
pub const run_have_compatible_atan2 = mac.run_have_compatible_atan2;
pub const run_have_compatible_exp = mac.run_have_compatible_exp;
pub const run_have_compatible_log = mac.run_have_compatible_log;
pub const run_have_compatible_log10 = mac.run_have_compatible_log10;
pub const run_ungetc_not_return_eof = mac.run_ungetc_not_return_eof;
pub const run_has_posix_semaphores = mac.run_has_posix_semaphores;
pub const run_getpwuid_r_sets_errno = mac.run_getpwuid_r_sets_errno;
pub const run_file_ops_check_ferror_of_previous_call = mac.run_file_ops_check_ferror_of_previous_call;
pub const run_clang_has_disable_tail_calls = mac.run_clang_has_disable_tail_calls;
