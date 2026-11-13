//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Internal  Listing  Machine
// Name      Name     Encode    Type        BitVec
//------------------------------------------------------------------------

// Illegal registers - must be first and have a value of 0

//           Internal Name
//          /      Listing Name
//         /      /        Machine Encode
//        /      /        /       Type
//       /      /        /       /           BitVec
//      /      /        /       /           /
REGDAT(NOREG, noreg,    0xf,   TyIllegal,  RA_DONTALLOCATE)

// 32 bit Integer Registers
REGDAT(R0,      r0,       ARMREG_R0,   TyInt64,    0)
REGDAT(R1,      r1,       ARMREG_R1,   TyInt64,    0)
REGDAT(R2,      r2,       ARMREG_R2,   TyInt64,    0)
REGDAT(R3,      r3,       ARMREG_R3,   TyInt64,    0)
REGDAT(R4,      r4,       ARMREG_R4,   TyInt64,    0)
REGDAT(R5,      r5,       ARMREG_R5,   TyInt64,    0)
REGDAT(R6,      r6,       ARMREG_R6,   TyInt64,    0)
REGDAT(R7,      r7,       ARMREG_R7,   TyInt64,    0)
REGDAT(R8,      r8,       ARMREG_R8,   TyInt64,    0)
REGDAT(R9,      r9,       ARMREG_R9,   TyInt64,    0)
REGDAT(R10,     r10,      ARMREG_R10,  TyInt64,    0)
REGDAT(R11,     r11,      ARMREG_R11,  TyInt64,    0)
REGDAT(R12,     r12,      ARMREG_R12,  TyInt64,    0)
REGDAT(R13,     r13,      ARMREG_R13,  TyInt64,    0)
REGDAT(R14,     r14,      ARMREG_R14,  TyInt64,    0)
REGDAT(R15,     r15,      ARMREG_R15,  TyInt64,    0)
REGDAT(R16,     r16,      ARMREG_R16,  TyInt64,    RA_DONTALLOCATE) // x16, x17 are "inter-procedural scratch registers", linker is free to insert stubs/islands/etc that use both of them and the surrounding code cannot assume they will be preserved
REGDAT(R17,     r17,      ARMREG_R17,  TyInt64,    RA_DONTALLOCATE) // scratch
REGDAT(R18,     r18,      ARMREG_R18,  TyInt64,    RA_DONTALLOCATE) // platform register (TEB). x18 is a platform register, should not be touched/modified/saved/restored
REGDAT(R19,     r19,      ARMREG_R19,  TyInt64,    RA_CALLEESAVE)
REGDAT(R20,     r20,      ARMREG_R20,  TyInt64,    RA_CALLEESAVE)
REGDAT(R21,     r21,      ARMREG_R21,  TyInt64,    RA_CALLEESAVE)
REGDAT(R22,     r22,      ARMREG_R22,  TyInt64,    RA_CALLEESAVE)
REGDAT(R23,     r23,      ARMREG_R23,  TyInt64,    RA_CALLEESAVE)
REGDAT(R24,     r24,      ARMREG_R24,  TyInt64,    RA_CALLEESAVE)
REGDAT(R25,     r25,      ARMREG_R25,  TyInt64,    RA_CALLEESAVE)
REGDAT(R26,     r26,      ARMREG_R26,  TyInt64,    RA_CALLEESAVE)
REGDAT(R27,     r27,      ARMREG_R27,  TyInt64,    RA_CALLEESAVE)
REGDAT(R28,     r28,      ARMREG_R28,  TyInt64,    RA_CALLEESAVE)
REGDAT(FP,      fp,       ARMREG_FP,   TyInt64,    RA_DONTALLOCATE)
REGDAT(LR,      lr,       ARMREG_LR,   TyInt64,    0)
REGDAT(SP,      sp,       ARMREG_SP,   TyInt64,    RA_DONTALLOCATE)
REGDAT(ZR,      zr,       ARMREG_ZR,   TyInt64,    RA_DONTALLOCATE)
//REGDAT(PC,      pc,       ARMREG_PC,  TyInt64,    RA_DONTALLOCATE)

