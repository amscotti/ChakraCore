//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#ifndef _WIN32

#include <cstdint>

extern "C" void __register_frame(const void* ehframe);
extern "C" void __deregister_frame(const void* ehframe);

// LLVM libunwind (linked by Zig's static libc++) implements the Darwin
// __register_frame contract: the argument is a single FDE pointer. GCC's
// libgcc instead expects the CIE at the start of a .eh_frame table. Our
// EmitBuffer/EhFrame buffers are CIE + FDE, so on a libc++ build advance to
// the FDE; a libstdc++ build keeps the CIE pointer.
inline const void* ChakraEhFrameFdeStart(const void* ehframe)
{
#if defined(_LIBCPP_VERSION) && !defined(__APPLE__) && (defined(_M_X64) || defined(__x86_64__))
    const unsigned char* bytes = static_cast<const unsigned char*>(ehframe);
    // DWARF frame entry: 4-byte little-endian length (excluding itself),
    // followed by the CIE id. 64-bit .eh_frame is not emitted here; the
    // 0xffffffff escape would make the skip below wrong, so fail loudly
    // in debug builds.
    uint32_t cieLength = static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
    // A CIE length outside this range cannot describe the CIE+FDE buffers we
    // hand to __register_frame (XDATA_SIZE is 0x80). Do not derive a pointer
    // past the buffer and do not abort the process on malformed input: fall
    // back to the raw pointer so the caller's registration is at least
    // bounded to the object it owns.
    if (cieLength < 4 || cieLength > 0x100)
    {
        return ehframe;
    }
    return bytes + sizeof(uint32_t) + cieLength;
#else
    return ehframe;
#endif
}

#if defined(_AMD64_) && !defined(DISABLE_JIT)
#if defined(__APPLE__) // no multi fde support
#ifndef __IOS__
typedef void(*mac_fde_reg_op)(const void*addr);
void mac_fde_wrapper(const char *dataStart, mac_fde_reg_op reg_op);
#define __REGISTER_FRAME(addr) mac_fde_wrapper((const char*)addr, __register_frame)
#define __DEREGISTER_FRAME(addr) mac_fde_wrapper((const char*)addr, __deregister_frame)
#else // __IOS__ // no JIT
#define __REGISTER_FRAME(addr)
#define __DEREGISTER_FRAME(addr)
#endif // !__IOS__
#else // multi FDE support
#define __REGISTER_FRAME(addr) __register_frame(addr)
#define __DEREGISTER_FRAME(addr) __deregister_frame(addr)
#endif // __APPLE__
#elif defined(_M_ARM64) // _AMD64_ && !DISABLE_JIT
#define __REGISTER_FRAME(addr) __register_frame(addr)
#define __DEREGISTER_FRAME(addr) __deregister_frame(addr)
#else
#define __REGISTER_FRAME(addr)
#define __DEREGISTER_FRAME(addr)
#endif // _AMD64_ && !DISABLE_JIT

#ifdef _M_ARM
#define __iso_volatile_load32(x) (*(const volatile __int32*)x)
#elif defined(_M_ARM64)
#define __iso_volatile_load64(x) (*(const volatile __int64*)x)
#endif

#endif // !_WIN32
