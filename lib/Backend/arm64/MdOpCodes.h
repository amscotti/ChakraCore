//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Instruction encoding macros.  Used by ARM instruction encoder.
//
#define UNUSED 0

//            opcode
//           /           layout
//          /           /          attrib          byte2
//         /           /          /               /         form
//        /           /          /               /         /               unused
//       /           /          /               /         /               /
//      /           /          /               /         /               /         dope
//     /           /          /               /         /               /         /

MACRO(ADD,        Reg3,       0,              UNUSED,   LEGAL_ADDSUB,   UNUSED,   D___)
MACRO(ADDS,       Reg3,       OpSideEffect,   UNUSED,   LEGAL_ADDSUB,   UNUSED,   D__S)
MACRO(ADR,        Reg3,       0,              UNUSED,   LEGAL_LABEL,    UNUSED,   D___)
MACRO(AND,        Reg3,       0,              UNUSED,   LEGAL_ALU3,     UNUSED,   D___)
MACRO(ANDS,       Reg3,       0,              UNUSED,   LEGAL_ALU3,     UNUSED,   D__S)
MACRO(ASR,        Reg3,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(B,          Br,         OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BEQ,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BNE,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BLT,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BLE,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BGT,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BGE,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BCS,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BCC,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BHI,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BLS,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BMI,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BPL,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BVS,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BVC,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_BLAB,     UNUSED,   D___)
MACRO(BFI,        Reg3,       0,              UNUSED,   LEGAL_BITFIELD, UNUSED,   D___)
MACRO(BFXIL,      Reg3,       0,              UNUSED,   LEGAL_BITFIELD, UNUSED,   D___)
MACRO(BIC,        Reg3,       OpSideEffect,   UNUSED,   LEGAL_ALU3,     UNUSED,   D___)
MACRO(BL,         CallI,      OpSideEffect,   UNUSED,   LEGAL_CALL,     UNUSED,   D___)
MACRO(BLR,        CallI,      OpSideEffect,   UNUSED,   LEGAL_REG2_ND,  UNUSED,   D___)
MACRO(BR,         Br,         OpSideEffect,   UNUSED,   LEGAL_REG2_ND,  UNUSED,   D___)
MACRO(CBZ,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_CBZ,      UNUSED,   D___)
MACRO(CBNZ,       BrReg2,     OpSideEffect,   UNUSED,   LEGAL_CBZ,      UNUSED,   D___)
MACRO(CLZ,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(RBIT,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(CMP,        Reg1,       OpSideEffect,   UNUSED,   LEGAL_PSEUDO,   UNUSED,   D__S)
MACRO(CMN,        Reg1,       OpSideEffect,   UNUSED,   LEGAL_PSEUDO,   UNUSED,   D__S)
// CMP src1, src2, SXTW -- used in multiply overflow checks
MACRO(CMP_SXTW,   Reg1,       OpSideEffect,   UNUSED,   LEGAL_REG3_ND,  UNUSED,   D__S)
// CSELcc src1, src2 -- select src1 if cc or src2 if not
MACRO(CSELEQ,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CSELNE,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CSELLT,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
// CSNEGPL src1, src2 -- select src1 if PL or -src1 if not; used in integer absolute value
MACRO(CSNEGPL,    Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(DEBUGBREAK, Reg1,       OpSideEffect,   UNUSED,   LEGAL_NONE,     UNUSED,   D___)
MACRO(EOR,        Reg3,       0,              UNUSED,   LEGAL_ALU3,     UNUSED,   D___)
// EOR src1, src2, ASR #31/63 -- used in floating-point-to-integer overflow checks
MACRO(EOR_ASR31,  Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(LDIMM,      Reg2,       0,              UNUSED,   LEGAL_LDIMM,    UNUSED,   DM__)
MACRO(LDP,        Reg3,       0,              UNUSED,   LEGAL_LOADP,    UNUSED,   DL__)
MACRO(LDP_POST,   Reg3,       0,              UNUSED,   LEGAL_LOADP,    UNUSED,   DL__)
MACRO(LDR,        Reg2,       0,              UNUSED,   LEGAL_LOAD,     UNUSED,   DL__)
MACRO(LDRS,       Reg2,       0,              UNUSED,   LEGAL_LOAD,     UNUSED,   DL__)
MACRO(LEA,        Reg3,       0,              UNUSED,   LEGAL_LEA,      UNUSED,   D___)
MACRO(LSL,        Reg2,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(LSR,        Reg2,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(MOV,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   DM__)
// Alias of MOV that won't get optimized out when src and dst are the same.
MACRO(MOV_TRUNC,  Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   DM__)
MACRO(MOVK,       Reg2,       0,              UNUSED,   LEGAL_LDIMM_S,    UNUSED,   DM__)
MACRO(MOVN,       Reg2,       0,              UNUSED,   LEGAL_LDIMM_S,    UNUSED,   DM__)
MACRO(MOVZ,       Reg2,       0,              UNUSED,   LEGAL_LDIMM_S,    UNUSED,   DM__)
MACRO(MRS_FPCR,   Reg1,       0,              UNUSED,   LEGAL_REG1,     UNUSED,   D___)
MACRO(MRS_FPSR,   Reg1,       0,              UNUSED,   LEGAL_REG1,     UNUSED,   D___)
MACRO(MSR_FPCR,   Reg2,       0,              UNUSED,   LEGAL_REG2_ND,  UNUSED,   D___)
MACRO(MSR_FPSR,   Reg2,       0,              UNUSED,   LEGAL_REG2_ND,  UNUSED,   D___)
MACRO(MSUB,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(MUL,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(MVN,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(NOP,        Empty,      0,              UNUSED,   LEGAL_NONE,     UNUSED,   D___)
MACRO(ORR,        Reg3,       0,              UNUSED,   LEGAL_ALU3,     UNUSED,   D___)
MACRO(PLD,        Reg2,       0,              UNUSED,   LEGAL_PLD,      UNUSED,   DL__)
MACRO(REM,        Reg3,       OpSideEffect,   UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(RET,        Reg2,       OpSideEffect,   UNUSED,   LEGAL_REG2_ND,  UNUSED,   D___)
MACRO(RORV,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SBFX,       Reg3,       0,              UNUSED,   LEGAL_BITFIELD, UNUSED,   D___)
MACRO(SDIV,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SMADDL,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SMULL,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(STP,        Reg3,       0,              UNUSED,   LEGAL_STOREP,   UNUSED,   DL__)
MACRO(STP_PRE,    Reg3,       0,              UNUSED,   LEGAL_STOREP,   UNUSED,   DL__)
MACRO(STR,        Reg2,       0,              UNUSED,   LEGAL_STORE,    UNUSED,   DS__)
MACRO(SUB,        Reg3,       0,              UNUSED,   LEGAL_ADDSUB,   UNUSED,   D___)
// SUB dst, src1, src2 LSL #4 -- used in prologs with _chkstk calls
MACRO(SUB_LSL4,   Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SUBS,       Reg3,       OpSideEffect,   UNUSED,   LEGAL_ADDSUB,   UNUSED,   D__S)
MACRO(TBNZ,       BrReg2,     OpSideEffect,   UNUSED,   LEGAL_TBZ,      UNUSED,   D___)
MACRO(TBZ,        BrReg2,     OpSideEffect,   UNUSED,   LEGAL_TBZ,      UNUSED,   D___)
MACRO(TST,        Reg2,       OpSideEffect,   UNUSED,   LEGAL_PSEUDO,   UNUSED,   D__S)
MACRO(UBFX,       Reg3,       0,              UNUSED,   LEGAL_BITFIELD, UNUSED,   D___)
MACRO(UDIV,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(UREM,       Reg3,       OpSideEffect,   UNUSED,   LEGAL_REG3,     UNUSED,   D___)

// Pseudo-op that loads the size of the arg out area. A special op with no src is used so that the
// actual arg out size can be fixed up by the encoder.
MACRO(LDARGOUTSZ, Reg1,       0,              UNUSED,   LEGAL_REG1,     UNUSED,   D___)

//VFP instructions:
MACRO(FABS,        Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FADD,        Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
// FCSELcc src1, src2 -- select src1 if cc or src2 if not
MACRO(FCSELEQ,     Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FCSELNE,     Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FCMP,        Reg1,      OpSideEffect,   UNUSED,   LEGAL_REG3_ND,  UNUSED,   D___)
MACRO(FCVT,        Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTM,       Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTN,       Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTP,       Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTZ,       Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FDIV,        Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FLDR,        Reg2,      0,              UNUSED,   LEGAL_LOAD,     UNUSED,   DL__)
MACRO(FLDP,        Reg2,      0,              UNUSED,   LEGAL_LOADP,    UNUSED,   DL__)
MACRO(FMIN,        Reg2,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FMAX,        Reg2,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FMOV,        Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   DM__)
MACRO(FMOV_GEN,    Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   DM__)
MACRO(FMUL,        Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FNEG,        Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FRINTM,      Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FRINTP,      Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FSUB,        Reg3,      0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FSQRT,       Reg2,      0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FSTR,        Reg2,      0,              UNUSED,   LEGAL_STORE,    UNUSED,   DS__)
MACRO(FSTP,        Reg2,      0,              UNUSED,   LEGAL_STOREP,   UNUSED,   DS__)

// W8-NEON slice 1 vector ops (128-bit Q). Appended after FSTP to preserve
// existing MD opcode values.
MACRO(MOVQ,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   DM__)
MACRO(STPQ,        Reg3,       0,              UNUSED,   LEGAL_STOREP,   UNUSED,   DS__)
MACRO(LDPQ,        Reg3,       0,              UNUSED,   LEGAL_LOADP,    UNUSED,   DL__)
MACRO(PAND,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(POR,         Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PXOR,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PADDD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PSUBD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PADDQ,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PSUBQ,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(ADDPS,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SUBPS,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(MULPS,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(DIVPS,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SQRTPS,      Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
// W8-NEON slice 2e (D2 arithmetic): float64x2 add/sub/mul/div/sqrt. Same
// vector shape as the F4 family (EmitNeonFadd etc.) with SIZE_2D. Reg3 for
// the binary ops, Reg2 for sqrt.
MACRO(ADDPD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SUBPD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(MULPD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(DIVPD,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SQRTPD,      Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)

// W8-NEON slice 2a: integer compares, vector NOT, integer multiply.
// Lane size comes from the IR type via NeonSizeForSimdType (like PADDD),
// so one MD opcode covers 4S/8H/16B. CMGE/CMHS give signed/unsigned Ge
// directly; Lt/Le reach them via operand swap in
// Simd128TryLowerMappedInstruction; Ne (and only Ne) needs a CMEQ+PMVN
// pair. PMVN is bitwise (SIZE_16B) so one opcode covers every Not lane
// shape; PMULW wraps per lane like wasm (low half kept).
MACRO(CMEQ,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CMGT,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CMGE,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CMHI,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(CMHS,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(PMVN,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(PMULW,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)

// W8-NEON slice 2e: negate + cross-lane reduction. NEGV is integer negate
// (2's complement per lane); FNEGV is float negate (sign-bit flip per lane,
// NaN-quiet). UMAXV/UMINV reduce a vector to one scalar lane (unsigned max /
// min across lanes; the scalar lands in lane 0). Lane sizes come from the IR
// type via NeonSizeForSimdType. LEGAL_REG2: operands are registers only;
// lane-index immediates (UMOVV lane 0 in the any/all_true expansion) use the
// LEGAL_SHIFT form of UMOVV, and the final 0/1 shift uses LEGAL_SHIFT LSR.
MACRO(NEGV,        Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FNEGV,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(UMAXV,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(UMINV,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FABSV,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)

// W8-NEON slice 2b: SIMD lane ops + float<->int conversions.
// Lane sizes come from the IR type (NeonSizeForSimdType, or the encoder's
// scalar mapping); signedness (UMOV vs SMOV) is chosen by the lowerer from
// the IR opcode. Splat is INSV lane 0 followed by DUPELV (DUP-element
// replicates one lane to all lanes; each MD instr is one machine word),
// except 64-bit splats which are a single DUPGV (the scalar size selects 2D
// lanes since D2 reuses the F4 type);
// UMOVV/SMOVV move one lane to a general register (zero-/sign-extend);
// INSV inserts a scalar into one lane (the lowerer copies the vector first);
// TBLV is the single-table shuffle step (out-of-range indices write 0).
// SCVTFV/UCVTFV are exact (round-to-nearest, like x64 CVTDQ2PS with its
// fixup); FCVTZSV/FCVTZUV truncate toward zero but saturate where x64
// CVTTPS2DQ yields INT_MIN, so the lowerer range-checks explicitly and traps
// exactly like x64. FCMGEV/FCMGTV/FCMEQV build the can't-trap lane masks
// those range checks reduce (via UMOVV + CBNZ).
MACRO(DUPELV,      Reg2,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(DUPGV,       Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(UMOVV,       Reg2,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(SMOVV,       Reg2,       0,              UNUSED,   LEGAL_SHIFT,    UNUSED,   D___)
MACRO(INSV,        Reg3,       0,              UNUSED,   LEGAL_INS,      UNUSED,   D___)
MACRO(TBLV,        Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SCVTFV,      Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(UCVTFV,      Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTZSV,     Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCVTZUV,     Reg2,       0,              UNUSED,   LEGAL_REG2,     UNUSED,   D___)
MACRO(FCMGEV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FCMGTV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FCMEQV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)

// W8-NEON slice 2c: float min/max (numeric), vector shifts, saturating
// integer arithmetic. FMINNMV/FMAXNMV are the numeric (NaN-suppressing)
// min/max; the lowerer blends in the original NaN lanes explicitly to match
// the interpreter bit-for-bit. SSHLV/USHLV take a splatted shift-count
// vector (positive = left, negative = right). SQADDV/SQSUBV/UQADDV/UQSUBV
// saturate per lane exactly like the wasm add_sat/sub_sat families. Lane
// size for every one of these comes from the IR type via
// NeonSizeForSimdType, like the slice-1/2a vector ops.
MACRO(FMINNMV,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(FMAXNMV,     Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SSHLV,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(USHLV,       Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SQADDV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(SQSUBV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(UQADDV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)
MACRO(UQSUBV,      Reg3,       0,              UNUSED,   LEGAL_REG3,     UNUSED,   D___)

// Wasm atomics: LDAR (acquire load) / STLR (release store) for
// LowerAtomicLoad/Store. Appended at EOF to preserve existing MD opcode
// values. Operands are (dst, [addr]) / ([addr], src) where addr is a
// register holding the materialized address; the encoder only supports the
// [Xn] form, so the lowerer LEAs the address into a TyMachPtr register.
// OpSideEffect keeps them from being CSE/DCE'd or reordered; DL__/DS__
// make EncoderMD classify them as loads/stores.
MACRO(LDAR,        Reg2,       OpSideEffect,   UNUSED,   LEGAL_LOAD,     UNUSED,   DL__)
MACRO(STLR,        Reg2,       OpSideEffect,   UNUSED,   LEGAL_STORE,    UNUSED,   DS__)