#ifndef INT_ARG_REG_COUNT
#define INT_ARG_REG_COUNT 8
#endif

// VFP Floating Point Registers
REGDAT(D0,      d0,       NEONREG_D0,    TyFloat64,  0)
REGDAT(D1,      d1,       NEONREG_D1,    TyFloat64,  0)
REGDAT(D2,      d2,       NEONREG_D2,    TyFloat64,  0)
REGDAT(D3,      d3,       NEONREG_D3,    TyFloat64,  0)
REGDAT(D4,      d4,       NEONREG_D4,    TyFloat64,  0)
REGDAT(D5,      d5,       NEONREG_D5,    TyFloat64,  0)
REGDAT(D6,      d6,       NEONREG_D6,    TyFloat64,  0)
REGDAT(D7,      d7,       NEONREG_D7,    TyFloat64,  0)
REGDAT(D8,      d8,       NEONREG_D8,    TyFloat64,  RA_CALLEESAVE)
REGDAT(D9,      d9,       NEONREG_D9,    TyFloat64,  RA_CALLEESAVE)
REGDAT(D10,     d10,      NEONREG_D10,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D11,     d11,      NEONREG_D11,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D12,     d12,      NEONREG_D12,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D13,     d13,      NEONREG_D13,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D14,     d14,      NEONREG_D14,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D15,     d15,      NEONREG_D15,   TyFloat64,  RA_CALLEESAVE)
REGDAT(D16,     d16,      NEONREG_D16,   TyFloat64,  0)
REGDAT(D17,     d17,      NEONREG_D17,   TyFloat64,  0)
REGDAT(D18,     d18,      NEONREG_D18,   TyFloat64,  0)
REGDAT(D19,     d19,      NEONREG_D19,   TyFloat64,  0)
REGDAT(D20,     d20,      NEONREG_D20,   TyFloat64,  0)
REGDAT(D21,     d21,      NEONREG_D21,   TyFloat64,  0)
REGDAT(D22,     d22,      NEONREG_D22,   TyFloat64,  0)
REGDAT(D23,     d23,      NEONREG_D23,   TyFloat64,  0)
REGDAT(D24,     d24,      NEONREG_D24,   TyFloat64,  0)
REGDAT(D25,     d25,      NEONREG_D25,   TyFloat64,  0)
REGDAT(D26,     d26,      NEONREG_D26,   TyFloat64,  0)
REGDAT(D27,     d27,      NEONREG_D27,   TyFloat64,  0)
REGDAT(D28,     d28,      NEONREG_D28,   TyFloat64,  0)
REGDAT(D29,     d29,      NEONREG_D29,   TyFloat64,  0)

// Arm64 has 66 register definitions. Ignore 2 for now to avoid going past the 64 that will fit into a bitvector during register allocation.
// TODO: Reorder registers instead to place DONT_ALLOCATE regs at the end. Requires some changes in LinearScan or for RegNumCount to be in the middle of the list.
//REGDAT(D30,     d30,      NEONREG_D30,   TyFloat64,  RA_CALLEESAVE)
//REGDAT(D31,     d31,      NEONREG_D31,   TyFloat64,  RA_CALLEESAVE)

