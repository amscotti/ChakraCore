//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#define RA_DONTALLOCATE     0x1
#define RA_CALLEESAVE       0x2

enum RegNum : BYTE {
#define REGDAT(Name, Listing,    Encode,    Type,    BitVec)  Reg ## Name,
#include "RegList.h"
#undef REGDAT

    RegNumCount,  // Number of operations
};

// W8-NEON slice 1: Q (128-bit) and D (64-bit) are the same hardware vector
// registers (ARM64Encoder.h: NEONREG_Dn == NEONREG_Qn). Distinct RegNums for
// Q would duplicate hardware and overflow the 64-bit BitVector allocator
// (RegNumCount is exactly 64). Q0-29 therefore alias D0-29; Q30/Q31 alias
// D0/D1 (D30/D31 already excluded for the same limit, see RegList.h). The
// Reg type stays TyFloat64 per the x64 XMM precedent (Shared LinearScan.cpp
// groups IsFloat64||IsSimd128 into floatRegs). Spill width is keyed off the
// operand type (IsSimd128 -> 16B) inside LinearScanMD.cpp, not off a
// separate RegNum range, for the same reason. Q8-15 are callee-saved exactly
// as D8-15 (same registers).
enum {
    RegQ0 = RegD0,
    RegQ1 = RegD1,
    RegQ2 = RegD2,
    RegQ3 = RegD3,
    RegQ4 = RegD4,
    RegQ5 = RegD5,
    RegQ6 = RegD6,
    RegQ7 = RegD7,
    RegQ8 = RegD8,
    RegQ9 = RegD9,
    RegQ10 = RegD10,
    RegQ11 = RegD11,
    RegQ12 = RegD12,
    RegQ13 = RegD13,
    RegQ14 = RegD14,
    RegQ15 = RegD15,
    RegQ16 = RegD16,
    RegQ17 = RegD17,
    RegQ18 = RegD18,
    RegQ19 = RegD19,
    RegQ20 = RegD20,
    RegQ21 = RegD21,
    RegQ22 = RegD22,
    RegQ23 = RegD23,
    RegQ24 = RegD24,
    RegQ25 = RegD25,
    RegQ26 = RegD26,
    RegQ27 = RegD27,
    RegQ28 = RegD28,
    RegQ29 = RegD29,
    RegQ30 = RegD0,
    RegQ31 = RegD1,
};

#define FIRST_FLOAT_REG RegD0
#define FIRST_SIMD_REG RegQ0
#define REGNUM_ISQREG(r) ((r) >= RegQ0 && (r) <= RegQ29)
#define REGNUM_ISSIMDREG(r) REGNUM_ISQREG(r)

#define FOREACH_REG(reg) \
        for (RegNum reg = (RegNum)(RegNOREG+1); reg != RegNumCount; reg = (RegNum)(reg+1))
#define NEXT_REG

#define FOREACH_INT_REG(reg) \
        for (RegNum reg = (RegNum)(RegNOREG+1); reg != FIRST_FLOAT_REG; reg = (RegNum)(reg+1))
#define NEXT_INT_REG

#define FOREACH_FLOAT_REG(reg) \
        for (RegNum reg = FIRST_FLOAT_REG; reg != RegNumCount; reg = (RegNum)(reg+1))
#define NEXT_FLOAT_REG

