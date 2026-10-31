//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// failthreads.c — driver-side thread-exhaustion shim for the
// host-spawn-check A/B evidence (see README.md in this directory).
//
// macOS offers no per-process pthread budget knob an unprivileged driver
// can turn:
//   - RLIMIT_NPROC (ulimit -u) gates fork()/posix_spawn() only; threads
//     do not count. Measured: 40 node worker_threads spawn fine under
//     `ulimit -u 5`; ch spawns 20 agents fine under `ulimit -u 5`.
//   - kern.num_taskthreads (16384 on the evidence machine) is a
//     system-wide boot sysctl, not settable per process.
// So this interposer simulates the OS refusing thread creation at the
// exact boundary real exhaustion crosses: pthread_create returns EAGAIN
// (the resource-exhaustion errno) for a bounded window, every other call
// goes through untouched. Injecting at the libc boundary means the PAL
// takes its genuine failure path end to end (pal/src/thread/pal_thread.cpp
// pthread_create -> ERROR_NOT_ENOUGH_MEMORY -> cleanup ->
// CreateThread returns NULL) — the same path the S6e campaigns' 100-400
// runtime churn hits at the process thread limit. No engine or host
// source is touched; the product binaries are byte-identical with or
// without the shim.
//
// Build:  cc -shared -fPIC -o failthreads.dylib failthreads.c
// Use:    DYLD_INSERT_LIBRARIES=<abs>/failthreads.dylib \
//         FAILTHREADS_AT=8 FAILTHREADS_DENY=1 <ch> -Test262 spawnfail.js
//
// Interposition uses the __DATA,__interpose section, not symbol
// shadowing: modern dyld resolves chained-fixup binds straight to the
// ordinal library, so an inserted dylib's same-name export is never
// consulted (measured: the bind goes to libsystem_pthread directly);
// dyld rebinds every reference when an image carries an __interpose
// tuple. The wrapper calls pthread_create DIRECTLY — dyld never rebinds
// references inside the image that declared the tuple, so that is the
// real libSystem function. Resolving lazily with dlsym(RTLD_NEXT, ...)
// inside the wrapper instead re-enters it through dyld's own thread
// creation before the pointer is cached and overflows the stack
// (measured: SIGSEGV on the first create).
//
// Env knobs:
//   FAILTHREADS_AT=N    fail pthread_create calls strictly after the
//                       first N successful ones (absent/invalid =>
//                       disarmed pass-through). 0 = deny all.
//   FAILTHREADS_DENY=D  stop denying after D denials (default 0 =
//                       deny forever). D=1 models the transient
//                       exhaustion spike of agent churn: one spawn is
//                       refused, the next one succeeds.
//   FAILTHREADS_LOG=1   also fprintf each decision to stderr
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define DYLD_INTERPOSE(replacement, replacee) \
  __attribute__((used)) static struct { const void* r; const void* e; } \
  _interpose_##replacee \
  __attribute__((section("__DATA,__interpose"))) = { \
    (const void*)(unsigned long)&replacement, \
    (const void*)(unsigned long)&replacee };

// NOTE: counters are non-atomic longs. Safe only under the driver's
// single-spawning-thread serialization (agents spawn sequentially through
// the host spawn-wait handshake); not a general-purpose interposer.
static long g_created = -1;

static long budget(void)
{
    const char* s = getenv("FAILTHREADS_AT");
    if (s == NULL) return -1;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0) return -1;
    return v;
}

static int failthreads_create(pthread_t* t, const pthread_attr_t* attr,
                              void* (*start)(void*), void* arg);

DYLD_INTERPOSE(failthreads_create, pthread_create)

static int failthreads_create(pthread_t* t, const pthread_attr_t* attr,
                              void* (*start)(void*), void* arg)
{
    static long g_denied = 0;

    if (g_created < 0)
    {
        g_created = 0;            // first call initializes the counter
    }
    long at = budget();
    long deny = 0;                // 0 = deny every call past the budget
    const char* ds = getenv("FAILTHREADS_DENY");
    if (ds != NULL) deny = strtol(ds, NULL, 10);

    if (at >= 0 && g_created >= at && (deny <= 0 || g_denied < deny))
    {
        g_denied++;
        if (getenv("FAILTHREADS_LOG"))
            fprintf(stderr, "[failthreads] denying pthread_create #%ld (budget %ld, denied %ld)\n",
                    g_created + 1, at, g_denied);
        return EAGAIN;            // what a thread-exhausted kernel returns
    }

    int rc = pthread_create(t, attr, start, arg);  // real one, see header
    if (rc == 0) g_created++;
    if (getenv("FAILTHREADS_LOG"))
        fprintf(stderr, "[failthreads] create #%ld -> %d\n", g_created, rc);
    return rc;
}
