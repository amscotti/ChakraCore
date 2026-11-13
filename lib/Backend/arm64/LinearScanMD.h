//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once
class OpHelperBlock;
class LinearScan;
class BailOutRecord;
class BranchBailOutRecord;

class LinearScanMD : public LinearScanMDShared
{
private:
    StackSym ** helperSpillSlots;
    Func      * func;
    uint32      maxOpHelperSpilledLiveranges;
    StackSym   *vfpSymTable[VFP_REGCOUNT];
    // W8-NEON slice 1: 16-byte spill slots for Q regs (typed by operand
    // IsSimd128, same register file as D; see Reg.h). Beside vfpSymTable so
    // a float64 value spilled from D0 and a 16-byte value spilled from Q0 do
    // not share a slot width.
    StackSym   *simdSymTable[SIMD_REGCOUNT];

public:
    LinearScanMD(Func *func);

    void        Init(LinearScan *linearScan);

    BitVector   FilterRegIntSizeConstraints(BitVector regsBv, BitVector sizeUsageBv) const;
    bool        FitRegIntSizeConstraints(RegNum reg, BitVector sizeUsageBv) const;
    bool        FitRegIntSizeConstraints(RegNum reg, IRType type) const;
    void        InsertOpHelperSpillAndRestores(SList<OpHelperBlock> *opHelperBlockList);
    void        EndOfHelperBlock(uint32 helperSpilledLiveranges);

    uint        UnAllocatableRegCount(Func *func) const
    { 
        // number of regs marked RA_DONTALLOCATE, including:
        //   ALT_LOCALS_PTR when not the SP
        uint result = 6;
        if (func->GetLocalsPointer() != RegSP)
        {
            result += 1;
        }
        return result;
    }

    StackSym   *EnsureSpillSymForVFPReg(RegNum reg, Func *func);
    // 16-byte spill for a SIMD value in a Q register. `type` is the
    // operand IRType (IsSimd128); the slot is 16-byte typed.
    StackSym   *EnsureSpillSymForSimdReg(RegNum reg, Func *func, IRType type);

    void        LegalizeDef(IR::Instr * instr);
    void        LegalizeUse(IR::Instr * instr, IR::Opnd * opnd);
    void        LegalizeConstantUse(IR::Instr * instr, IR::Opnd * opnd) { /* no op for arm */ }

    void        GenerateBailOut(IR::Instr * instr, __in_ecount(registerSaveSymsCount) StackSym ** registerSaveSyms, uint registerSaveSymsCount);
private:
    static void SaveAllRegisters(BailOutRecord *const bailOutRecord);
public:
    static void SaveAllRegistersAndBailOut(BailOutRecord *const bailOutRecord);
    static void SaveAllRegistersAndBranchBailOut(BranchBailOutRecord *const bailOutRecord, const BOOL condition);
    static RegNum GetParamReg(IR::SymOpnd *symOpnd, Func *func);

    bool        IsAllocatable(RegNum reg, Func *func) const;
    static uint GetRegisterSaveSlotCount() {
        return RegisterSaveSlotCount ;
    }
    static uint GetRegisterSaveIndex(RegNum reg);
    static RegNum GetRegisterFromSaveIndex(uint offset);

    static const uint RegisterSaveSlotCount = RegNumCount + VFP_REGCOUNT;

private:
    bool        LegalizeOffset(IR::Instr * instr, IR::Opnd * opnd);
    bool        LegalizeLEA(IR::Instr * instr, IR::SymOpnd * opnd);
    void        InsertOpHelperSpillsAndRestores(const OpHelperBlock& opHelperBlock);
};
