//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Prefix header for compiling bin/ch under macOS AddressSanitizer.
// bin/ch/ch.cpp calls pthread_exit(&retval) under NO_SANITIZE_ADDRESS_CHECK.
// On macOS, Mach exception handler background threads keep running after
// pthread_exit() from the main thread, hanging the process indefinitely.
// Redirecting pthread_exit() in bin/ch to exit() restores clean process exit.
#ifndef TOOLS_BUILD_ASAN_APPLE_PREFIX_H
#define TOOLS_BUILD_ASAN_APPLE_PREFIX_H

#if defined(__APPLE__) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <pthread.h>
#include <stdlib.h>
#ifndef pthread_exit
#define pthread_exit(x) exit(*(int*)(x))
#endif
#endif
#endif

#endif // TOOLS_BUILD_ASAN_APPLE_PREFIX_H