// W8-NEON slice 1 Q registers (128-bit). NEONREG_Qn (ARM64Encoder.h:82) is
// the same encode as NEONREG_Dn (same hardware vector register, wider view),
// and Q0-31 as distinct RegNums would overflow the 64-bit BitVector
// allocator (RegNumCount is 64). The Q class is therefore defined as aliases
// to D (see Reg.h: RegQn = RegDn, TyFloat64 per x64 XMM precedent, Q8-15
// callee-saved exactly as D8-15). The listing below documents the Q set for
// grep/encoder parity; the RegNum/encode mappings live in Reg.h.
// Q0  NEONREG_Q0  TyFloat64  caller-saved (alias RegD0)
// Q1  NEONREG_Q1  TyFloat64  caller-saved (alias RegD1)
// Q2  NEONREG_Q2  TyFloat64  caller-saved (alias RegD2)
// Q3  NEONREG_Q3  TyFloat64  caller-saved (alias RegD3)
// Q4  NEONREG_Q4  TyFloat64  caller-saved (alias RegD4)
// Q5  NEONREG_Q5  TyFloat64  caller-saved (alias RegD5)
// Q6  NEONREG_Q6  TyFloat64  caller-saved (alias RegD6)
// Q7  NEONREG_Q7  TyFloat64  caller-saved (alias RegD7)
// Q8  NEONREG_Q8  TyFloat64  callee-saved (alias RegD8)
// Q9  NEONREG_Q9  TyFloat64  callee-saved (alias RegD9)
// Q10 NEONREG_Q10 TyFloat64  callee-saved (alias RegD10)
// Q11 NEONREG_Q11 TyFloat64  callee-saved (alias RegD11)
// Q12 NEONREG_Q12 TyFloat64  callee-saved (alias RegD12)
// Q13 NEONREG_Q13 TyFloat64  callee-saved (alias RegD13)
// Q14 NEONREG_Q14 TyFloat64  callee-saved (alias RegD14)
// Q15 NEONREG_Q15 TyFloat64  callee-saved (alias RegD15)
// Q16 NEONREG_Q16 TyFloat64  caller-saved (alias RegD16)
// Q17 NEONREG_Q17 TyFloat64  caller-saved (alias RegD17)
// Q18 NEONREG_Q18 TyFloat64  caller-saved (alias RegD18)
// Q19 NEONREG_Q19 TyFloat64  caller-saved (alias RegD19)
// Q20 NEONREG_Q20 TyFloat64  caller-saved (alias RegD20)
// Q21 NEONREG_Q21 TyFloat64  caller-saved (alias RegD21)
// Q22 NEONREG_Q22 TyFloat64  caller-saved (alias RegD22)
// Q23 NEONREG_Q23 TyFloat64  caller-saved (alias RegD23)
// Q24 NEONREG_Q24 TyFloat64  caller-saved (alias RegD24)
// Q25 NEONREG_Q25 TyFloat64  caller-saved (alias RegD25)
// Q26 NEONREG_Q26 TyFloat64  caller-saved (alias RegD26)
// Q27 NEONREG_Q27 TyFloat64  caller-saved (alias RegD27)
// Q28 NEONREG_Q28 TyFloat64  caller-saved (alias RegD28)
// Q29 NEONREG_Q29 TyFloat64  caller-saved (alias RegD29)
// Q30 NEONREG_Q30 TyFloat64  caller-saved (alias RegD0; D30 excluded for BitVector limit)
// Q31 NEONREG_Q31 TyFloat64  caller-saved (alias RegD1; D31 excluded for BitVector limit)
#define FIRST_SIMD_ARG_REG RegQ0
#define LAST_SIMD_REG RegQ29
#define LAST_SIMD_REG_NUM 29
#define FIRST_SIMD_CALLEE_SAVED_REG RegQ8
#define LAST_SIMD_CALLEE_SAVED_REG RegQ15
#define SIMD_REGCOUNT VFP_REGCOUNT

// NOTE: Any ordering change in reg list or addition/deletion requires a corresponding update in SaveAllRegistersAndBailOut/SaveAllRegistersAndBranchBailOut

#define FIRST_DOUBLE_ARG_REG RegD0
#define LAST_DOUBLE_REG RegD29
#define LAST_DOUBLE_REG_NUM 29
#define LAST_FLOAT_REG_NUM 29
#define FIRST_DOUBLE_CALLEE_SAVED_REG_NUM 16
#define LAST_DOUBLE_CALLEE_SAVED_REG_NUM 29
#define VFP_REGCOUNT\
    ((LAST_DOUBLE_REG - FIRST_DOUBLE_ARG_REG) + 1)

#define REGNUM_ISVFPREG(r) ((r) >= RegD0 && (r) <= LAST_DOUBLE_REG)

