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

// LLVM libunwind (linked on Darwin and by Zig's static libc++ ELF builds)
// implements the Darwin single-FDE __register_frame(fde) contract, whereas
// GNU libgcc_eh expects the CIE at the start of a .eh_frame table. Builds may
// explicitly define CHAKRA_UNWINDER_SINGLE_FDE (1 = single FDE, 0 = CIE table);
// otherwise infer it from ELF+libc++/libunwind or Apple Mach-O.
#ifndef CHAKRA_UNWINDER_SINGLE_FDE
#if defined(__APPLE__) || defined(__LLVM_LIBUNWIND__) || (defined(__ELF__) && defined(_LIBCPP_VERSION))
#define CHAKRA_UNWINDER_SINGLE_FDE 1
#else
#define CHAKRA_UNWINDER_SINGLE_FDE 0
#endif
#endif

// Advance past a leading DWARF .eh_frame CIE to its single FDE when targeting
// an ELF single-FDE unwinder. On __APPLE__ Mach-O, mac_fde_wrapper already
// performs the CIE-to-FDE walk from the CIE pointer, so return ehframe as-is.
inline const void* ChakraEhFrameFdeStart(const void* ehframe)
{
#if !defined(__APPLE__) && defined(__ELF__) && CHAKRA_UNWINDER_SINGLE_FDE
    const unsigned char* bytes = static_cast<const unsigned char*>(ehframe);
    // DWARF .eh_frame CIE header: 4-byte little-endian length (excluding
    // itself), followed by 4-byte CIE_id == 0. Reject 0xffffffff escapes,
    // non-CIE records (cieId != 0), and lengths outside XDATA_SIZE bounds so
    // non-CIE/malformed inputs return ehframe untouched.
    uint32_t cieLength = static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
    uint32_t cieId = static_cast<uint32_t>(bytes[4])
        | (static_cast<uint32_t>(bytes[5]) << 8)
        | (static_cast<uint32_t>(bytes[6]) << 16)
        | (static_cast<uint32_t>(bytes[7]) << 24);
    if (cieId != 0 || cieLength < 4 || cieLength > 0x100)
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
#elif defined(_M_ARM64) && !defined(DISABLE_JIT) && (defined(__APPLE__) || defined(CHAKRA_ARM64_DWARF_EH_FRAME))
// Only Darwin Mach-O (and builds explicitly emitting ARM64 DWARF .eh_frame)
// register DWARF unwind records; ELF ARM64 currently emits Windows .xdata.
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
