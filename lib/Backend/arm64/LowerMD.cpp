//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "Backend.h"
#include "Arm64StackLayout.h"
#include "Language/JavascriptFunctionArgIndex.h"

const Js::OpCode LowererMD::MDUncondBranchOpcode = Js::OpCode::B;
const Js::OpCode LowererMD::MDMultiBranchOpcode = Js::OpCode::BR;
const Js::OpCode LowererMD::MDTestOpcode = Js::OpCode::TST;
const Js::OpCode LowererMD::MDOrOpcode = Js::OpCode::ORR;
const Js::OpCode LowererMD::MDXorOpcode = Js::OpCode::EOR;
const Js::OpCode LowererMD::MDOverflowBranchOpcode = Js::OpCode::BVS;
const Js::OpCode LowererMD::MDNotOverflowBranchOpcode = Js::OpCode::BVC;
const Js::OpCode LowererMD::MDConvertFloat32ToFloat64Opcode = Js::OpCode::FCVT;
const Js::OpCode LowererMD::MDConvertFloat64ToFloat32Opcode = Js::OpCode::FCVT;
const Js::OpCode LowererMD::MDCallOpcode = Js::OpCode::Call;
const Js::OpCode LowererMD::MDImulOpcode = Js::OpCode::MUL;
const Js::OpCode LowererMD::MDLea = Js::OpCode::LEA;
const Js::OpCode LowererMD::MDSpecBlockNEOpcode = Js::OpCode::CSELNE;
const Js::OpCode LowererMD::MDSpecBlockFNEOpcode = Js::OpCode::FCSELNE;

template<typename T>
inline void Swap(T& x, T& y)
{
    T temp = x;
    x = y;
    y = temp;
}

// Static utility fn()
//
bool
LowererMD::IsAssign(const IR::Instr *instr)
{
    return (instr->m_opcode == Js::OpCode::MOV ||
            instr->m_opcode == Js::OpCode::FMOV ||
            instr->m_opcode == Js::OpCode::MOVQ ||
            instr->m_opcode == Js::OpCode::LDIMM ||
            instr->m_opcode == Js::OpCode::LDR ||
            instr->m_opcode == Js::OpCode::LDRS ||
            instr->m_opcode == Js::OpCode::FLDR ||
            instr->m_opcode == Js::OpCode::STR ||
            instr->m_opcode == Js::OpCode::FSTR);
}

///----------------------------------------------------------------------------
///
/// LowererMD::IsCall
///
///----------------------------------------------------------------------------

bool
LowererMD::IsCall(const IR::Instr *instr)
{
    return (instr->m_opcode == Js::OpCode::BL ||
            instr->m_opcode == Js::OpCode::BLR);
}

///----------------------------------------------------------------------------
///
/// LowererMD::IsIndirectBranch
///
///----------------------------------------------------------------------------

bool
LowererMD::IsIndirectBranch(const IR::Instr *instr)
{
    return (instr->m_opcode == Js::OpCode::BR);
}


///----------------------------------------------------------------------------
///
/// LowererMD::IsUnconditionalBranch
///
///----------------------------------------------------------------------------

bool
LowererMD::IsUnconditionalBranch(const IR::Instr *instr)
{
    return (instr->m_opcode == Js::OpCode::B || 
            instr->m_opcode == Js::OpCode::BR);
}

bool
LowererMD::IsReturnInstr(const IR::Instr *instr)
{
    return instr->m_opcode == Js::OpCode::RET;
}

///----------------------------------------------------------------------------
///
/// LowererMD::InvertBranch
///
///----------------------------------------------------------------------------

void
LowererMD::InvertBranch(IR::BranchInstr *branchInstr)
{
    switch (branchInstr->m_opcode)
    {
    case Js::OpCode::BEQ:
        branchInstr->m_opcode = Js::OpCode::BNE;
        break;
    case Js::OpCode::BNE:
        branchInstr->m_opcode = Js::OpCode::BEQ;
        break;
    case Js::OpCode::BGE:
        branchInstr->m_opcode = Js::OpCode::BLT;
        break;
    case Js::OpCode::BGT:
        branchInstr->m_opcode = Js::OpCode::BLE;
        break;
    case Js::OpCode::BLT:
        branchInstr->m_opcode = Js::OpCode::BGE;
        break;
    case Js::OpCode::BLE:
        branchInstr->m_opcode = Js::OpCode::BGT;
        break;
    case Js::OpCode::BCS:
        branchInstr->m_opcode = Js::OpCode::BCC;
        break;
    case Js::OpCode::BCC:
        branchInstr->m_opcode = Js::OpCode::BCS;
        break;
    case Js::OpCode::BMI:
        branchInstr->m_opcode = Js::OpCode::BPL;
        break;
    case Js::OpCode::BPL:
        branchInstr->m_opcode = Js::OpCode::BMI;
        break;
    case Js::OpCode::BVS:
        branchInstr->m_opcode = Js::OpCode::BVC;
        break;
    case Js::OpCode::BVC:
        branchInstr->m_opcode = Js::OpCode::BVS;
        break;
    case Js::OpCode::BLS:
        branchInstr->m_opcode = Js::OpCode::BHI;
        break;
    case Js::OpCode::BHI:
        branchInstr->m_opcode = Js::OpCode::BLS;
        break;
    case Js::OpCode::CBZ:
        branchInstr->m_opcode = Js::OpCode::CBNZ;
        break;
    case Js::OpCode::CBNZ:
        branchInstr->m_opcode = Js::OpCode::CBZ;
        break;
    case Js::OpCode::TBZ:
        branchInstr->m_opcode = Js::OpCode::TBNZ;
        break;
    case Js::OpCode::TBNZ:
        branchInstr->m_opcode = Js::OpCode::TBZ;
        break;

    default:
        AssertMsg(UNREACHED, "B missing in InvertBranch()");
    }

}

Js::OpCode
LowererMD::MDConvertFloat64ToInt32Opcode(const RoundMode roundMode)
{
    switch (roundMode)
    {
    case RoundModeTowardZero:
        return Js::OpCode::FCVTZ;
    case RoundModeTowardInteger:
        return Js::OpCode::Nop;
    case RoundModeHalfToEven:
        return Js::OpCode::FCVTN;
    default:
        AssertMsg(0, "RoundMode has no MD mapping.");
        return Js::OpCode::Nop;
    }
}

// GenerateMemRef: Return an opnd that can be used to access the given address.
// ARM can't encode direct accesses to physical addresses, so put the address in a register
// and return an indir. (This facilitates re-use of the loaded address without having to re-load it.)
IR::Opnd *
LowererMD::GenerateMemRef(intptr_t addr, IRType type, IR::Instr *instr, bool dontEncode)
{
    IR::RegOpnd *baseOpnd = IR::RegOpnd::New(TyMachReg, this->m_func);
    IR::AddrOpnd *addrOpnd = IR::AddrOpnd::New(addr, IR::AddrOpndKindDynamicMisc, this->m_func, dontEncode);
    Lowerer::InsertMove(baseOpnd, addrOpnd, instr);

    return IR::IndirOpnd::New(baseOpnd, 0, type, this->m_func);
}

void
LowererMD::FlipHelperCallArgsOrder()
{
    int left  = 0;
    int right = helperCallArgsCount - 1;
    while (left < right)
    {
        IR::Opnd *tempOpnd = helperCallArgs[left];
        helperCallArgs[left] = helperCallArgs[right];
        helperCallArgs[right] = tempOpnd;

        left++;
        right--;
    }
}

IR::Instr *
LowererMD::LowerCallHelper(IR::Instr *instrCall)
{
    IR::Opnd *argOpnd = instrCall->UnlinkSrc2();
    IR::Instr          *prevInstr = instrCall;
    IR::JnHelperMethod helperMethod = instrCall->GetSrc1()->AsHelperCallOpnd()->m_fnHelper;
    instrCall->FreeSrc1();

    while (argOpnd)
    {
        Assert(argOpnd->IsRegOpnd());
        IR::RegOpnd *regArg = argOpnd->AsRegOpnd();

        Assert(regArg->m_sym->m_isSingleDef);
        IR::Instr *instrArg = regArg->m_sym->m_instrDef;

        Assert(instrArg->m_opcode == Js::OpCode::ArgOut_A || instrArg->m_opcode == Js::OpCode::ExtendArg_A &&
        (
            helperMethod == IR::JnHelperMethod::HelperOP_InitCachedScope ||
            helperMethod == IR::JnHelperMethod::HelperScrFunc_OP_NewScFuncHomeObj ||
            helperMethod == IR::JnHelperMethod::HelperScrFunc_OP_NewScGenFuncHomeObj ||
            helperMethod == IR::JnHelperMethod::HelperRestify ||
            helperMethod == IR::JnHelperMethod::HelperStPropIdArrFromVar
        ));
        prevInstr = this->LoadHelperArgument(prevInstr, instrArg->GetSrc1());

        argOpnd = instrArg->GetSrc2();

        if (instrArg->m_opcode == Js::OpCode::ArgOut_A)
        {
            instrArg->UnlinkSrc1();
            if (argOpnd)
            {
                instrArg->UnlinkSrc2();
            }
            regArg->Free(this->m_func);
            instrArg->Remove();
        }
        else if (instrArg->m_opcode == Js::OpCode::ExtendArg_A)
        {
            if (instrArg->GetSrc1()->IsRegOpnd())
            {
                m_lowerer->addToLiveOnBackEdgeSyms->Set(instrArg->GetSrc1()->AsRegOpnd()->GetStackSym()->m_id);
            }
        }
    }

    switch (helperMethod)
    {
    case IR::JnHelperMethod::HelperScrFunc_OP_NewScFuncHomeObj:
    case IR::JnHelperMethod::HelperScrFunc_OP_NewScGenFuncHomeObj:
        break;
    default:
        prevInstr = m_lowerer->LoadScriptContext(prevInstr);
        break;
    }
    this->FlipHelperCallArgsOrder();
    return this->ChangeToHelperCall(instrCall, helperMethod);
}

// Lower a call: May be either helper or native JS call. Just set the opcode, and
// put the result into the return register. (No stack adjustment required.)
IR::Instr *
LowererMD::LowerCall(IR::Instr * callInstr, Js::ArgSlot argCount)
{
    IR::Instr *retInstr = callInstr;
    IR::Opnd *targetOpnd = callInstr->GetSrc1();
    AssertMsg(targetOpnd, "Call without a target?");

    // This is required here due to calls created during lowering
    callInstr->m_func->SetHasCallsOnSelfAndParents();

    if (targetOpnd->IsRegOpnd())
    {
        // Indirect call
        callInstr->m_opcode = Js::OpCode::BLR;
    }
    else
    {
        AssertMsg(targetOpnd->IsHelperCallOpnd(), "Why haven't we loaded the call target?");
        // Direct call
        //
        // load the address into a register because we cannot directly access more than 24 bit constants
        // in BL instruction. Non helper call methods will already be accessed indirectly.
        //
        // Skip this for bailout calls. The register allocator will lower that as appropriate, without affecting spill choices.

        if (!callInstr->HasBailOutInfo())
        {
            IR::RegOpnd     *regOpnd = IR::RegOpnd::New(nullptr, RegLR, TyMachPtr, this->m_func);
            IR::Instr       *movInstr = IR::Instr::New(Js::OpCode::LDIMM, regOpnd, callInstr->UnlinkSrc1(), this->m_func);
            regOpnd->m_isCallArg = true;
            callInstr->SetSrc1(regOpnd);
            callInstr->InsertBefore(movInstr);
        }
        callInstr->m_opcode = Js::OpCode::BLR;
    }

    IR::Opnd *dstOpnd = callInstr->GetDst();
    if (dstOpnd)
    {
        Js::OpCode assignOp;
        RegNum returnReg;

        if (IRType_IsSimd128(dstOpnd->GetType()))
        {
            // W8-NEON slice 1: 128-bit returns in Q0 via 16-byte move.
            assignOp = GetAssignOp(dstOpnd->GetType());
            returnReg = (RegNum)RegQ0;
        }
        else if(dstOpnd->IsFloat64() || dstOpnd->IsFloat32())
        {
            // f32 returns in s0 (low half of d0, ABI section 4); f64 in d0.
            assignOp = Js::OpCode::FMOV;
            returnReg = RETURN_DBL_REG;
        }
        else
        {
            assignOp = Js::OpCode::MOV;
            returnReg = RETURN_REG;

            if (callInstr->GetSrc1()->IsHelperCallOpnd())
            {
                // Truncate the result of a conversion to 32-bit int, because the C++ code doesn't.
                IR::HelperCallOpnd *helperOpnd = callInstr->GetSrc1()->AsHelperCallOpnd();
                if (helperOpnd->m_fnHelper == IR::HelperConv_ToInt32 ||
                    helperOpnd->m_fnHelper == IR::HelperConv_ToInt32_Full ||
                    helperOpnd->m_fnHelper == IR::HelperConv_ToInt32Core ||
                    helperOpnd->m_fnHelper == IR::HelperConv_ToUInt32 ||
                    helperOpnd->m_fnHelper == IR::HelperConv_ToUInt32_Full ||
                    helperOpnd->m_fnHelper == IR::HelperConv_ToUInt32Core)
                {
                    assignOp = Js::OpCode::MOV_TRUNC;
                }
            }
        }

        IR::Instr * movInstr = callInstr->SinkDst(assignOp);

        callInstr->GetDst()->AsRegOpnd()->SetReg(returnReg);
        movInstr->GetSrc1()->AsRegOpnd()->SetReg(returnReg);

        retInstr = movInstr;

        Legalize(retInstr);
    }

    //
    // assign the arguments to appropriate positions
    //

    AssertMsg(this->helperCallArgsCount >= 0, "Fatal. helper call arguments ought to be positive");
    AssertMsg(this->helperCallArgsCount <= MaxArgumentsToHelper, "Too many helper call arguments");

    uint16 argsLeft = this->helperCallArgsCount;
    uint16 doubleArgsLeft = this->helperCallDoubleArgsCount;
    uint16 intArgsLeft = argsLeft - doubleArgsLeft;

    while(argsLeft > 0)
    {
        IR::Opnd *helperArgOpnd = this->helperCallArgs[this->helperCallArgsCount - argsLeft];
        IR::Opnd * opndParam = nullptr;

        if (helperArgOpnd->IsFloat() || IRType_IsSimd128(helperArgOpnd->GetType()))
        {
            opndParam = this->GetOpndForArgSlot(doubleArgsLeft - 1, helperArgOpnd);
            AssertMsg(opndParam->IsRegOpnd(), "NYI for other kind of operands");
            --doubleArgsLeft;
        }
        else
        {
            opndParam = this->GetOpndForArgSlot(intArgsLeft - 1, helperArgOpnd);
            --intArgsLeft;
        }
        Lowerer::InsertMove(opndParam, helperArgOpnd, callInstr);
        --argsLeft;
    }
    Assert(doubleArgsLeft == 0 && intArgsLeft == 0 && argsLeft == 0);

#if defined(__APPLE__)
    // Darwin passes variadic arguments on the stack. C++ JavascriptMethod
    // thunks and the interpreter thunk read [function, callInfo, arg0, ...]
    // at [sp + slot*8]. Copy the register arguments there too. Stack arguments
    // already occupy [sp + slot*8], past the 8 register slots.
    if (argCount > 0)
    {
        this->HomeJsCallArgs(callInstr, argCount);
    }
#endif

    // We're done with the args (if any) now, so clear the param location state.
    this->FinishArgLowering();

    return retInstr;
}

IR::Instr *
LowererMD::LoadDynamicArgument(IR::Instr *instr, uint argNumber)
{
    Assert(instr->m_opcode == Js::OpCode::ArgOut_A_Dynamic);
    Assert(instr->GetSrc2() == nullptr);

    // On Apple every JS slot, including x0-x7, lives in the argv image at
    // [sp + slot*8]. A move into x3-x7 here is not kept live until the blr:
    // callInfo setup reuses those registers. Store the value and reload the
    // registers immediately before the call. A slot past the eighth is already
    // that image (user arg 5 is slot 8). Other targets use [sp + (slot-8)*8].
#if defined(__APPLE__)
    Js::ArgSlot slot = (Js::ArgSlot)(argNumber - 1);
    if (slot < NUM_INT_ARG_REGS)
    {
        IR::RegOpnd * spOpnd = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
        IR::IndirOpnd * mem = IR::IndirOpnd::New(spOpnd, (int32)(slot * MachRegInt), TyMachReg, this->m_func);
        instr->SetDst(mem);
        instr->m_opcode = Js::OpCode::MOV;
        LegalizeMD::LegalizeInstr(instr);
        if (this->m_func->m_argSlotsForFunctionsCalled < NUM_INT_ARG_REGS)
        {
            this->m_func->m_argSlotsForFunctionsCalled = NUM_INT_ARG_REGS;
        }
        return instr;
    }
    IR::Opnd* dst = GetOpndForArgSlot(slot, nullptr, true);
#else
    IR::Opnd* dst = GetOpndForArgSlot((Js::ArgSlot) (argNumber - 1));
#endif
    instr->SetDst(dst);
    instr->m_opcode = Js::OpCode::MOV;
    LegalizeMD::LegalizeInstr(instr);

    return instr;
}

IR::Instr *
LowererMD::LoadDynamicArgumentUsingLength(IR::Instr *instr)
{
    Assert(instr->m_opcode == Js::OpCode::ArgOut_A_Dynamic);
    IR::RegOpnd* src2 = instr->UnlinkSrc2()->AsRegOpnd();

    // src2 is the user-argument index. apply(arguments) only reaches here
    // for index >= 5. On Apple that is argv slot index+3, past the 8 homes.
    // Spread also uses this loop for earlier indices; those land in the homes.
#if defined(__APPLE__)
    // Full argv image: user arg N is slot N+3, so arg 5 is [sp+64].
    IR::Instr *adjust = IR::Instr::New(Js::OpCode::ADD, IR::RegOpnd::New(src2->GetType(), this->m_func), src2, IR::IntConstOpnd::New(3, TyInt8, this->m_func), this->m_func);
#else
    // Windows ARM64 puts the first stack user argument at [sp].
    IR::Instr *adjust = IR::Instr::New(Js::OpCode::SUB, IR::RegOpnd::New(src2->GetType(), this->m_func), src2, IR::IntConstOpnd::New(INT_ARG_REG_COUNT - 3, TyInt8, this->m_func), this->m_func);
#endif
    instr->InsertBefore(adjust);
    LegalizeMD::LegalizeInstr(adjust);
    IR::RegOpnd *stackPointer   = IR::RegOpnd::New(nullptr, GetRegStackPointer(), TyMachReg, this->m_func);
    IR::IndirOpnd *actualsLocation = IR::IndirOpnd::New(stackPointer, adjust->GetDst()->AsRegOpnd(), GetDefaultIndirScale(), TyMachReg, this->m_func);
    instr->SetDst(actualsLocation);
    instr->m_opcode = Js::OpCode::LDR;
    LegalizeMD::LegalizeInstr(instr);

    return instr;
}

void
LowererMD::SetMaxArgSlots(Js::ArgSlot actualCount /*including this*/)
{
    Js::ArgSlot offset = 3;//For function object & callInfo & this
    if (this->m_func->m_argSlotsForFunctionsCalled < (uint32) (actualCount + offset))
    {
        this->m_func->m_argSlotsForFunctionsCalled = (uint32)(actualCount + offset);
    }
    return;
}

void
LowererMD::GenerateMemInit(IR::RegOpnd * opnd, int32 offset, size_t value, IR::Instr * insertBeforeInstr, bool isZeroed)
{
    m_lowerer->GenerateMemInit(opnd, offset, (uint32)value, insertBeforeInstr, isZeroed);
}

IR::Instr *
LowererMD::LowerCallIDynamic(IR::Instr *callInstr, IR::Instr*saveThisArgOutInstr, IR::Opnd *argsLength, ushort callFlags, IR::Instr * insertBeforeInstrForCFG)
{
    callInstr->InsertBefore(saveThisArgOutInstr); //Move this Argout next to call;
    this->LoadDynamicArgument(saveThisArgOutInstr, 3); //this pointer is the 3rd argument

    //callInfo
    if (callInstr->m_func->IsInlinee())
    {
        Assert(argsLength->AsIntConstOpnd()->GetValue() == callInstr->m_func->actualCount);
        this->SetMaxArgSlots((Js::ArgSlot)callInstr->m_func->actualCount);
    }
    else
    {
        callInstr->InsertBefore(IR::Instr::New(Js::OpCode::ADD, argsLength, argsLength, IR::IntConstOpnd::New(1, TyInt8, this->m_func), this->m_func));
        this->SetMaxArgSlots(Js::InlineeCallInfo::MaxInlineeArgoutCount);
    }
    Lowerer::InsertMove( this->GetOpndForArgSlot(1), argsLength, callInstr);

    IR::RegOpnd    *funcObjOpnd = callInstr->UnlinkSrc1()->AsRegOpnd();
    GeneratePreCall(callInstr, funcObjOpnd, insertBeforeInstrForCFG);

    // functionOpnd is the first argument.
    IR::Opnd * opndParam = this->GetOpndForArgSlot(0);
    Lowerer::InsertMove(opndParam, funcObjOpnd, callInstr);
    IR::Instr * lowered = this->LowerCall(callInstr, 0);
#if defined(__APPLE__)
    // x0 and x1 are set above, then the entry-point load can reuse x2-x7.
    // Slots 2+ were stored by LoadDynamicArgument. Put function and callInfo
    // in the image too, then refill x0-x7 from it as the last uses before the blr.
    this->StoreAppleJsArgSlot(0, funcObjOpnd, callInstr);
    this->StoreAppleJsArgSlot(1, argsLength, callInstr);
    this->ReloadAppleJsCallRegs(callInstr);
#endif
    return lowered;
}

void
LowererMD::GenerateFunctionObjectTest(IR::Instr * callInstr, IR::RegOpnd  *functionObjOpnd, bool isHelper, IR::LabelInstr* continueAfterExLabel /* = nullptr */)
{
    AssertMsg(!m_func->IsJitInDebugMode() || continueAfterExLabel, "When jit is in debug mode, continueAfterExLabel must be provided otherwise continue after exception may cause AV.");

    // Need check and error if we are calling a tagged int.
    if (!functionObjOpnd->IsNotTaggedValue())
    {
        IR::LabelInstr * helperLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
        if (this->GenerateObjectTest(functionObjOpnd, callInstr, helperLabel))
        {

            IR::LabelInstr * callLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, isHelper);
            IR::Instr * instr = IR::BranchInstr::New(Js::OpCode::B, callLabel, this->m_func);
            callInstr->InsertBefore(instr);

            callInstr->InsertBefore(helperLabel);
            callInstr->InsertBefore(callLabel);

            this->m_lowerer->GenerateRuntimeError(callLabel, JSERR_NeedFunction);

            if (continueAfterExLabel)
            {
                // Under debugger the RuntimeError (exception) can be ignored, generate branch to jmp to safe place
                // (which would normally be debugger bailout check).
                IR::BranchInstr* continueAfterEx = IR::BranchInstr::New(LowererMD::MDUncondBranchOpcode, continueAfterExLabel, this->m_func);
                callLabel->InsertBefore(continueAfterEx);
            }
        }
    }
}

IR::Instr*
LowererMD::GeneratePreCall(IR::Instr * callInstr, IR::Opnd  *functionObjOpnd, IR::Instr * insertBeforeInstrForCFGCheck)
{
    if (insertBeforeInstrForCFGCheck == nullptr)
    {
        insertBeforeInstrForCFGCheck = callInstr;
    }

    IR::RegOpnd * functionTypeRegOpnd = nullptr;
    IR::IndirOpnd * entryPointIndirOpnd = nullptr;

    // For calls to fixed functions we load the function's type directly from the known (hard-coded) function object address.
    // For other calls, we need to load it from the function object stored in a register operand.
    if (functionObjOpnd->IsAddrOpnd() && functionObjOpnd->AsAddrOpnd()->m_isFunction)
    {
        functionTypeRegOpnd = this->m_lowerer->GenerateFunctionTypeFromFixedFunctionObject(insertBeforeInstrForCFGCheck, functionObjOpnd);
    }
    else if (functionObjOpnd->IsRegOpnd())
    {
        AssertMsg(functionObjOpnd->AsRegOpnd()->m_sym->IsStackSym(), "Expected call target to be stackSym");

        functionTypeRegOpnd = IR::RegOpnd::New(TyMachReg, this->m_func);

        IR::IndirOpnd* functionTypeIndirOpnd = IR::IndirOpnd::New(functionObjOpnd->AsRegOpnd(),
            Js::RecyclableObject::GetOffsetOfType(), TyMachReg, this->m_func);
        Lowerer::InsertMove(functionTypeRegOpnd, functionTypeIndirOpnd, insertBeforeInstrForCFGCheck);
    }
    else
    {
        AnalysisAssertMsg(false, "Unexpected call target operand type.");
    }
    entryPointIndirOpnd = IR::IndirOpnd::New(functionTypeRegOpnd, Js::Type::GetOffsetOfEntryPoint(), TyMachPtr, m_func);

    IR::RegOpnd *entryPointRegOpnd = functionTypeRegOpnd;
    entryPointRegOpnd->m_isCallArg = true;

    IR::Instr * stackParamInsert = Lowerer::InsertMove(entryPointRegOpnd, entryPointIndirOpnd, insertBeforeInstrForCFGCheck);

    // targetAddrOpnd is the address we'll call.
    callInstr->SetSrc1(entryPointRegOpnd);

#if defined(_CONTROL_FLOW_GUARD)
    // verify that the call target is valid (CFG Check)
    if (!PHASE_OFF(Js::CFGInJitPhase, this->m_func))
    {
        this->GenerateCFGCheck(entryPointRegOpnd, insertBeforeInstrForCFGCheck);
    }
#endif

    return stackParamInsert;
}

IR::Instr *
LowererMD::LowerCallI(IR::Instr * callInstr, ushort callFlags, bool isHelper, IR::Instr * insertBeforeInstrForCFG)
{
    // Indirect call using JS calling convention:
    // R0 = callee func object
    // R1 = callinfo
    // R2 = arg0 ("this")
    // R3 = arg1
    // [sp] = arg2
    // etc.

    // First load the target address. Note that we want to wind up with this:
    // ...
    // [sp+4] = arg3
    // [sp] = arg2
    // load target addr from func obj
    // R3 = arg1
    // ...
    // R0 = func obj
    // BLX target addr
    // This way the register containing the target addr interferes with the param regs
    // only, not the regs we use to store params to the stack.

    // We're sinking the stores of stack params so that the call sequence is contiguous.
    // This is required by nested calls, since each call will re-use the same stack slots.
    // But if there is no nesting, stack params can be stored as soon as they're computed.

    IR::Opnd * functionObjOpnd = callInstr->UnlinkSrc1();
    IR::Instr * insertBeforeInstrForCFGCheck = callInstr;

    // If this is a call for new, we already pass the function operand through NewScObject,
    // which checks if the function operand is a real function or not, don't need to add a check again.
    // If this is a call to a fixed function, we've already verified that the target is, indeed, a function.
    if (callInstr->m_opcode != Js::OpCode::CallIFixed && !(callFlags & Js::CallFlags_New))
    {
        Assert(functionObjOpnd->IsRegOpnd());
        IR::LabelInstr* continueAfterExLabel = Lowerer::InsertContinueAfterExceptionLabelForDebugger(m_func, callInstr, isHelper);
        GenerateFunctionObjectTest(callInstr, functionObjOpnd->AsRegOpnd(), isHelper, continueAfterExLabel);
    }
    else if (insertBeforeInstrForCFG != nullptr)
    {
//        RegNum dstReg = insertBeforeInstrForCFG->GetDst()->AsRegOpnd()->GetReg();
//        AssertMsg(dstReg == RegArg2 || dstReg == RegArg3, "NewScObject should insert the first Argument in RegArg2/RegArg3 only based on Spread call or not.");
        insertBeforeInstrForCFGCheck = insertBeforeInstrForCFG;
    }

    IR::Instr * stackParamInsert = GeneratePreCall(callInstr, functionObjOpnd, insertBeforeInstrForCFGCheck);

    // We need to get the calculated CallInfo in SimpleJit because that doesn't include any changes for stack alignment
    IR::IntConstOpnd *callInfo;
    int32 argCount = this->LowerCallArgs(callInstr, stackParamInsert, callFlags, 1, &callInfo);

    // functionObjOpnd is the first argument.
    IR::Opnd * opndParam = this->GetOpndForArgSlot(0);
    Lowerer::InsertMove(opndParam, functionObjOpnd, callInstr);

    IR::Opnd *const finalDst = callInstr->GetDst();

    // Finally, lower the call instruction itself.
    IR::Instr* ret = this->LowerCall(callInstr, (Js::ArgSlot)argCount);

    IR::AutoReuseOpnd autoReuseSavedFunctionObjOpnd;
    if (callInstr->IsJitProfilingInstr())
    {
        Assert(callInstr->m_func->IsSimpleJit());
        Assert(!CONFIG_FLAG(NewSimpleJit));

        if(finalDst &&
            finalDst->IsRegOpnd() &&
            functionObjOpnd->IsRegOpnd() &&
            finalDst->AsRegOpnd()->m_sym == functionObjOpnd->AsRegOpnd()->m_sym)
        {
            // The function object sym is going to be overwritten, so save it in a temp for profiling
            IR::RegOpnd *const savedFunctionObjOpnd = IR::RegOpnd::New(functionObjOpnd->GetType(), callInstr->m_func);
            autoReuseSavedFunctionObjOpnd.Initialize(savedFunctionObjOpnd, callInstr->m_func);
            Lowerer::InsertMove(savedFunctionObjOpnd, functionObjOpnd, callInstr->m_next);
            functionObjOpnd = savedFunctionObjOpnd;
        }

        auto instr = callInstr->AsJitProfilingInstr();
        ret = this->m_lowerer->GenerateCallProfiling(
            instr->profileId,
            instr->inlineCacheIndex,
            instr->GetDst(),
            functionObjOpnd,
            callInfo,
            instr->isProfiledReturnCall,
            callInstr,
            ret);
    }
    return ret;
}

// W4 slice 1 (doc/wasm-arm64-plan.md): asm.js internal/external call lowering.
// Internal (wasm -> wasm/asm.js) calls load the entrypoint through the
// ScriptFunction EntryPointInfo jsMethod indirection (mirrors the x64
// GeneratePreCall AsmJsCallI branch) and pass unboxed args in the wasm
// slot positions (doc/wasm-arm64-abi.md sections 2-3: internal convention),
// like x64. External (wasm -> JS import) calls load the Type entrypoint and
// pass Var args (boxing already done by IRBuilder ToVar conversions).
IR::Instr *
LowererMD::GeneratePreCallAsmJs(IR::Instr * callInstr, IR::Opnd * functionObjOpnd, IR::Instr * insertBeforeInstrForCFGCheck, IR::RegOpnd ** entryPointOpndOut)
{
#if !defined(ENABLE_WASM)
    // Asm.js entrypoint resolution (JnHelper AsmJsGetEntryPoint) only
    // exists in wasm builds; unreachable otherwise.
    (void)callInstr; (void)functionObjOpnd; (void)insertBeforeInstrForCFGCheck; (void)entryPointOpndOut;
    Assert(UNREACHED);
    return nullptr;
#else
    if (insertBeforeInstrForCFGCheck == nullptr)
    {
        insertBeforeInstrForCFGCheck = callInstr;
    }
    Assert(functionObjOpnd->IsRegOpnd());
    // Resolve the internal-call entrypoint (EntryPointInfo::jsMethod) through
    // the JnHelper machinery rather than a hand-written field chain: the
    // three-load chain is not lowered reliably on this backend (a torn load
    // walks to a bogus small address and AsmJsJitCall jumps to 0x1b90/0x1ba0),
    // while FunctionEntryPointInfo::GetAddress() itself uses this exact
    // indirection. AsmJsGetEntryPoint does no allocation and cannot reenter.
    IR::RegOpnd * entryPointOpnd = IR::RegOpnd::New(TyMachPtr, m_func);
    entryPointOpnd->m_isCallArg = true;
    IR::Instr * helperCall = IR::Instr::New(Js::OpCode::Call, entryPointOpnd, m_func);
    insertBeforeInstrForCFGCheck->InsertBefore(helperCall);
    LoadHelperArgument(helperCall, functionObjOpnd);
    IR::Instr * call = ChangeToHelperCall(helperCall, IR::HelperOp_GetAsmJsEntryPoint);
    // (ChangeToHelperCall's LowerCall forces entryPointOpnd's result into x0.)
    // The park/restore moves are emitted by the caller AFTER LowerCallArgs:
    // arg materialization may need scratch regs, and a restore emitted here
    // would be a dead def (no IR consumer reads x0) that the allocator may
    // clobber before the branch.
    *entryPointOpndOut = entryPointOpnd;
    return helperCall;
#endif
}

IR::Instr *
LowererMD::LowerAsmJsCallI(IR::Instr * callInstr)
{
    IR::Opnd * functionObjOpnd = callInstr->UnlinkSrc1();
    IR::RegOpnd * entryPointOpnd = nullptr;
    IR::Instr * stackParamInsert = GeneratePreCallAsmJs(callInstr, functionObjOpnd, nullptr, &entryPointOpnd);
    // W4 slice 1: wasm params occupy arg slots 3+k in the callee (the codegen
    // param layout: slot 0 = function, 1,2 = callinfo/env, 3+k = wasm args;
    // see IRBuilderAsmJs ParamSlotSym(k+1) + formal-param offset). The other
    // helpers/agreement with the external bridge (which presents [func][gap]
    // [gap][args] in the slot image) agree on slots 3+k, so extraParams = 2.
    int32 argCount = this->LowerCallArgs(callInstr, stackParamInsert, Js::CallFlags_Value, 2);
    // The internal call must present the callee function object in x0
    // (slot 0): the callee reads [fp+0x10] as its function object for its own
    // module lookups, and a JIT->JIT caller must establish it (the blr
    // target would otherwise occupy x0, breaking the second level of nested
    // internal calls). Park the entrypoint in fixed x16 and restore the
    // function object into fixed x0 AFTER arg lowering (mirrors CallE order):
    // arg materialization above may use x0 as scratch, and an earlier restore
    // is a dead def the allocator can clobber (Octane zlib SIGSEGV).
    IR::RegOpnd * x16Opnd = IR::RegOpnd::New(nullptr, RegR16, TyMachPtr, m_func);
    IR::RegOpnd * x0Opnd = IR::RegOpnd::New(nullptr, RegR0, TyMachPtr, m_func);
    Lowerer::InsertMove(x16Opnd, entryPointOpnd, callInstr);
    Lowerer::InsertMove(x0Opnd, functionObjOpnd, callInstr);
    callInstr->SetSrc1(x16Opnd);
    IR::Instr * ret = this->LowerCall(callInstr, (Js::ArgSlot)argCount);
    return ret;
}

IR::Instr *
LowererMD::LowerAsmJsCallE(IR::Instr * callInstr)
{
    IR::Opnd * functionObjOpnd = callInstr->UnlinkSrc1();
    Assert(functionObjOpnd->IsRegOpnd());
    IR::Instr * stackParamInsert = GeneratePreCall(callInstr, functionObjOpnd, nullptr);
    IR::IntConstOpnd *callInfo = nullptr;
    int32 argCount = this->LowerCallArgs(callInstr, stackParamInsert, Js::CallFlags_Value, 1, &callInfo);
    IR::Opnd * opndParam = this->GetOpndForArgSlot(0);
    Lowerer::InsertMove(opndParam, functionObjOpnd, callInstr);
    IR::Instr * ret = this->LowerCall(callInstr, (Js::ArgSlot)argCount);
    return ret;
}

IR::Instr *
LowererMD::LoadFloatHelperArgument(IR::Instr * instrInsert, IR::Opnd * opndArg)
{
    Assert(opndArg->IsFloat32());
    return LoadHelperArgument(instrInsert, opndArg);
}

void
LowererMD::HelperCallForAsmMathBuiltin(IR::Instr* instr, IR::JnHelperMethod helperMethodFloat, IR::JnHelperMethod helperMethodDouble)
{
    Assert(instr->m_opcode == Js::OpCode::InlineMathFloor || instr->m_opcode == Js::OpCode::InlineMathCeil || instr->m_opcode == Js::OpCode::Trunc_A || instr->m_opcode == Js::OpCode::Nearest_A);
    Assert(instr->GetDst()->IsFloat());
    Assert(instr->GetDst()->GetType() == instr->GetSrc1()->GetType());
    Assert(!instr->GetSrc2());
    IR::Opnd * argOpnd = instr->UnlinkSrc1();
    IR::JnHelperMethod helperMethod;
    if (argOpnd->IsFloat32())
    {
        helperMethod = helperMethodFloat;
        LoadFloatHelperArgument(instr, argOpnd);
    }
    else
    {
        helperMethod = helperMethodDouble;
        LoadDoubleHelperArgument(instr, argOpnd);
    }
    ChangeToHelperCall(instr, helperMethod);
}

IR::Instr *
LowererMD::LowerExitInstrAsmJs(IR::ExitInstr * exitInstr)
{
    // Epilogue is identical to the JS epilogue (return value already in x0/d0 via LowerRet).
    return LowerExitInstr(exitInstr);
}

// W4 slice 2 (doc/wasm-arm64-plan.md): memory ops with explicit bounds
// checks against the generic buffer (D3, doc/wasm-arm64-abi.md section 8).
// Ports the x64 LowerWasmArrayBoundsCheck / LowerAsmJs{Ld,St}ElemHelper
// shapes; traps via GenerateThrow(WASMERR_ArrayIndexOutOfRange), no signals.
IR::Instr *
LowererMD::LowerWasmArrayBoundsCheck(IR::Instr * instr, IR::Opnd *addrOpnd)
{
    IR::IndirOpnd * indirOpnd = addrOpnd->AsIndirOpnd();
    IR::RegOpnd * indexOpnd = indirOpnd->GetIndexOpnd();
    if (indexOpnd && !indexOpnd->IsIntegral32())
    {
        Assert(UNREACHED);
        m_lowerer->GenerateThrow(IR::IntConstOpnd::New(WASMERR_ArrayIndexOutOfRange, TyInt32, m_func), instr);
        return instr;
    }
    if (m_func->GetJITFunctionBody()->UsesWAsmJsFastVirtualBuffer())
    {
        return instr;
    }
    Assert(instr->GetSrc2());
    IR::LabelInstr * helperLabel = Lowerer::InsertLabel(true, instr);
    IR::LabelInstr * loadLabel = Lowerer::InsertLabel(false, instr);
    IR::LabelInstr * doneLabel = Lowerer::InsertLabel(false, instr);
    uint32 offset = indirOpnd->GetOffset();
    IR::Opnd *arrayLenOpnd = instr->GetSrc2();
    IR::Int64ConstOpnd * constOffsetOpnd = IR::Int64ConstOpnd::New((int64)addrOpnd->GetSize() + (int64)offset, TyInt64, m_func);
    IR::Opnd *cmpOpnd;
    if (indexOpnd != nullptr)
    {
        // Wasm addresses are 32-bit unsigned: zero-extend the index to
        // 64-bit before the 64-bit bounds add. A 32-bit add would wrap
        // (0xFFFFFFFC + 4 -> 0) and let negative/wraparound addresses
        // escape the check and fault (test/wasm/memory.js segfault).
        IR::RegOpnd *extIndex = IR::RegOpnd::New(TyInt64, m_func);
        IR::Instr *ubfx = IR::Instr::New(Js::OpCode::UBFX, extIndex,
            indexOpnd->UseWithNewType(TyInt32, m_func),
            IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func, true), m_func);
        helperLabel->InsertBefore(ubfx);
        LegalizeMD::LegalizeInstr(ubfx);
        cmpOpnd = IR::RegOpnd::New(TyInt64, m_func);
        Lowerer::InsertAdd(true, cmpOpnd, extIndex, constOffsetOpnd, helperLabel);
    }
    else
    {
        cmpOpnd = constOffsetOpnd;
    }
    m_lowerer->InsertCompareBranch(cmpOpnd, arrayLenOpnd, Js::OpCode::BrGt_A, true, helperLabel, helperLabel);
    m_lowerer->GenerateThrow(IR::IntConstOpnd::New(WASMERR_ArrayIndexOutOfRange, TyInt32, m_func), loadLabel);
    Lowerer::InsertBranch(Js::OpCode::Br, loadLabel, helperLabel);
    return doneLabel;
}

IR::Instr *
LowererMD::LowerAsmJsLdElemHelper(IR::Instr * instr)
{
    IR::Instr * done;
    IR::Opnd * src1 = instr->UnlinkSrc1();
    IRType type = src1->GetType();
    IR::RegOpnd * indexOpnd = src1->AsIndirOpnd()->GetIndexOpnd();
#if ENABLE_FAST_ARRAYBUFFER
    if (CONFIG_FLAG(WasmFastArray) && m_func->GetJITFunctionBody()->IsWasmFunction())
    {
        return instr;
    }
#endif
    {
        IR::LabelInstr * helperLabel = Lowerer::InsertLabel(true, instr);
        IR::LabelInstr * loadLabel = Lowerer::InsertLabel(false, instr);
        IR::LabelInstr * doneLabel = Lowerer::InsertLabel(false, instr);
        IR::Opnd *cmpOpnd;
        if (indexOpnd)
        {
            cmpOpnd = indexOpnd;
        }
        else
        {
            cmpOpnd = IR::IntConstOpnd::New(src1->AsIndirOpnd()->GetOffset(), TyUint32, m_func);
        }
        m_lowerer->InsertCompareBranch(cmpOpnd, instr->UnlinkSrc2(), Js::OpCode::BrGe_A, true, helperLabel, helperLabel);
        Lowerer::InsertBranch(Js::OpCode::Br, loadLabel, helperLabel);
        if (IRType_IsFloat(type))
        {
            Lowerer::InsertMove(instr->UnlinkDst(), IR::FloatConstOpnd::New(Js::NumberConstants::NaN, type, m_func), loadLabel);
        }
        else
        {
            Lowerer::InsertMove(instr->UnlinkDst(), IR::IntConstOpnd::New(0, TyInt8, m_func), loadLabel);
        }
        Lowerer::InsertBranch(Js::OpCode::Br, doneLabel, loadLabel);
        done = doneLabel;
    }
    return done;
}

IR::Instr *
LowererMD::LowerAsmJsStElemHelper(IR::Instr * instr)
{
    IR::Instr * done;
    IR::Opnd * dst = instr->UnlinkDst();
    IR::RegOpnd * indexOpnd = dst->AsIndirOpnd()->GetIndexOpnd();
    {
        IR::LabelInstr * helperLabel = Lowerer::InsertLabel(true, instr);
        IR::LabelInstr * storeLabel = Lowerer::InsertLabel(false, instr);
        IR::LabelInstr * doneLabel = Lowerer::InsertLabel(false, instr);
        IR::Opnd * cmpOpnd;
        if (indexOpnd)
        {
            cmpOpnd = dst->AsIndirOpnd()->GetIndexOpnd();
        }
        else
        {
            cmpOpnd = IR::IntConstOpnd::New(dst->AsIndirOpnd()->GetOffset(), TyUint32, m_func);
        }
        m_lowerer->InsertCompareBranch(cmpOpnd, instr->UnlinkSrc2(), Js::OpCode::BrGe_A, true, helperLabel, helperLabel);
        Lowerer::InsertBranch(Js::OpCode::Br, storeLabel, helperLabel);
        Lowerer::InsertBranch(Js::OpCode::Br, doneLabel, storeLabel);
        done = doneLabel;
    }
    return done;
}

int32
LowererMD::LowerCallArgs(IR::Instr *callInstr, IR::Instr *stackParamInsert, ushort callFlags, Js::ArgSlot extraParams, IR::IntConstOpnd **callInfoOpndRef)
{
    AssertMsg(this->helperCallArgsCount == 0, "We don't support nested helper calls yet");

    uint32 argCount = 0;

    IR::Opnd* opndParam;
    // Now walk the user arguments and remember the arg count.

    IR::Instr * argInstr = callInstr;
    IR::Opnd *src2Opnd = callInstr->UnlinkSrc2();
    while (src2Opnd->IsSymOpnd())
    {
        // Get the arg instr
        IR::SymOpnd *   argLinkOpnd = src2Opnd->AsSymOpnd();
        StackSym *      argLinkSym  = argLinkOpnd->m_sym->AsStackSym();
        AssertMsg(argLinkSym->IsArgSlotSym() && argLinkSym->m_isSingleDef, "Arg tree not single def...");
        argLinkOpnd->Free(this->m_func);

        argInstr = argLinkSym->m_instrDef;

        // The arg sym isn't assigned a constant directly anymore
        argLinkSym->m_isConst = false;
        argLinkSym->m_isIntConst = false;
        argLinkSym->m_isTaggableIntConst = false;

        // The arg slot nums are 1-based, so subtract 1. Then add 1 for the non-user args (callinfo).
        auto argSlotNum = argLinkSym->GetArgSlotNum();
        if(argSlotNum + extraParams < argSlotNum)
        {
            Js::Throw::OutOfMemory();
        }
        opndParam = this->GetOpndForArgSlot(argSlotNum + extraParams, nullptr, true);

        src2Opnd = argInstr->UnlinkSrc2();
        // W4 slice 1: asm.js float args ride in int slots (callee homes x-regs;
        // docs section 3 internal convention is slot-based, floats as bits).
        // MOV float->int is illegal on ARM64; transfer bits via FMOV_GEN to a
        // same-size int reg first (f64->x, f32->x32). The bitcopy must not
        // itself land in the fixed x0 (GeneratePreCallAsmJs parks the callee
        // function object there): use fixed scratch x9, never an arg slot.
        if (argInstr->GetSrc1()->IsFloat() && !opndParam->IsFloat())
        {
            IRType tempType = argInstr->GetSrc1()->IsFloat32() ? TyInt32 : TyMachReg;
            IR::RegOpnd * tempInt = opndParam->IsRegOpnd()
                ? IR::RegOpnd::New(nullptr, RegR9, tempType, this->m_func)
                : IR::RegOpnd::New(tempType, this->m_func);
            IR::Instr * movGen = IR::Instr::New(Js::OpCode::FMOV_GEN, tempInt, argInstr->GetSrc1(), this->m_func);
            if (opndParam->IsRegOpnd())
            {
                callInstr->InsertBefore(movGen);
            }
            else
            {
                stackParamInsert->InsertBefore(movGen);
            }
            argInstr->SetSrc1(tempInt);
        }
        argInstr->ReplaceDst(opndParam);
        argInstr->Unlink();
        if (opndParam->IsRegOpnd())
        {
            callInstr->InsertBefore(argInstr);
        }
        else
        {
            stackParamInsert->InsertBefore(argInstr);
        }
        this->ChangeToAssign(argInstr);
        argCount++;
    }

    IR::RegOpnd * argLinkOpnd = src2Opnd->AsRegOpnd();
    StackSym *argLinkSym = argLinkOpnd->m_sym->AsStackSym();
    AssertMsg(!argLinkSym->IsArgSlotSym() && argLinkSym->m_isSingleDef, "Arg tree not single def...");

    IR::Instr *startCallInstr = argLinkSym->m_instrDef;

    AssertMsg(startCallInstr->m_opcode == Js::OpCode::StartCall || startCallInstr->m_opcode == Js::OpCode::LoweredStartCall, "Problem with arg chain.");
    AssertMsg(startCallInstr->GetArgOutCount(/*getInterpreterArgOutCount*/ false) == argCount ||
              m_func->GetJITFunctionBody()->IsAsmJsMode(),
        "ArgCount doesn't match StartCall count");

    // Deal with the SC.
    this->LowerStartCall(startCallInstr);

    // W4 slice 1: internal asm.js calls (AsmJsCallI) take no CallInfo slot
    // (mirrors the x64 `m_opcode != AsmJsCallI` skip); external (AsmJsCallE)
    // and JS calls keep it at slot `extraParams`.
    if (callInstr->m_opcode != Js::OpCode::AsmJsCallI)
    {
        // Second argument is the callinfo.
        IR::IntConstOpnd *opndCallInfo = Lowerer::MakeCallInfoConst(callFlags, argCount, m_func);
        if(callInfoOpndRef)
        {
            opndCallInfo->Use(m_func);
            *callInfoOpndRef = opndCallInfo;
        }
        opndParam = this->GetOpndForArgSlot(extraParams);
        Lowerer::InsertMove(opndParam, opndCallInfo, callInstr);
        return argCount + 1 + extraParams; // + 1 for call flags
    }
    // Include the hidden +1 so HomeJsCallArgs homes every wasm arg slot on
    // ARM64 Apple (wasm arg k arrives at slot extraParams+k+1): CallE's
    // +1 covers slots 2+ the same way, so no ABI change either way.
    return argCount + 1 + extraParams; // + 1 to home the last wasm arg slot
}

IR::Instr *
LowererMD::LowerStartCall(IR::Instr * instr)
{
    // StartCall doesn't need to generate a stack adjustment. Just delete it.
    instr->m_opcode = Js::OpCode::LoweredStartCall;
    return instr;
}

IR::Instr *
LowererMD::LoadHelperArgument(IR::Instr * instr, IR::Opnd * opndArgValue)
{
    // Load the given parameter into the appropriate location.
    // We update the current param state so we can do this work without making the caller
    // do the work.
    Assert(this->helperCallArgsCount < LowererMD::MaxArgumentsToHelper);

    __analysis_assume(this->helperCallArgsCount < MaxArgumentsToHelper);

    helperCallArgs[helperCallArgsCount++] = opndArgValue;

    // Helper float args (incl. TyFloat32) ride the double-arg slots: they
    // must count as double args, or LowerCall's helper-homing loop reads
    // a float-typed arg with doubleArgsLeft==0 and underflows GetOpndForArgSlot.
    if (opndArgValue->IsFloat())
    {
        this->helperCallDoubleArgsCount++;
    }

    return instr;
}

void
LowererMD::FinishArgLowering()
{
    this->helperCallArgsCount = 0;
    this->helperCallDoubleArgsCount = 0;
}

#if defined(__APPLE__)
void
LowererMD::StoreAppleJsArgSlot(Js::ArgSlot slot, IR::Opnd * src, IR::Instr * insertBefore)
{
    Assert(slot < NUM_INT_ARG_REGS);
    IR::RegOpnd * spOpnd = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
    IR::IndirOpnd * mem = IR::IndirOpnd::New(spOpnd, (int32)(slot * MachRegInt), TyMachReg, this->m_func);
    Lowerer::InsertMove(mem, src, insertBefore, false);
    if (this->m_func->m_argSlotsForFunctionsCalled < NUM_INT_ARG_REGS)
    {
        this->m_func->m_argSlotsForFunctionsCalled = NUM_INT_ARG_REGS;
    }
}

void
LowererMD::ReloadAppleJsCallRegs(IR::Instr * callInstr)
{
    IR::RegOpnd * spOpnd = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
    for (Js::ArgSlot slot = 0; slot < NUM_INT_ARG_REGS; ++slot)
    {
        IR::IndirOpnd * src = IR::IndirOpnd::New(spOpnd, (int32)(slot * MachRegInt), TyMachReg, this->m_func);
        IR::RegOpnd * dst = IR::RegOpnd::New(nullptr, (RegNum)(FIRST_INT_ARG_REG + slot), TyMachReg, this->m_func);
        dst->m_isCallArg = true;
        IR::Instr * load = IR::Instr::New(Js::OpCode::LDR, dst, src, this->m_func);
        callInstr->InsertBefore(load);
        LegalizeMD::LegalizeInstr(load);
    }
    if (this->m_func->m_argSlotsForFunctionsCalled < NUM_INT_ARG_REGS)
    {
        this->m_func->m_argSlotsForFunctionsCalled = NUM_INT_ARG_REGS;
    }
}
#endif

void
LowererMD::HomeJsCallArgs(IR::Instr * callInstr, Js::ArgSlot argCount)
{
#if defined(__APPLE__)
    Js::ArgSlot homeCount = argCount < NUM_INT_ARG_REGS ? argCount : (Js::ArgSlot)NUM_INT_ARG_REGS;
    IR::RegOpnd * spOpnd = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
    for (Js::ArgSlot slot = 0; slot < homeCount; ++slot)
    {
        IR::RegOpnd * argReg = IR::RegOpnd::New(nullptr, (RegNum)(FIRST_INT_ARG_REG + slot), TyMachReg, this->m_func);
        argReg->m_isCallArg = true;
        IR::IndirOpnd * dst = IR::IndirOpnd::New(spOpnd, (int32)(slot * MachRegInt), TyMachReg, this->m_func);
        IR::Instr * store = IR::Instr::New(Js::OpCode::STR, dst, argReg, this->m_func);
        callInstr->InsertBefore(store);
        LegalizeMD::LegalizeInstr(store);
    }

    uint32 slots = homeCount;
    if ((slots & 1) != 0)
    {
        ++slots;
    }
    if (this->m_func->m_argSlotsForFunctionsCalled < slots)
    {
        this->m_func->m_argSlotsForFunctionsCalled = slots;
    }
#else
    (void)callInstr;
    (void)argCount;
#endif
}

IR::Opnd *
LowererMD::GetOpndForArgSlot(Js::ArgSlot argSlot, IR::Opnd * argOpnd, bool jsStackImage)
{
    IR::Opnd * opndParam = nullptr;

    IRType type = argOpnd ? argOpnd->GetType() : TyMachReg;
    // W8-NEON slice 1: 128-bit args use Q0-7, then 16-byte stack slots.
    if (argOpnd != nullptr && IRType_IsSimd128(type))
    {
        if (argSlot < 8)
        {
            IR::RegOpnd *regOpnd = IR::RegOpnd::New(nullptr, (RegNum)(RegQ0 + argSlot), type, this->m_func);
            regOpnd->m_isCallArg = true;
            opndParam = regOpnd;
        }
        else
        {
            IR::RegOpnd * spBase = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
            IntConstType offset;
#if defined(__APPLE__)
            if (jsStackImage)
            {
                offset = argSlot * MachRegSimd128;
            }
            else
#endif
            {
                offset = (int32)((argSlot - 8) * MachRegSimd128);
            }
            opndParam = IR::IndirOpnd::New(spBase, int32(offset), type, this->m_func);
            uint32 needed = (uint32)((offset + MachRegSimd128) / MachRegInt);
            if (this->m_func->m_argSlotsForFunctionsCalled < needed)
            {
                this->m_func->m_argSlotsForFunctionsCalled = needed;
            }
        }
        return opndParam;
    }
    if (argOpnd == nullptr || !argOpnd->IsFloat())
    {
        if (argSlot < NUM_INT_ARG_REGS)
        {
            // Return an instance of the next arg register.
            IR::RegOpnd *regOpnd;
            regOpnd = IR::RegOpnd::New(nullptr, (RegNum)(argSlot + FIRST_INT_ARG_REG), type, this->m_func);

            regOpnd->m_isCallArg = true;

            opndParam = regOpnd;
        }
        else
        {
            // Create a stack slot reference and bump up the size of this function's outgoing param area,
            // if necessary. Helper calls use the platform ABI: the first stack
            // argument is at [sp]. A JS call on Apple keeps a full argv image
            // at [sp + slot*8], so slot 8 is at [sp + 64].
#if !defined(__APPLE__)
            (void)jsStackImage;
#endif
            IntConstType offset;
#if defined(__APPLE__)
            if (jsStackImage)
            {
                offset = argSlot * MachRegInt;
            }
            else
#endif
            {
                argSlot = argSlot - NUM_INT_ARG_REGS;
                offset = argSlot * MachRegInt;
            }
            IR::RegOpnd * spBase = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);
            opndParam = IR::IndirOpnd::New(spBase, int32(offset), type, this->m_func);

            if (this->m_func->m_argSlotsForFunctionsCalled < (uint32)(argSlot + 1))
            {
                this->m_func->m_argSlotsForFunctionsCalled = argSlot + 1;
            }
        }
    }
    else
    {
        if (argSlot < MaxDoubleArgumentsToHelper)
        {
            // Return an instance of the next arg register.
            IR::RegOpnd *regOpnd;
            regOpnd = IR::RegOpnd::New(nullptr, (RegNum)(argSlot + FIRST_DOUBLE_ARG_REG), type, this->m_func);
            regOpnd->m_isCallArg = true;
            opndParam = regOpnd;
        }
        else
        {
            AssertMsg(false,"More than 8 double parameter passing disallowed");
        }
    }
    return opndParam;
}

IR::Instr *
LowererMD::LoadDoubleHelperArgument(IR::Instr * instr, IR::Opnd * opndArg)
{
    // Load the given parameter into the appropriate location.
    // We update the current param state so we can do this work without making the caller
    // do the work.
    Assert(opndArg->GetType() == TyMachDouble);
    return this->LoadHelperArgument(instr, opndArg);
}

void
LowererMD::GenerateStackProbe(IR::Instr *insertInstr, bool afterProlog)
{
    //
    // Generate a stack overflow check. This can be as simple as a cmp esp, const
    // because this function is guaranteed to be called on its base thread only.
    // If the check fails call ThreadContext::ProbeCurrentStack which will check again and must throw.
    //
    //       LDIMM r17, ThreadContext::scriptStackLimit + frameSize //Load to register first, as this can be more than 12 bit supported in CMP
    //       CMP   sp, r17
    //       BHI   done
    // begin:
    //       LDIMM  r0, frameSize
    //       LDIMM  r1, scriptContext
    //       LDIMM  r2, ThreadContext::ProbeCurrentStack //MUST THROW
    //       BLX    r2                                  //BX r2 if the stackprobe is before prolog
    // done:
    //

    // For thread context with script interrupt enabled:
    //       LDIMM r17, &ThreadContext::scriptStackLimitForCurrentThread
    //       LDR   r17, [r17]
    //       MOV   r15, frameSize
    //       ADDS  r17, r17, r15
    //       BVS   $helper
    //       CMP   sp, r17
    //       BHI   done
    // $helper:
    //       LDIMM  r0, frameSize
    //       LDIMM  r1, scriptContext
    //       LDIMM  r2, ThreadContext::ProbeCurrentStack //MUST THROW
    //       BLX    r2                                  //BX r2 if the stackprobe is before prolog
    // done:
    //

    //m_localStackHeight for ARM contains (m_argSlotsForFunctionsCalled * MachPtr)
    uint32 frameSize = this->m_func->m_localStackHeight + Js::Constants::MinStackJIT;

    IR::RegOpnd *scratchOpnd = IR::RegOpnd::New(nullptr, SCRATCH_REG, TyMachReg, this->m_func);
    IR::LabelInstr *helperLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, afterProlog);
    IR::Instr *instr;
    bool doInterruptProbe = m_func->GetJITFunctionBody()->DoInterruptProbe();

    if (doInterruptProbe || !m_func->GetThreadContextInfo()->IsThreadBound())
    {
        // LDIMM r17, &ThreadContext::scriptStackLimitForCurrentThread
        intptr_t pLimit = m_func->GetThreadContextInfo()->GetThreadStackLimitAddr();
        Lowerer::InsertMove(scratchOpnd, IR::AddrOpnd::New(pLimit, IR::AddrOpndKindDynamicMisc, this->m_func), insertInstr);

        // LDR   r17, [r17, #0]
        Lowerer::InsertMove(scratchOpnd, IR::IndirOpnd::New(scratchOpnd, 0, TyMachReg, this->m_func), insertInstr);

        AssertMsg(!IS_CONST_00000FFF(frameSize), "For small size we can just add frameSize to r17");

        // MOV r15, frameSize
        IR::Opnd* spAllocRegOpnd = IR::RegOpnd::New(nullptr, SP_ALLOC_SCRATCH_REG, TyMachReg, this->m_func);
        Lowerer::InsertMove(spAllocRegOpnd, IR::IntConstOpnd::New(frameSize, TyMachReg, this->m_func), insertInstr);

        // ADDS r17, r17, r15
        instr = IR::Instr::New(Js::OpCode::ADDS, scratchOpnd, scratchOpnd, spAllocRegOpnd, this->m_func);
        insertInstr->InsertBefore(instr);

        // If this add overflows, we have to call the helper.
        instr = IR::BranchInstr::New(Js::OpCode::BVS, helperLabel, this->m_func);
        insertInstr->InsertBefore(instr);
    }
    else
    {
        // MOV r17, frameSize + scriptStackLimit
        uint64 scriptStackLimit = m_func->GetThreadContextInfo()->GetScriptStackLimit();
        IR::Opnd *stackLimitOpnd = IR::IntConstOpnd::New(frameSize + scriptStackLimit, TyMachReg, this->m_func);
        Lowerer::InsertMove(scratchOpnd, stackLimitOpnd, insertInstr);
    }

    IR::LabelInstr *doneLabelInstr = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, false);
    if (!IS_FAULTINJECT_STACK_PROBE_ON) // Do stack check fastpath only if not doing StackProbe fault injection
    {
        // CMP sp, r17
        instr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
        instr->SetSrc1(IR::RegOpnd::New(nullptr, GetRegStackPointer(), TyMachReg, this->m_func));
        instr->SetSrc2(scratchOpnd);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);

        // BHI done
        instr = IR::BranchInstr::New(Js::OpCode::BHI, doneLabelInstr, this->m_func);
        insertInstr->InsertBefore(instr);
    }

    insertInstr->InsertBefore(helperLabel);

    // ToDo (SaAgarwa): Make sure all SP offsets are correct
    // Zero out the pointer to the list of stack nested funcs, since the functions won't be initialized on this path.
    /*
    scratchOpnd = IR::RegOpnd::New(nullptr, RegR0, TyMachReg, m_func);
    IR::RegOpnd *frameReg = IR::RegOpnd::New(nullptr, GetRegFramePointer(), TyMachReg, m_func);
    Lowerer::InsertMove(scratchOpnd, IR::IntConstOpnd::New(0, TyMachReg, m_func), insertInstr);
    IR::Opnd *indirOpnd = IR::IndirOpnd::New(
        frameReg, -(int32)(Js::Constants::StackNestedFuncList * sizeof(Js::Var)), TyMachReg, m_func);
    Lowerer::InsertMove(indirOpnd, scratchOpnd, insertInstr);
    */
    IR::RegOpnd *r0Opnd = IR::RegOpnd::New(nullptr, RegR0, TyMachReg, this->m_func);
    Lowerer::InsertMove(r0Opnd, IR::IntConstOpnd::New(frameSize, TyMachReg, this->m_func, true), insertInstr);

    IR::RegOpnd *r1Opnd = IR::RegOpnd::New(nullptr, RegR1, TyMachReg, this->m_func);
    Lowerer::InsertMove(r1Opnd, this->m_lowerer->LoadScriptContextOpnd(insertInstr), insertInstr);

    IR::RegOpnd *r2Opnd = IR::RegOpnd::New(nullptr, RegR2, TyMachReg, m_func);
    Lowerer::InsertMove(r2Opnd, IR::HelperCallOpnd::New(IR::HelperProbeCurrentStack, this->m_func), insertInstr);

    instr = IR::Instr::New(afterProlog? Js::OpCode::BLR : Js::OpCode::BR, this->m_func);
    instr->SetSrc1(r2Opnd);
    insertInstr->InsertBefore(instr);

    insertInstr->InsertBefore(doneLabelInstr);
}

//
// Emits an inline reserve-floor check after the prolog (FDE-covered, mirroring
// the small-stack probe). The ARM64 wasm entry alloca reserves a fixed region
// below the slot image; unbounded recursion would grow past it and clobber the
// bridge frame (sending the stack walker into a cycle). On breach the helper
// throws StackOverflow while the chain is still intact. A zero floor disables
// the check naturally (sp is always above frameSize). No entry registers are
// clobbered on the fast path; the slow path never returns.
//
void
LowererMD::GenerateWasmReserveFloorCheck(IR::Instr *insertInstr)
{
    intptr_t floorAddr = m_func->GetThreadContextInfo()->GetWasmReserveFloorAddr();
    if (floorAddr == 0)
    {
        // Out-of-process codegen: no floor to check against.
        return;
    }

    // Same margin as the stack probe.
    uint32 frameSize = this->m_func->m_localStackHeight + Js::Constants::MinStackJIT;

    IR::RegOpnd *scratchOpnd = IR::RegOpnd::New(nullptr, SCRATCH_REG, TyMachReg, this->m_func);
    IR::LabelInstr *helperLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, false);
    IR::LabelInstr *doneLabelInstr = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, false);
    IR::Instr *instr;

    // LDIMM r17, floorAddr; LDR r17, [r17]
    Lowerer::InsertMove(scratchOpnd, IR::AddrOpnd::New(floorAddr, IR::AddrOpndKindDynamicMisc, this->m_func), insertInstr);
    Lowerer::InsertMove(scratchOpnd, IR::IndirOpnd::New(scratchOpnd, 0, TyMachReg, this->m_func), insertInstr);

    // MOV r15, frameSize; ADDS r17, r17, r15; BVS helper (overflow: fail safe)
    IR::Opnd* spAllocRegOpnd = IR::RegOpnd::New(nullptr, SP_ALLOC_SCRATCH_REG, TyMachReg, this->m_func);
    Lowerer::InsertMove(spAllocRegOpnd, IR::IntConstOpnd::New(frameSize, TyMachReg, this->m_func), insertInstr);
    instr = IR::Instr::New(Js::OpCode::ADDS, scratchOpnd, scratchOpnd, spAllocRegOpnd, this->m_func);
    insertInstr->InsertBefore(instr);
    instr = IR::BranchInstr::New(Js::OpCode::BVS, helperLabel, this->m_func);
    insertInstr->InsertBefore(instr);

    // CMP sp, r17; BHI done
    instr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
    instr->SetSrc1(IR::RegOpnd::New(nullptr, GetRegStackPointer(), TyMachReg, this->m_func));
    instr->SetSrc2(scratchOpnd);
    insertInstr->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
    instr = IR::BranchInstr::New(Js::OpCode::BHI, doneLabelInstr, this->m_func);
    insertInstr->InsertBefore(instr);

    // Slow path: helper re-verifies and throws StackOverflow (noreturn).
    insertInstr->InsertBefore(helperLabel);
    IR::RegOpnd *r0Opnd = IR::RegOpnd::New(nullptr, RegR0, TyMachReg, this->m_func);
    Lowerer::InsertMove(r0Opnd, IR::IntConstOpnd::New(frameSize, TyMachReg, this->m_func, true), insertInstr);
    IR::RegOpnd *r1Opnd = IR::RegOpnd::New(nullptr, RegR1, TyMachReg, this->m_func);
    Lowerer::InsertMove(r1Opnd, this->m_lowerer->LoadScriptContextOpnd(insertInstr), insertInstr);
    IR::RegOpnd *r2Opnd = IR::RegOpnd::New(nullptr, RegR2, TyMachReg, m_func);
    Lowerer::InsertMove(r2Opnd, IR::HelperCallOpnd::New(IR::HelperProbeWasmReserveFloor, this->m_func), insertInstr);
    instr = IR::Instr::New(Js::OpCode::BLR, this->m_func);
    instr->SetSrc1(r2Opnd);
    insertInstr->InsertBefore(instr);

    insertInstr->InsertBefore(doneLabelInstr);
}

//
// Emits the code to allocate 'size' amount of space on stack. for values smaller than PAGE_SIZE
// this will just emit sub SP,size otherwise calls _chkstk.
//
bool
LowererMD::GenerateStackAllocation(IR::Instr *instr, uint32 allocSize, uint32 probeSize)
{
    IR::RegOpnd* spOpnd = IR::RegOpnd::New(nullptr, GetRegStackPointer(), TyMachReg, this->m_func);

    if (IsSmallStack(probeSize))
    {
        AssertMsg(!(allocSize & 0xFFFFF000), "Must fit in 12 bits");
        AssertMsg(allocSize % MachStackAlignment == 0, "Must be aligned");
        // Generate SUB SP, SP, stackSize
        IR::IntConstOpnd *  stackSizeOpnd   = IR::IntConstOpnd::New(allocSize, TyMachReg, this->m_func, true);
        IR::Instr * subInstr = IR::Instr::New(Js::OpCode::SUB, spOpnd, spOpnd, stackSizeOpnd, this->m_func);
        instr->InsertBefore(subInstr);
        return false;
    }

    //__chkStk is a leaf function and hence alignment is not required.

    // Generate _chkstk call
    // LDIMM RegR15, stackSize/16
    // LDIMM RegR17, HelperCRT_chkstk
    // BLX RegR17
    // SUB SP, SP, x15, lsl #4

    //chkstk expects the stacksize argument in R15 register
    IR::RegOpnd *spAllocOpnd = IR::RegOpnd::New(nullptr, SP_ALLOC_SCRATCH_REG, TyMachReg, this->m_func);
    IR::RegOpnd *targetOpnd = IR::RegOpnd::New(nullptr, SCRATCH_REG, TyMachReg, this->m_func);

    IR::IntConstOpnd *  stackSizeOpnd   = IR::IntConstOpnd::New((allocSize / MachStackAlignment), TyMachReg, this->m_func, true);

    IR::Instr *movHelperAddrInstr = IR::Instr::New(Js::OpCode::LDIMM, targetOpnd, IR::HelperCallOpnd::New(IR::HelperCRT_chkstk, this->m_func), this->m_func);
    instr->InsertBefore(movHelperAddrInstr);

    IR::Instr *movInstr = IR::Instr::New(Js::OpCode::LDIMM, spAllocOpnd, stackSizeOpnd, this->m_func);
    instr->InsertBefore(movInstr);

    IR::Instr * callInstr = IR::Instr::New(Js::OpCode::BLR, spAllocOpnd, targetOpnd, this->m_func);
    instr->InsertBefore(callInstr);

    // _chkstk succeeded adjust SP by allocSize. r15 contains allocSize/16 so left shift r15 by 4 to get allocSize
    // Generate SUB SP, SP, x15, lsl #4
    IR::Instr * subInstr = IR::Instr::New(Js::OpCode::SUB_LSL4, spOpnd, spOpnd, spAllocOpnd, this->m_func);
    instr->InsertBefore(subInstr);

    // return true to imply scratch register is trashed
    return true;
}

void
LowererMD::GenerateStackDeallocation(IR::Instr *instr, uint32 allocSize)
{
    IR::RegOpnd * spOpnd = IR::RegOpnd::New(nullptr, this->GetRegStackPointer(), TyMachReg, this->m_func);

    IR::Instr * spAdjustInstr = IR::Instr::New(Js::OpCode::ADD,
        spOpnd,
        spOpnd,
        IR::IntConstOpnd::New(allocSize, TyMachReg, this->m_func, true), this->m_func);
    instr->InsertBefore(spAdjustInstr);
    LegalizeMD::LegalizeInstr(spAdjustInstr);
}



ARM64StackLayout::ARM64StackLayout(Func* func)
    : m_hasCalls(false),
    m_hasTry(func->HasTry()),
    m_argSlotCount(func->m_argSlotsForFunctionsCalled),
    m_localsArea(func->m_localStackHeight)
{
    Assert(m_localsArea % 16 == 0);
    Assert(m_argSlotCount % 2 == 0);

    // If there is a try, behave specially because the try/catch/finally helpers assume a
    // fully-populated stack layout.
    if (this->m_hasTry)
    {
        this->m_hasCalls = true;
        this->m_savedRegisters.SetRange(FIRST_CALLEE_SAVED_GP_REG, CALLEE_SAVED_GP_REG_COUNT);
        this->m_savedDoubles.SetRange(FIRST_CALLEE_SAVED_DBL_REG, CALLEE_SAVED_DOUBLE_REG_COUNT);
        this->m_homedParams.SetRange(0, NUM_INT_ARG_REGS);
    }

    // Otherwise, be more selective
    else
    {
        // Determine integer register saves. Since registers are always saved in pairs, mark both registers
        // in each pair as being saved even if only one is actually used.
        for (RegNum curReg = FIRST_CALLEE_SAVED_GP_REG; curReg <= LAST_CALLEE_SAVED_GP_REG; curReg = RegNum(curReg + 2))
        {
            Assert(LinearScan::IsCalleeSaved(curReg));
            RegNum nextReg = RegNum(curReg + 1);
            Assert(LinearScan::IsCalleeSaved(nextReg));
            if (func->m_regsUsed.Test(curReg) || func->m_regsUsed.Test(nextReg))
            {
                this->m_savedRegisters.SetRange(curReg, 2);
            }
        }

        // Determine double register saves. Since registers are always saved in pairs, mark both registers
        // in each pair as being saved even if only one is actually used.
        for (RegNum curReg = FIRST_CALLEE_SAVED_DBL_REG; curReg <= LAST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg + 2))
        {
            Assert(LinearScan::IsCalleeSaved(curReg));
            RegNum nextReg = RegNum(curReg + 1);
            Assert(LinearScan::IsCalleeSaved(nextReg));
            if (func->m_regsUsed.Test(curReg) || func->m_regsUsed.Test(nextReg))
            {
                this->m_savedDoubles.SetRange(curReg, 2);
            }
        }

        // W8-NEON slice 1: for AsmJs SIMD functions, the callee-saved vector
        // saves move to the 16-byte SavedSimd area (Q8-15, same RegNums as
        // D8-15) so full 128-bit values survive calls. Non-SIMD functions
        // keep the scalar layout bit-identical.
        bool funcUsesSimd = false;
        if (func->GetJITFunctionBody()->IsAsmJsMode())
        {
            const AsmJsJITInfo *info = func->GetJITFunctionBody()->GetAsmJsInfo();
            if (info != nullptr)
            {
                Js::AsmJsRetType::Which ret = info->GetRetType();
                funcUsesSimd = (ret == Js::AsmJsRetType::Int32x4 || ret == Js::AsmJsRetType::Int16x8 ||
                    ret == Js::AsmJsRetType::Int8x16 || ret == Js::AsmJsRetType::Uint32x4 ||
                    ret == Js::AsmJsRetType::Uint16x8 || ret == Js::AsmJsRetType::Uint8x16 ||
                    ret == Js::AsmJsRetType::Bool32x4 || ret == Js::AsmJsRetType::Bool16x8 ||
                    ret == Js::AsmJsRetType::Bool8x16 || ret == Js::AsmJsRetType::Float32x4 ||
                    ret == Js::AsmJsRetType::Float64x2 || ret == Js::AsmJsRetType::Int64x2);
                for (Js::ArgSlot i = 0; i < info->GetArgCount() && !funcUsesSimd; ++i)
                {
                    Js::AsmJsVarType::Which a = info->GetArgType(i);
                    funcUsesSimd = (a == Js::AsmJsVarType::Int32x4 || a == Js::AsmJsVarType::Int16x8 ||
                        a == Js::AsmJsVarType::Int8x16 || a == Js::AsmJsVarType::Uint32x4 ||
                        a == Js::AsmJsVarType::Uint16x8 || a == Js::AsmJsVarType::Uint8x16 ||
                        a == Js::AsmJsVarType::Bool32x4 || a == Js::AsmJsVarType::Bool16x8 ||
                        a == Js::AsmJsVarType::Bool8x16 || a == Js::AsmJsVarType::Float32x4 ||
                        a == Js::AsmJsVarType::Float64x2);
                }
                funcUsesSimd = funcUsesSimd || info->GetSimdVarCount() > 0 || info->GetSimdTmpCount() > 0
                    || info->GetSimdConstCount() > 0;
            }
        }
        if (funcUsesSimd && !this->m_savedDoubles.IsEmpty())
        {
            this->m_savedSimd = this->m_savedDoubles;
            this->m_savedDoubles.ClearAll();
        }
        // W8-NEON slice 1: remember SIMD use for homing below (Q0-7, 16B stack).
        // Layout totals must include the homed-simd area, so the check is
        // duplicated after HasCalls is known; the flag is recomputed there.

        // Determine if there are nested calls.
        //
        // If the function has a try, we need to have the same register saves in the prolog as the 
        // arm64_CallEhFrame helper, so that we can use the same epilog. So always allocate a slot 
        // for the stack nested func here whether we actually do have any stack nested func or not
        // TODO-STACK-NESTED-FUNC:  May be use a different arm64_CallEhFrame for when we have 
        // stack nested func?
        //
        // Note that this->TotalStackSize() will not include the homed parameters yet, so we add in
        // the worst case assumption (homing all NUM_INT_ARG_REGS).
        this->m_hasCalls = func->GetHasCalls() ||
            func->HasAnyStackNestedFunc() || 
            !LowererMD::IsSmallStack(this->TotalStackSize() + NUM_INT_ARG_REGS * MachRegInt) ||
            Lowerer::IsArgSaveRequired(func);

        // Home the params. This is done to enable on-the-fly creation of the arguments object,
        // Dyno bailout code, etc. For non-global functions, that means homing all the param registers
        // (since we have to assume they all have valid parameters). For the global function,
        // just home x0 (function object) and x1 (callinfo), which the runtime can't get by any other means.
        int homedParams = MIN_HOMED_PARAM_REGS;
        if (func->IsLoopBody())
        {
            // Jitted loop body takes only one "user" param: the pointer to the local slots.
            homedParams += 1;
        }
        else if (!this->m_hasCalls)
        {
            // A leaf function (no calls of any kind, including helpers) may still need its params, or, if it
            // has none, may still need the function object and call info.
            homedParams += func->GetInParamsCount();
        }
        else
        {
            homedParams = NUM_INT_ARG_REGS;
        }

        // Round up to an even number to keep stack alignment
        if (homedParams % 2 != 0)
        {
            homedParams += 1;
        }
        this->m_homedParams.SetRange(0, (homedParams < NUM_INT_ARG_REGS) ? homedParams : NUM_INT_ARG_REGS);
        // W8-NEON slice 1: home Q0-7 (16B each) for SIMD functions that call.
        // Q params arrive in Q0-7 (GetParamReg); a call clobbers Q0-7
        // (caller-saved), so they are homed like the int params.
        if (funcUsesSimd && this->m_hasCalls)
        {
            const AsmJsJITInfo *info2 = func->GetJITFunctionBody()->GetAsmJsInfo();
            int simdHomed = 0;
            if (info2 != nullptr)
            {
                for (Js::ArgSlot i = 0; i < info2->GetArgCount() && simdHomed < 8; ++i)
                {
                    Js::AsmJsVarType::Which a = info2->GetArgType(i);
                    if (a == Js::AsmJsVarType::Int32x4 || a == Js::AsmJsVarType::Int16x8 ||
                        a == Js::AsmJsVarType::Int8x16 || a == Js::AsmJsVarType::Uint32x4 ||
                        a == Js::AsmJsVarType::Uint16x8 || a == Js::AsmJsVarType::Uint8x16 ||
                        a == Js::AsmJsVarType::Bool32x4 || a == Js::AsmJsVarType::Bool16x8 ||
                        a == Js::AsmJsVarType::Bool8x16 || a == Js::AsmJsVarType::Float32x4 ||
                        a == Js::AsmJsVarType::Float64x2)
                    {
                        // Q index matches the vector-arg position, not the
                        // overall slot; home the first Q regs conservatively.
                        simdHomed++;
                    }
                }
            }
            // Round to pairs for STP alignment; always home at least Q0-Q1.
            if (simdHomed > 0 && (simdHomed % 2) != 0) simdHomed++;
            if (simdHomed == 0) simdHomed = 2;
            if (simdHomed > 8) simdHomed = 8;
            this->m_homedSimd.SetRange(RegQ0, simdHomed);
        }
    }
}

IR::Instr *
LowererMD::LowerEntryInstr(IR::EntryInstr * entryInstr)
{
    IR::Instr *insertInstr = entryInstr->m_next;

    // Begin recording info for later pdata/xdata emission.
    this->m_func->m_unwindInfo.Init(this->m_func);

    // Ensure there are an even number of slots for called functions
    if (this->m_func->m_argSlotsForFunctionsCalled % 2 != 0)
    {
        this->m_func->m_argSlotsForFunctionsCalled += 1;
    }

    if (this->m_func->HasInlinee())
    {
        // Allocate the inlined arg out stack in the locals. Allocate an additional slot so that
        // we can unconditionally clear the first slot past the current frame.
        this->m_func->m_localStackHeight += this->m_func->GetInlineeArgumentStackSize();
    }

    // Ensure the locals area is 16 byte aligned.
    this->m_func->m_localStackHeight = Math::Align<int32>(this->m_func->m_localStackHeight, MachStackAlignment);

    // Now that the localStackHeight has been adjusted, compute the final layout
    ARM64StackLayout layout(this->m_func);
    Assert(layout.TotalStackSize() % 16 == 0);

    // Set the arguments offset relative to the end of the locals area
    this->m_func->m_ArgumentsOffset = layout.HomedParamsOffset() - (layout.LocalsOffset() + layout.LocalsSize());

    // Set the frame height if inlinee arguments are needed
    if (m_func->GetMaxInlineeArgOutSize() != 0)
    {
        // subtracting 2 for frame pointer & return address
        this->m_func->GetJITOutput()->SetFrameHeight(this->m_func->m_localStackHeight + this->m_func->m_ArgumentsOffset - 2 * MachRegInt);
    }

    // Two situations to handle:
    //
    //    1. If total stack allocation < 512, we can do a single allocation up front
    //    2. Otherwise, we allocate the register area first, save regs, then allocate locals
    //
    // Breaking this down, there are two stack allocations
    //
    //    Allocation 1 = situation1 ? TotalStackSize : RegisterAreaSize
    //    Allocation 2 = TotalStackSize - Allocation 1
    //
    //    <probe>
    // prologStart:
    //    sub   sp, sp, #allocation1
    //    stp   d8-d15, [sp, #savedDoublesOffset - allocation2]
    //    stp   x19-x28, [sp, #savedRegistersOffset - allocation2]
    //    stp   fp, lr, [sp, #fpLrOffset - allocation2]
    //    add   fp, sp, #fpLrOffset - allocation2
    //    sub   sp, sp, #allocation2 (might be call to _chkstk)
    // prologEnd:
    //    stp   zr, zr, [fp, #argSlotOffset - fpLrOffset]
    //    stp   x0-x7, [fp, #paramSaveOffset - fpLrOffset]
    //    add   localsptr, sp, #localsOffset
    //    sub   ehsave, fp, #fpLrOffset - registerAreaOffset
    //

    // Determine the 1 or 2 stack allocation sizes
    ULONG stackAllocation1 = (layout.TotalStackSize() < 512) ? layout.TotalStackSize() : layout.RegisterAreaSize();
    ULONG stackAllocation2 = layout.TotalStackSize() - stackAllocation1;

//    this->GenerateDebugBreak(insertInstr);

    // Generate a stack probe for large stacks first even before register push
    bool fStackProbeAfterProlog = IsSmallStack(layout.TotalStackSize());
    if (!fStackProbeAfterProlog)
    {
        GenerateStackProbe(insertInstr, false);
    }

    // Create the prologStart label
    IR::LabelInstr *prologStartLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    insertInstr->InsertBefore(prologStartLabel);
    this->m_func->m_unwindInfo.SetFunctionOffsetLabel(UnwindPrologStart, prologStartLabel);

    // Perform the initial stack allocation (guaranteed to be small)
    IR::RegOpnd *spOpnd = IR::RegOpnd::New(nullptr, RegSP, TyMachReg, this->m_func);
    if (stackAllocation1 > 0)
    {
        IR::Instr * instrSub = IR::Instr::New(Js::OpCode::SUB, spOpnd, spOpnd, IR::IntConstOpnd::New(stackAllocation1, TyMachReg, this->m_func), this->m_func);
        insertInstr->InsertBefore(instrSub);
    }

    // Save doubles in pairs
    if (!layout.SavedDoubles().IsEmpty())
    {
        ULONG curOffset = layout.SavedDoublesOffset() - stackAllocation2;
        for (RegNum curReg = FIRST_CALLEE_SAVED_DBL_REG; curReg <= LAST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg + 2))
        {
            if (layout.SavedDoubles().Test(curReg))
            {
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrStp = IR::Instr::New(Js::OpCode::FSTP,
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(curReg, TyMachDouble, this->m_func),
                    IR::RegOpnd::New(nextReg, TyMachDouble, this->m_func), this->m_func);
                insertInstr->InsertBefore(instrStp);
                curOffset += 2 * MachRegDouble;
            }
        }
    }

    // W8-NEON slice 1: save Q8-15 as 16-byte pairs (STPQ, count*16).
    if (!layout.SavedSimd().IsEmpty())
    {
        ULONG curOffset = layout.SavedSimdOffset() - stackAllocation2;
        for (RegNum curReg = FIRST_CALLEE_SAVED_DBL_REG; curReg <= LAST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg + 2))
        {
            if (layout.SavedSimd().Test(curReg))
            {
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STPQ,
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(curReg, TySimd128I4, this->m_func),
                    IR::RegOpnd::New(nextReg, TySimd128I4, this->m_func), this->m_func);
                insertInstr->InsertBefore(instrStp);
                curOffset += 2 * MachRegSimd128;
            }
        }
    }

    // Save integer registers in pairs
    if (!layout.SavedRegisters().IsEmpty())
    {
        ULONG curOffset = layout.SavedRegistersOffset() - stackAllocation2;
        for (RegNum curReg = FIRST_CALLEE_SAVED_GP_REG; curReg <= LAST_CALLEE_SAVED_GP_REG; curReg = RegNum(curReg + 2))
        {
            if (layout.SavedRegisters().Test(curReg))
            {
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STP,
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(curReg, TyMachReg, this->m_func),
                    IR::RegOpnd::New(nextReg, TyMachReg, this->m_func), this->m_func);
                insertInstr->InsertBefore(instrStp);
                curOffset += 2 * MachRegInt;
            }
        }
    }

    // Save FP/LR and compute FP
    IR::RegOpnd *fpOpnd = fpOpnd = IR::RegOpnd::New(nullptr, RegFP, TyMachReg, this->m_func);
    if (layout.HasCalls())
    {
        // STP fp, lr, [sp, #offs]
        ULONG fpOffset = layout.FpLrOffset() - stackAllocation2;
        IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STP,
            IR::IndirOpnd::New(spOpnd, fpOffset, TyMachReg, this->m_func),
            fpOpnd, IR::RegOpnd::New(RegLR, TyMachReg, this->m_func), this->m_func);
        insertInstr->InsertBefore(instrStp);

        // ADD fp, sp, #offs
        // For exception handling, do this part AFTER the prolog to allow for proper unwinding
        if (!layout.HasTry())
        {
            Lowerer::InsertAdd(false, fpOpnd, spOpnd, IR::IntConstOpnd::New(fpOffset, TyMachReg, this->m_func), insertInstr);
        }
    }

    // Perform the second (potentially large) stack allocation
    if (stackAllocation2 > 0)
    {
        // TODO: is the probeSize parameter correct here?
        this->GenerateStackAllocation(insertInstr, stackAllocation2, stackAllocation1 + stackAllocation2);
    }

    // Future work in the register area should be done FP-relative if it is set up
    IR::RegOpnd *regAreaBaseOpnd = layout.HasCalls() ? fpOpnd : spOpnd;
    ULONG regAreaBaseOffset = layout.HasCalls() ? layout.FpLrOffset() : 0;

    // This marks the end of the formal prolog (for EH purposes); create and register a label
    IR::LabelInstr *prologEndLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    insertInstr->InsertBefore(prologEndLabel);
    this->m_func->m_unwindInfo.SetFunctionOffsetLabel(UnwindPrologEnd, prologEndLabel);

    // Compute the FP now if there is a try present
    if (layout.HasTry())
    {
        Lowerer::InsertAdd(false, fpOpnd, spOpnd, IR::IntConstOpnd::New(layout.FpLrOffset(), TyMachReg, this->m_func), insertInstr);
    }

    // Zero the argument slot if present
    IR::RegOpnd *zrOpnd = IR::RegOpnd::New(nullptr, RegZR, TyMachReg, this->m_func);
    if (layout.ArgSlotSize() > 0)
    {
        IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STP,
            IR::IndirOpnd::New(regAreaBaseOpnd, layout.ArgSlotOffset() - regAreaBaseOffset, TyMachReg, this->m_func),
            zrOpnd, zrOpnd, this->m_func);
        insertInstr->InsertBefore(instrStp);
    }

    // Home parameter registers in pairs
    if (!layout.HomedParams().IsEmpty())
    {
        ULONG curOffset = layout.HomedParamsOffset() - regAreaBaseOffset;
        for (RegNum curReg = FIRST_INT_ARG_REG; curReg <= LAST_INT_ARG_REG; curReg = RegNum(curReg + 2))
        {
            if (layout.HomedParams().Test(curReg))
            {
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STP,
                    IR::IndirOpnd::New(regAreaBaseOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(curReg, TyMachReg, this->m_func),
                    IR::RegOpnd::New(nextReg, TyMachReg, this->m_func), this->m_func);
                insertInstr->InsertBefore(instrStp);
                curOffset += 2 * MachRegInt;
            }
        }
    }

    // W8-NEON slice 1: home Q0-7 (16B each) in pairs.
    if (!layout.HomedSimd().IsEmpty())
    {
        ULONG curOffset = layout.HomedSimdOffset() - regAreaBaseOffset;
        for (RegNum curReg = (RegNum)RegQ0; curReg <= (RegNum)RegQ7; curReg = RegNum(curReg + 2))
        {
            if (layout.HomedSimd().Test(curReg))
            {
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrStp = IR::Instr::New(Js::OpCode::STPQ,
                    IR::IndirOpnd::New(regAreaBaseOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(curReg, TySimd128I4, this->m_func),
                    IR::RegOpnd::New(nextReg, TySimd128I4, this->m_func), this->m_func);
                insertInstr->InsertBefore(instrStp);
                curOffset += 2 * MachRegSimd128;
            }
        }
    }

    // Compute the locals pointer if needed
    RegNum localsReg = this->m_func->GetLocalsPointer();
    if (localsReg != RegSP)
    {
        IR::RegOpnd* localsOpnd = IR::RegOpnd::New(nullptr, localsReg, TyMachReg, this->m_func);
        Lowerer::InsertAdd(false, localsOpnd, spOpnd, IR::IntConstOpnd::New(layout.LocalsOffset(), TyMachReg, this->m_func), insertInstr);
    }

    // Zero initialize the first inlinee frames argc.
    if (this->m_func->GetMaxInlineeArgOutSize() != 0)
    {
        // STR argc, zr
        StackSym *sym = this->m_func->m_symTable->GetArgSlotSym((Js::ArgSlot) - 1);
        sym->m_isInlinedArgSlot = true;
        sym->m_offset = 0;
        IR::Instr * instrStr = IR::Instr::New(Js::OpCode::STR, IR::SymOpnd::New(sym, 0, TyMachReg, this->m_func), zrOpnd, this->m_func);
        insertInstr->InsertBefore(instrStr);
    }

    // Now do the stack probe for small stacks
    // hasCalls catches the recursion case
    if (layout.HasCalls() && fStackProbeAfterProlog)
    {
        GenerateStackProbe(insertInstr, true); //stack is already aligned in this case
    }

    // Guard the slot-image reserve floor after the prolog (FDE-covered, like the
    // small-stack probe above) so a reserve breach throws from unwindable code.
    // Only asm.js/wasm functions run under AsmJsJitCall with the fixed alloca
    // reserve. The prolog has already allocated the frame; the check trips one
    // frame later, still far above the reserve bottom.
    if (m_func->GetJITFunctionBody()->IsAsmJsMode())
    {
        GenerateWasmReserveFloorCheck(insertInstr);
    }

#if defined(__APPLE__) && defined(_M_ARM64)
    // Record the finished frame for the DWARF FDE. Offsets are from the final
    // SP. arm64_CallEhFrame builds this same register area, so a throw inside
    // a try unwinds back to OP_TryCatch instead of the parent caller.
    {
        UnwindInfoManager* unwind = &this->m_func->m_unwindInfo;
        unwind->ResetDarwinFrame(layout.TotalStackSize());
        if (!layout.SavedDoubles().IsEmpty())
        {
            ULONG offset = layout.SavedDoublesOffset();
            for (RegNum curReg = FIRST_CALLEE_SAVED_DBL_REG; curReg <= LAST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg + 2))
            {
                if (layout.SavedDoubles().Test(curReg))
                {
                    // RegEncode stores NEONREG_BASE + n. DWARF Vn is 64 + n.
                    unwind->AddDarwinSave((uint16)(EncoderMD::GetRegEncode(curReg) - NEONREG_BASE + 64), offset);
                    unwind->AddDarwinSave((uint16)(EncoderMD::GetRegEncode(RegNum(curReg + 1)) - NEONREG_BASE + 64), offset + MachRegDouble);
                    offset += 2 * MachRegDouble;
                }
            }
        }
        // W8-NEON slice 1: Q8-15 Darwin Vn (64+n), 16-byte stride.
        if (!layout.SavedSimd().IsEmpty())
        {
            ULONG offset = layout.SavedSimdOffset();
            for (RegNum curReg = FIRST_CALLEE_SAVED_DBL_REG; curReg <= LAST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg + 2))
            {
                if (layout.SavedSimd().Test(curReg))
                {
                    unwind->AddDarwinSave((uint16)(EncoderMD::GetRegEncode(curReg) - NEONREG_BASE + 64), offset);
                    unwind->AddDarwinSave((uint16)(EncoderMD::GetRegEncode(RegNum(curReg + 1)) - NEONREG_BASE + 64), offset + MachRegSimd128);
                    offset += 2 * MachRegSimd128;
                }
            }
        }
        if (!layout.SavedRegisters().IsEmpty())
        {
            ULONG offset = layout.SavedRegistersOffset();
            for (RegNum curReg = FIRST_CALLEE_SAVED_GP_REG; curReg <= LAST_CALLEE_SAVED_GP_REG; curReg = RegNum(curReg + 2))
            {
                if (layout.SavedRegisters().Test(curReg))
                {
                    unwind->AddDarwinSave(EncoderMD::GetRegEncode(curReg), offset);
                    unwind->AddDarwinSave(EncoderMD::GetRegEncode(RegNum(curReg + 1)), offset + MachRegInt);
                    offset += 2 * MachRegInt;
                }
            }
        }
        if (layout.HasCalls())
        {
            unwind->AddDarwinSave(EncoderMD::GetRegEncode(RegFP), layout.FpLrOffset());
            unwind->AddDarwinSave(EncoderMD::GetRegEncode(RegLR), layout.FpLrOffset() + MachRegInt);
        }
    }
#endif

    return entryInstr;
}

IR::Instr *
LowererMD::LowerExitInstr(IR::ExitInstr * exitInstr)
{
    // Compute the final layout (should match the prolog)
    ARM64StackLayout layout(this->m_func);
    Assert(layout.TotalStackSize() % 16 == 0);

    // Determine the 1 or 2 stack allocation sizes
    // Note that on exit, if there is a try, we always do a 2-step deallocation because the
    // epilog is re-used by the try/catch/finally code
    ULONG stackAllocation1 = (layout.TotalStackSize() < 512 && !layout.HasTry()) ? layout.TotalStackSize() : layout.RegisterAreaSize();
    ULONG stackAllocation2 = layout.TotalStackSize() - stackAllocation1;

    // Mark the start of the epilog
    IR::LabelInstr *epilogStartLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    exitInstr->InsertBefore(epilogStartLabel);
    this->m_func->m_unwindInfo.SetFunctionOffsetLabel(UnwindEpilogStart, epilogStartLabel);

    IR::RegOpnd *spOpnd = IR::RegOpnd::New(nullptr, RegSP, TyMachReg, this->m_func);
    IR::RegOpnd *fpOpnd = IR::RegOpnd::New(nullptr, RegFP, TyMachReg, this->m_func);

    // Exception handling regions exit via the same epilog
    IR::LabelInstr* ehEpilogLabel = this->m_func->m_epilogLabel;
    if (ehEpilogLabel != nullptr)
    {
        ehEpilogLabel->Unlink();
        exitInstr->InsertBefore(ehEpilogLabel);
    }

    // Undo the last stack allocation
    if (stackAllocation2 > 0)
    {
        GenerateStackDeallocation(exitInstr, stackAllocation2);
    }

    // Recover FP and LR
    if (layout.HasCalls())
    {
        // LDP fp, lr, [sp, #offs]
        ULONG fpOffset = layout.FpLrOffset() - stackAllocation2;
        IR::Instr * instrLdp = IR::Instr::New(Js::OpCode::LDP, fpOpnd,
            IR::IndirOpnd::New(spOpnd, fpOffset, TyMachReg, this->m_func),
            IR::RegOpnd::New(RegLR, TyMachReg, this->m_func), this->m_func);
        exitInstr->InsertBefore(instrLdp);
    }

    // Recover integer registers in pairs
    if (!layout.SavedRegisters().IsEmpty())
    {
        ULONG curOffset = layout.SavedRegistersOffset() - stackAllocation2 + layout.SavedRegistersSize();
        for (RegNum curReg = RegNum(LAST_CALLEE_SAVED_GP_REG - 1); curReg >= FIRST_CALLEE_SAVED_GP_REG; curReg = RegNum(curReg - 2))
        {
            if (layout.SavedRegisters().Test(curReg))
            {
                curOffset -= 2 * MachRegInt;
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrLdp = IR::Instr::New(Js::OpCode::LDP,
                    IR::RegOpnd::New(curReg, TyMachReg, this->m_func),
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(nextReg, TyMachReg, this->m_func), this->m_func);
                exitInstr->InsertBefore(instrLdp);
            }
        }
    }

    // Recover doubles in pairs
    if (!layout.SavedDoubles().IsEmpty())
    {
        ULONG curOffset = layout.SavedDoublesOffset() - stackAllocation2 + layout.SavedDoublesSize();
        for (RegNum curReg = RegNum(LAST_CALLEE_SAVED_DBL_REG - 1); curReg >= FIRST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg - 2))
        {
            if (layout.SavedDoubles().Test(curReg))
            {
                curOffset -= 2 * MachRegDouble;
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrLdp = IR::Instr::New(Js::OpCode::FLDP,
                    IR::RegOpnd::New(curReg, TyMachDouble, this->m_func),
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(nextReg, TyMachDouble, this->m_func), this->m_func);
                exitInstr->InsertBefore(instrLdp);
            }
        }
    }

    // W8-NEON slice 1: restore Q8-15 (LDPQ, 16-byte pairs).
    if (!layout.SavedSimd().IsEmpty())
    {
        ULONG curOffset = layout.SavedSimdOffset() - stackAllocation2 + layout.SavedSimdSize();
        for (RegNum curReg = RegNum(LAST_CALLEE_SAVED_DBL_REG - 1); curReg >= FIRST_CALLEE_SAVED_DBL_REG; curReg = RegNum(curReg - 2))
        {
            if (layout.SavedSimd().Test(curReg))
            {
                curOffset -= 2 * MachRegSimd128;
                RegNum nextReg = RegNum(curReg + 1);
                IR::Instr * instrLdp = IR::Instr::New(Js::OpCode::LDPQ,
                    IR::RegOpnd::New(curReg, TySimd128I4, this->m_func),
                    IR::IndirOpnd::New(spOpnd, curOffset, TyMachReg, this->m_func),
                    IR::RegOpnd::New(nextReg, TySimd128I4, this->m_func), this->m_func);
                exitInstr->InsertBefore(instrLdp);
            }
        }
    }

    // Final stack deallocation
    if (stackAllocation1 > 0)
    {
        GenerateStackDeallocation(exitInstr, stackAllocation1);
    }

    // Return
    IR::Instr *  instrRet = IR::Instr::New(Js::OpCode::RET, nullptr, IR::RegOpnd::New(nullptr, RegLR, TyMachReg, this->m_func), this->m_func);
    exitInstr->InsertBefore(instrRet);

    // Label the end
    IR::LabelInstr *epilogEndLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    exitInstr->InsertBefore(epilogEndLabel);
    this->m_func->m_unwindInfo.SetFunctionOffsetLabel(UnwindEpilogEnd, epilogEndLabel);

    return exitInstr;
}

IR::Instr *
LowererMD::LoadNewScObjFirstArg(IR::Instr * instr, IR::Opnd * argSrc, ushort extraArgs)
{
    // Spread moves down the argument slot by one.
    // LowerCallArgs will handle the extraArgs. We only need to specify the argument number
    // i.e 1 and not + extraArgs as done in AMD64
    IR::SymOpnd *argOpnd = IR::SymOpnd::New(this->m_func->m_symTable->GetArgSlotSym(1), TyVar, this->m_func);
    IR::Instr *argInstr = IR::Instr::New(Js::OpCode::ArgOut_A, argOpnd, argSrc, this->m_func);
    instr->InsertBefore(argInstr);

    // Insert the argument into the arg chain.
    if (m_lowerer->IsSpreadCall(instr))
    {
        // Spread calls need LdSpreadIndices as the last arg in the arg chain.
        instr = m_lowerer->GetLdSpreadIndicesInstr(instr);
    }
    IR::Opnd *linkOpnd = instr->UnlinkSrc2();
    argInstr->SetSrc2(linkOpnd);
    instr->SetSrc2(argOpnd);

    return argInstr;
}

IR::Instr *
LowererMD::LowerTry(IR::Instr * tryInstr, IR::JnHelperMethod helperMethod)
{
    // Mark the entry to the try
    IR::Instr * instr = tryInstr->GetNextRealInstrOrLabel();
    AssertMsg(instr->IsLabelInstr(), "No label at the entry to a try?");
    IR::LabelInstr * tryAddr = instr->AsLabelInstr();

    // Arg 7: ScriptContext
    this->m_lowerer->LoadScriptContext(tryAddr);

    if (tryInstr->m_opcode == Js::OpCode::TryCatch || this->m_func->DoOptimizeTry() || (this->m_func->IsSimpleJit() && this->m_func->hasBailout))
    {
        // Arg 6 : hasBailedOutOffset
        IR::Opnd * hasBailedOutOffset = IR::IntConstOpnd::New(this->m_func->GetHasBailedOutSym()->m_offset + tryInstr->m_func->GetInlineeArgumentStackSize(), TyInt32, this->m_func);
        this->LoadHelperArgument(tryAddr, hasBailedOutOffset);
    }

    // Arg 5: arg out size
    IR::RegOpnd * argOutSize = IR::RegOpnd::New(TyMachReg, this->m_func);
    instr = IR::Instr::New(Js::OpCode::LDARGOUTSZ, argOutSize, this->m_func);
    tryAddr->InsertBefore(instr);
    this->LoadHelperArgument(tryAddr, argOutSize);

    // Arg 4: locals pointer
    IR::RegOpnd * localsPtr = IR::RegOpnd::New(nullptr, this->m_func->GetLocalsPointer(), TyMachReg, this->m_func);
    this->LoadHelperArgument(tryAddr, localsPtr);

    // Arg 3: frame pointer
    IR::RegOpnd * framePtr = IR::RegOpnd::New(nullptr, FRAME_REG, TyMachReg, this->m_func);
    this->LoadHelperArgument(tryAddr, framePtr);

    // Arg 2: helper address
    IR::LabelInstr * helperAddr = tryInstr->AsBranchInstr()->GetTarget();
    this->LoadHelperArgument(tryAddr, IR::LabelOpnd::New(helperAddr, this->m_func));

    // Arg 1: try address
    this->LoadHelperArgument(tryAddr, IR::LabelOpnd::New(tryAddr, this->m_func));

    // Call the helper
    IR::RegOpnd *continuationAddr =
        IR::RegOpnd::New(StackSym::New(TyMachReg,this->m_func), RETURN_REG, TyMachReg, this->m_func);
    IR::Instr * callInstr = IR::Instr::New(
        Js::OpCode::Call, continuationAddr, IR::HelperCallOpnd::New(helperMethod, this->m_func), this->m_func);
    tryAddr->InsertBefore(callInstr);
    this->LowerCall(callInstr, 0);

    // Jump to the continuation address supplied by the helper
    IR::BranchInstr *branchInstr = IR::MultiBranchInstr::New(Js::OpCode::BR, continuationAddr, this->m_func);
    tryAddr->InsertBefore(branchInstr);

    return tryInstr->m_prev;
}

IR::Instr *
LowererMD::LowerLeaveNull(IR::Instr * leaveInstr)
{
    IR::Instr * instrPrev = leaveInstr->m_prev;

    // Return a NULL continuation address to the caller to indicate that the finally did not seize the flow.
    this->LowerEHRegionReturn(leaveInstr, IR::IntConstOpnd::New(0, TyMachReg, this->m_func));

    leaveInstr->Remove();
    return instrPrev;
}

IR::Instr *
LowererMD::LowerEHRegionReturn(IR::Instr * insertBeforeInstr, IR::Opnd * targetOpnd)
{
    IR::RegOpnd *retReg    = IR::RegOpnd::New(nullptr, RETURN_REG, TyMachReg, this->m_func);

    // Load the continuation address into the return register.
    Lowerer::InsertMove(retReg, targetOpnd, insertBeforeInstr);

    IR::LabelInstr *epilogLabel = this->EnsureEHEpilogLabel();
    IR::BranchInstr *jmpInstr = IR::BranchInstr::New(Js::OpCode::B, epilogLabel, this->m_func);
    insertBeforeInstr->InsertBefore(jmpInstr);

    // return the last instruction inserted
    return jmpInstr;
}

///----------------------------------------------------------------------------
///
/// LowererMD::Init
///
///----------------------------------------------------------------------------

void
LowererMD::Init(Lowerer *lowerer)
{
    m_lowerer = lowerer;
    // The arg slot count computed by an earlier phase (e.g., IRBuilder) doesn't work for
    // ARM if it accounts for nesting. Clear it here and let Lower compute its own value.
    this->m_func->m_argSlotsForFunctionsCalled = 0;
#ifdef ENABLE_WASM_SIMD
    Simd128InitOpcodeMap();
#endif
}

// W8-NEON slice 1: NEON-native opcode map. Unlike the x86 SSE map
// (LowerMDSharedSimd128.cpp), this maps only the slice-1 direct families;
// everything else stays unmapped and fails safe in Simd128Instruction.
#ifdef ENABLE_WASM_SIMD
#define GET_NEONOPCODE(irOpcode) m_simd128OpCodesMap[(uint32)(irOpcode - Js::OpCode::Simd128_Start)]
#define SET_NEONOPCODE(irOpcode, mdOpcode) \
    Assert((uint32)m_simd128OpCodesMap[(uint32)(Js::OpCode::irOpcode - Js::OpCode::Simd128_Start)] == 0);\
    Assert(Js::OpCode::mdOpcode > Js::OpCode::MDStart);\
    m_simd128OpCodesMap[(uint32)(Js::OpCode::irOpcode - Js::OpCode::Simd128_Start)] = Js::OpCode::mdOpcode;

void
LowererMD::Simd128InitOpcodeMap()
{
    m_simd128OpCodesMap = JitAnewArrayZ(m_lowerer->m_alloc, Js::OpCode, Js::Simd128OpcodeCount());
    Assert(Js::OpCode::Simd128_End + (Js::OpCode)1 == Js::OpCode::Simd128_Start_Extend);
    // Logical (bitwise, SIZE_16B): PAND/POR/PXOR cover And/Or/Xor across
    // lane shapes (I4/I8/I16/U/B variants are the same instruction).
    SET_NEONOPCODE(Simd128_And_I4, PAND);
    SET_NEONOPCODE(Simd128_And_I8, PAND);
    SET_NEONOPCODE(Simd128_And_I16, PAND);
    SET_NEONOPCODE(Simd128_And_U4, PAND);
    SET_NEONOPCODE(Simd128_And_U8, PAND);
    SET_NEONOPCODE(Simd128_And_U16, PAND);
    SET_NEONOPCODE(Simd128_And_B4, PAND);
    SET_NEONOPCODE(Simd128_And_B8, PAND);
    SET_NEONOPCODE(Simd128_And_B16, PAND);
    SET_NEONOPCODE(Simd128_Or_I4, POR);
    SET_NEONOPCODE(Simd128_Or_I8, POR);
    SET_NEONOPCODE(Simd128_Or_I16, POR);
    SET_NEONOPCODE(Simd128_Or_U4, POR);
    SET_NEONOPCODE(Simd128_Or_U8, POR);
    SET_NEONOPCODE(Simd128_Or_U16, POR);
    SET_NEONOPCODE(Simd128_Or_B4, POR);
    SET_NEONOPCODE(Simd128_Or_B8, POR);
    SET_NEONOPCODE(Simd128_Or_B16, POR);
    SET_NEONOPCODE(Simd128_Xor_I4, PXOR);
    SET_NEONOPCODE(Simd128_Xor_I8, PXOR);
    SET_NEONOPCODE(Simd128_Xor_I16, PXOR);
    SET_NEONOPCODE(Simd128_Xor_U4, PXOR);
    SET_NEONOPCODE(Simd128_Xor_U8, PXOR);
    SET_NEONOPCODE(Simd128_Xor_U16, PXOR);
    SET_NEONOPCODE(Simd128_Xor_B4, PXOR);
    SET_NEONOPCODE(Simd128_Xor_B8, PXOR);
    SET_NEONOPCODE(Simd128_Xor_B16, PXOR);
    // Integer add/sub. PADDD/PSUBD use the 4S form (32-bit lanes); PADDQ/PSUBQ
    // use the 2D form (64-bit lanes). 8/16-bit lanes are the same add/sub
    // with a narrower lane size, encoded from the IR type in EmitNeon.
    SET_NEONOPCODE(Simd128_Add_I4, PADDD);
    SET_NEONOPCODE(Simd128_Sub_I4, PSUBD);
    SET_NEONOPCODE(Simd128_Add_I8, PADDD);
    SET_NEONOPCODE(Simd128_Sub_I8, PSUBD);
    SET_NEONOPCODE(Simd128_Add_I16, PADDD);
    SET_NEONOPCODE(Simd128_Sub_I16, PSUBD);
    SET_NEONOPCODE(Simd128_Add_U4, PADDD);
    SET_NEONOPCODE(Simd128_Sub_U4, PSUBD);
    SET_NEONOPCODE(Simd128_Add_U8, PADDD);
    SET_NEONOPCODE(Simd128_Sub_U8, PSUBD);
    SET_NEONOPCODE(Simd128_Add_U16, PADDD);
    SET_NEONOPCODE(Simd128_Sub_U16, PSUBD);
    SET_NEONOPCODE(Simd128_Add_I2, PADDQ);
    SET_NEONOPCODE(Simd128_Sub_I2, PSUBQ);
    // Slice 2e (D2 arithmetic): float64x2 add/sub/mul/div/sqrt are NEON
    // float vector ops (FADD etc. with SIZE_2D), NOT the integer PADDQ/PSUBQ
    // the D2 spellings were previously lumped with I2 under. Integer 64-bit
    // lanes keep PADDQ/PSUBQ; double lanes need float arithmetic (integer
    // add of double bit patterns silently miscomputes, e.g. 1.0+2.0 would
    // yield +Inf bits).
    SET_NEONOPCODE(Simd128_Add_D2, ADDPD);
    SET_NEONOPCODE(Simd128_Sub_D2, SUBPD);
    SET_NEONOPCODE(Simd128_Mul_D2, MULPD);
    SET_NEONOPCODE(Simd128_Div_D2, DIVPD);
    SET_NEONOPCODE(Simd128_Sqrt_D2, SQRTPD);
    // Floating add/sub/mul/div/sqrt are the same vector shape (FADD etc. with
    // SIZE_4S/SIZE_2D), so they are included; scalar FADD/FMUL/FDIV/FSQRT MD
    // opcodes are distinct (EmitNeonFadd with SIZE_1S/1D).
    SET_NEONOPCODE(Simd128_Add_F4, ADDPS);
    SET_NEONOPCODE(Simd128_Sub_F4, SUBPS);
    SET_NEONOPCODE(Simd128_Mul_F4, MULPS);
    SET_NEONOPCODE(Simd128_Div_F4, DIVPS);
    SET_NEONOPCODE(Simd128_Sqrt_F4, SQRTPS);
    // Slice 2a: integer compares. Eq is bitwise-identical across signed/
    // unsigned spellings, so U aliases share CMEQ. Signed Gt uses CMGT,
    // unsigned Gt uses CMHI (native unsigned compare; no x86-style sign-bias
    // sequence needed). Ge uses CMGE (signed) / CMHS (unsigned) directly;
    // Lt/Le reach the same instructions via operand swap in
    // Simd128TryLowerMappedInstruction. Ne is not mapped (CMEQ+PMVN pair in
    // Simd128LowerUnMappedInstruction). Result lanes are all-ones/all-zero
    // masks natively. B-type sources have no compare IR opcodes (Bool is only
    // the result type), so no B entries are needed.
    SET_NEONOPCODE(Simd128_Eq_I4, CMEQ);
    SET_NEONOPCODE(Simd128_Eq_U4, CMEQ);
    SET_NEONOPCODE(Simd128_Eq_I8, CMEQ);
    SET_NEONOPCODE(Simd128_Eq_U8, CMEQ);
    SET_NEONOPCODE(Simd128_Eq_I16, CMEQ);
    SET_NEONOPCODE(Simd128_Eq_U16, CMEQ);
    SET_NEONOPCODE(Simd128_Gt_I4, CMGT);
    SET_NEONOPCODE(Simd128_Gt_I8, CMGT);
    SET_NEONOPCODE(Simd128_Gt_I16, CMGT);
    SET_NEONOPCODE(Simd128_Lt_I4, CMGT);
    SET_NEONOPCODE(Simd128_Lt_I8, CMGT);
    SET_NEONOPCODE(Simd128_Lt_I16, CMGT);
    SET_NEONOPCODE(Simd128_GtEq_I4, CMGE);
    SET_NEONOPCODE(Simd128_GtEq_I8, CMGE);
    SET_NEONOPCODE(Simd128_GtEq_I16, CMGE);
    SET_NEONOPCODE(Simd128_LtEq_I4, CMGE);
    SET_NEONOPCODE(Simd128_LtEq_I8, CMGE);
    SET_NEONOPCODE(Simd128_LtEq_I16, CMGE);
    SET_NEONOPCODE(Simd128_Gt_U4, CMHI);
    SET_NEONOPCODE(Simd128_Gt_U8, CMHI);
    SET_NEONOPCODE(Simd128_Gt_U16, CMHI);
    SET_NEONOPCODE(Simd128_Lt_U4, CMHI);
    SET_NEONOPCODE(Simd128_Lt_U8, CMHI);
    SET_NEONOPCODE(Simd128_Lt_U16, CMHI);
    SET_NEONOPCODE(Simd128_GtEq_U4, CMHS);
    SET_NEONOPCODE(Simd128_GtEq_U8, CMHS);
    SET_NEONOPCODE(Simd128_GtEq_U16, CMHS);
    SET_NEONOPCODE(Simd128_LtEq_U4, CMHS);
    SET_NEONOPCODE(Simd128_LtEq_U8, CMHS);
    SET_NEONOPCODE(Simd128_LtEq_U16, CMHS);
    // Slice 2a: V128Not. The wasm decoder maps v128.not to Simd128_Not_I4;
    // the asm.js Not_*/B* spellings are the same bitwise complement, so they
    // share PMVN (vector MVN, no all-ones mask load like x64 XORPS).
    SET_NEONOPCODE(Simd128_Not_I4, PMVN);
    SET_NEONOPCODE(Simd128_Not_I8, PMVN);
    SET_NEONOPCODE(Simd128_Not_I16, PMVN);
    SET_NEONOPCODE(Simd128_Not_U4, PMVN);
    SET_NEONOPCODE(Simd128_Not_U8, PMVN);
    SET_NEONOPCODE(Simd128_Not_U16, PMVN);
    SET_NEONOPCODE(Simd128_Not_B4, PMVN);
    SET_NEONOPCODE(Simd128_Not_B8, PMVN);
    // Slice 2a: integer multiply. NEON MUL keeps the low half per lane
    // (2's-complement wrap), matching wasm mul semantics, so one MD opcode
    // covers I4/U4 (4S), I8/U8 (8H) and I16/U16 (16B); lane size comes from
    // the IR type. (No I2 mul exists in this vintage.)
    SET_NEONOPCODE(Simd128_Mul_I4, PMULW);
    SET_NEONOPCODE(Simd128_Mul_U4, PMULW);
    SET_NEONOPCODE(Simd128_Mul_I8, PMULW);
    SET_NEONOPCODE(Simd128_Mul_U8, PMULW);
    SET_NEONOPCODE(Simd128_Mul_I16, PMULW);
    SET_NEONOPCODE(Simd128_Mul_U16, PMULW);
    // Slice 2c: saturating integer arithmetic. NEON SQADD/SQSUB/UQADD/
    // UQSUB saturate per lane exactly like the wasm add_sat/sub_sat
    // families (signed saturate to the lane's INT_MIN/INT_MAX, unsigned
    // add saturates to UINT_MAX, unsigned sub saturates to 0), so one MD
    // opcode covers the I8 (16-bit lanes) and I16 (8-bit lanes) spellings
    // alike; the lane size comes from the IR type. There is no 32-bit
    // saturating family in this vintage (no such wasm opcode), and no
    // narrow/widening-load IR opcodes at all (no load8x8/load16x4/
    // load32x2/load64x2 in WasmBinaryOpcodesSimd.h), so those stay FailFast
    // by absence: nothing can construct them.
    SET_NEONOPCODE(Simd128_AddSaturate_I8, SQADDV);
    SET_NEONOPCODE(Simd128_SubSaturate_I8, SQSUBV);
    SET_NEONOPCODE(Simd128_AddSaturate_I16, SQADDV);
    SET_NEONOPCODE(Simd128_SubSaturate_I16, SQSUBV);
    SET_NEONOPCODE(Simd128_AddSaturate_U8, UQADDV);
    SET_NEONOPCODE(Simd128_SubSaturate_U8, UQSUBV);
    SET_NEONOPCODE(Simd128_AddSaturate_U16, UQADDV);
    SET_NEONOPCODE(Simd128_SubSaturate_U16, UQSUBV);
    // Slice 2d: float compares. NEON FCMEQ/FCMGT/FCMGE yield all-ones on
    // true and all-zero on false-or-NaN with no trap, matching wasm compare
    // semantics exactly (eq false on NaN, ordered false on NaN). Lt/Le go
    // through the operand swap below; Ne goes through CompareInverted.
    // Lane size comes from the IR type (F4/B4 -> 4S, D2/I2 -> 2D).
    SET_NEONOPCODE(Simd128_Eq_F4, FCMEQV);
    SET_NEONOPCODE(Simd128_Gt_F4, FCMGTV);
    SET_NEONOPCODE(Simd128_GtEq_F4, FCMGEV);
    SET_NEONOPCODE(Simd128_Lt_F4, FCMGTV);
    SET_NEONOPCODE(Simd128_LtEq_F4, FCMGEV);
    SET_NEONOPCODE(Simd128_Eq_D2, FCMEQV);
    SET_NEONOPCODE(Simd128_Gt_D2, FCMGTV);
    SET_NEONOPCODE(Simd128_GtEq_D2, FCMGEV);
    SET_NEONOPCODE(Simd128_Lt_D2, FCMGTV);
    SET_NEONOPCODE(Simd128_LtEq_D2, FCMGEV);
    // Slice 2e: negate. NEON NEG negates each integer lane (2's complement,
    // matching wasm neg for I4/I8/I16/I2; unsigned spellings U4/U8/U16 map
    // here too since negation is bitwise-identical regardless of signedness)
    // and FNEG flips each float lane's sign bit (NaN-quiet, matching wasm
    // F4/D2 neg). Lane size comes from the IR type.
    SET_NEONOPCODE(Simd128_Neg_I4, NEGV);
    SET_NEONOPCODE(Simd128_Neg_I8, NEGV);
    SET_NEONOPCODE(Simd128_Neg_I16, NEGV);
    SET_NEONOPCODE(Simd128_Neg_I2, NEGV);
    SET_NEONOPCODE(Simd128_Neg_U4, NEGV);
    SET_NEONOPCODE(Simd128_Neg_U8, NEGV);
    SET_NEONOPCODE(Simd128_Neg_U16, NEGV);
    SET_NEONOPCODE(Simd128_Neg_F4, FNEGV);
    SET_NEONOPCODE(Simd128_Neg_D2, FNEGV);
    // Slice 2e (abs): NEON FABS per lane (NaN-quiet, -0 -> +0), matching
    // wasm abs exactly. Lane size comes from the IR type.
    SET_NEONOPCODE(Simd128_Abs_F4, FABSV);
    SET_NEONOPCODE(Simd128_Abs_D2, FABSV);
}

IR::Instr *
LowererMD::Simd128Instruction(IR::Instr *instr)
{
    if (!instr->GetDst())
    {
        Assert(!instr->m_func->GetJITFunctionBody()->IsAsmJsMode());
        IR::Instr *pInstr = instr->m_prev;
        instr->Remove();
        return pInstr;
    }
    if (Simd128TryLowerMappedInstruction(instr))
    {
        return instr->m_prev;
    }
    return Simd128LowerUnMappedInstruction(instr);
}

bool
LowererMD::Simd128TryLowerMappedInstruction(IR::Instr *instr)
{
    Js::OpCode opcode = GET_NEONOPCODE(instr->m_opcode);
    if ((uint32)opcode == 0)
    {
        return false;
    }
    Assert(instr->GetDst() && instr->GetDst()->IsRegOpnd() && instr->GetDst()->IsSimd128());
    Assert(instr->GetSrc1() && instr->GetSrc1()->IsRegOpnd() && instr->GetSrc1()->IsSimd128());
    Assert(!instr->GetSrc2() || (instr->GetSrc2()->IsRegOpnd() && instr->GetSrc2()->IsSimd128()));
    // Slice 2a: Lt/Le via operand swap (mirrors x64 Simd128TryLowerMappedInstruction,
    // which swaps Lt to Gt; extended here to Le via Ge for both signs).
    // Lt(a,b) == Gt(b,a); Le(a,b) == Ge(b,a); unsigned pairs use CMHI/CMHS.
    bool swapOperands = false;
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_Lt_I4:
    case Js::OpCode::Simd128_Lt_I8:
    case Js::OpCode::Simd128_Lt_I16:
        Assert(opcode == Js::OpCode::CMGT);
        swapOperands = true;
        break;
    case Js::OpCode::Simd128_Lt_U4:
    case Js::OpCode::Simd128_Lt_U8:
    case Js::OpCode::Simd128_Lt_U16:
        Assert(opcode == Js::OpCode::CMHI);
        swapOperands = true;
        break;
    case Js::OpCode::Simd128_LtEq_I4:
    case Js::OpCode::Simd128_LtEq_I8:
    case Js::OpCode::Simd128_LtEq_I16:
        Assert(opcode == Js::OpCode::CMGE);
        swapOperands = true;
        break;
    case Js::OpCode::Simd128_LtEq_U4:
    case Js::OpCode::Simd128_LtEq_U8:
    case Js::OpCode::Simd128_LtEq_U16:
        Assert(opcode == Js::OpCode::CMHS);
        swapOperands = true;
        break;
    // Slice 2d: float Lt/Le swap onto Gt/Ge. Lt(a,b) == Gt(b,a) and
    // Le(a,b) == Ge(b,a) hold bitwise for NaN too (both sides all-zero),
    // so no NaN fixup is needed (unlike min/max).
    case Js::OpCode::Simd128_Lt_F4:
    case Js::OpCode::Simd128_Lt_D2:
        Assert(opcode == Js::OpCode::FCMGTV);
        swapOperands = true;
        break;
    case Js::OpCode::Simd128_LtEq_F4:
    case Js::OpCode::Simd128_LtEq_D2:
        Assert(opcode == Js::OpCode::FCMGEV);
        swapOperands = true;
        break;
    default:
        break;
    }
    if (swapOperands)
    {
        IR::Opnd *src1 = instr->UnlinkSrc1();
        IR::Opnd *src2 = instr->UnlinkSrc2();
        instr->SetSrc1(src2);
        instr->SetSrc2(src1);
    }
    instr->m_opcode = opcode;
    LegalizeMD::LegalizeInstr(instr);
    return true;
}

IR::Instr *
LowererMD::Simd128LowerUnMappedInstruction(IR::Instr *instr)
{
    // Slice-1 unmapped: LdC (constant load via MOVQ) and 16-byte LdArr/StArr
    // (Q loads/stores for wasm memory) are lowered.
    // Slice 2a unmapped: Ne (CMEQ + PMVN pair; every other integer compare is
    // a single mapped NEON instruction).
    // Slice 2b unmapped: splat/extract/replace/shuffle + conversions below.
    // Slice 2c unmapped: shifts, float min/max (F4 + D2). Saturating
    // arithmetic is mapped (single NEON instruction each).
    // Slice 2d unmapped: float Ne (F4 + D2) via CompareInverted; every other
    // float compare is mapped (single NEON instruction each, Lt/Le swapped).
    // Slice 2e unmapped: bitselect + any_true/all_true (B4/B8/B16); negate
    // maps directly (single NEON instruction each).
    // Every other opcode fails safe (controlled FailFast, never silent miscompile).
    if (instr->m_opcode == Js::OpCode::Simd128_LdC)
    {
        return Simd128LoadConst(instr);
    }
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_Splat_I4:
    case Js::OpCode::Simd128_Splat_U4:
    case Js::OpCode::Simd128_Splat_I8:
    case Js::OpCode::Simd128_Splat_U8:
    case Js::OpCode::Simd128_Splat_I16:
    case Js::OpCode::Simd128_Splat_U16:
    case Js::OpCode::Simd128_Splat_I2:
    case Js::OpCode::Simd128_Splat_F4:
    case Js::OpCode::Simd128_Splat_D2:
        return Simd128LowerSplat(instr);
    case Js::OpCode::Simd128_ExtractLane_I4:
    case Js::OpCode::Simd128_ExtractLane_U4:
    case Js::OpCode::Simd128_ExtractLane_I8:
    case Js::OpCode::Simd128_ExtractLane_U8:
    case Js::OpCode::Simd128_ExtractLane_I16:
    case Js::OpCode::Simd128_ExtractLane_U16:
    case Js::OpCode::Simd128_ExtractLane_I2:
    case Js::OpCode::Simd128_ExtractLane_F4:
    case Js::OpCode::Simd128_ExtractLane_D2:
        return Simd128LowerExtractLane(instr);
    case Js::OpCode::Simd128_ReplaceLane_I4:
    case Js::OpCode::Simd128_ReplaceLane_U4:
    case Js::OpCode::Simd128_ReplaceLane_I8:
    case Js::OpCode::Simd128_ReplaceLane_U8:
    case Js::OpCode::Simd128_ReplaceLane_I16:
    case Js::OpCode::Simd128_ReplaceLane_U16:
    case Js::OpCode::Simd128_ReplaceLane_I2:
    case Js::OpCode::Simd128_ReplaceLane_F4:
    case Js::OpCode::Simd128_ReplaceLane_D2:
        return Simd128LowerReplaceLane(instr);
    case Js::OpCode::Simd128_Shuffle_U16:
        return Simd128LowerShuffle(instr);
    case Js::OpCode::Simd128_FromFloat32x4_I4:
        return Simd128LowerInt32x4FromFloat32x4(instr);
    case Js::OpCode::Simd128_FromFloat32x4_U4:
        return Simd128LowerUint32x4FromFloat32x4(instr);
    case Js::OpCode::Simd128_FromInt32x4_F4:
        return Simd128LowerFloat32x4FromInt32x4(instr);
    case Js::OpCode::Simd128_FromUint32x4_F4:
        return Simd128LowerFloat32x4FromUint32x4(instr);
    case Js::OpCode::Simd128_Min_F4:
    case Js::OpCode::Simd128_Max_F4:
        return Simd128LowerMinMax_F4(instr);
    case Js::OpCode::Simd128_Min_D2:
    case Js::OpCode::Simd128_Max_D2:
        return Simd128LowerMinMax_D2(instr);
    case Js::OpCode::Simd128_ShLtByScalar_I4:
    case Js::OpCode::Simd128_ShLtByScalar_U4:
    case Js::OpCode::Simd128_ShRtByScalar_I4:
    case Js::OpCode::Simd128_ShRtByScalar_U4:
    case Js::OpCode::Simd128_ShLtByScalar_I8:
    case Js::OpCode::Simd128_ShLtByScalar_U8:
    case Js::OpCode::Simd128_ShRtByScalar_I8:
    case Js::OpCode::Simd128_ShRtByScalar_U8:
    case Js::OpCode::Simd128_ShLtByScalar_I16:
    case Js::OpCode::Simd128_ShLtByScalar_U16:
    case Js::OpCode::Simd128_ShRtByScalar_I16:
    case Js::OpCode::Simd128_ShRtByScalar_U16:
    case Js::OpCode::Simd128_ShLtByScalar_I2:
    case Js::OpCode::Simd128_ShRtByScalar_I2:
    case Js::OpCode::Simd128_ShRtByScalar_U2:
        return Simd128LowerShift(instr);
    default:
        break;
    }
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_LdArr_I4:
    case Js::OpCode::Simd128_LdArr_I8:
    case Js::OpCode::Simd128_LdArr_I16:
    case Js::OpCode::Simd128_LdArr_U4:
    case Js::OpCode::Simd128_LdArr_U8:
    case Js::OpCode::Simd128_LdArr_U16:
    case Js::OpCode::Simd128_LdArr_F4:
    case Js::OpCode::Simd128_LdArrConst_I4:
    case Js::OpCode::Simd128_LdArrConst_I8:
    case Js::OpCode::Simd128_LdArrConst_I16:
    case Js::OpCode::Simd128_LdArrConst_U4:
    case Js::OpCode::Simd128_LdArrConst_U8:
    case Js::OpCode::Simd128_LdArrConst_U16:
    case Js::OpCode::Simd128_LdArrConst_F4:
        Assert(m_func->GetJITFunctionBody()->IsAsmJsMode());
        return Simd128AsmJsLowerLoadElem(instr);
    case Js::OpCode::Simd128_StArr_I4:
    case Js::OpCode::Simd128_StArr_I8:
    case Js::OpCode::Simd128_StArr_I16:
    case Js::OpCode::Simd128_StArr_U4:
    case Js::OpCode::Simd128_StArr_U8:
    case Js::OpCode::Simd128_StArr_U16:
    case Js::OpCode::Simd128_StArr_F4:
    case Js::OpCode::Simd128_StArrConst_I4:
    case Js::OpCode::Simd128_StArrConst_I8:
    case Js::OpCode::Simd128_StArrConst_I16:
    case Js::OpCode::Simd128_StArrConst_U4:
    case Js::OpCode::Simd128_StArrConst_U8:
    case Js::OpCode::Simd128_StArrConst_U16:
    case Js::OpCode::Simd128_StArrConst_F4:
        Assert(m_func->GetJITFunctionBody()->IsAsmJsMode());
        return Simd128AsmJsLowerStoreElem(instr);
    case Js::OpCode::Simd128_Neq_I4:
    case Js::OpCode::Simd128_Neq_I8:
    case Js::OpCode::Simd128_Neq_I16:
    case Js::OpCode::Simd128_Neq_U4:
    case Js::OpCode::Simd128_Neq_U8:
    case Js::OpCode::Simd128_Neq_U16:
        return Simd128LowerCompareInverted(instr, Js::OpCode::CMEQ);
    // Slice 2d: float Ne is NOT(Eq), same shape as integer Ne. NEON FCMEQ
    // yields 0 on any NaN lane, so inversion yields 1 (true) exactly when
    // the wasm ne semantics require it.
    case Js::OpCode::Simd128_Neq_F4:
    case Js::OpCode::Simd128_Neq_D2:
        return Simd128LowerCompareInverted(instr, Js::OpCode::FCMEQV);
    // Slice 2e: bitselect (local mirror of the shared 3-instruction shape)
    // and any_true/all_true (canonicalize + reduce + move + shift).
    case Js::OpCode::Simd128_BitSelect_I4:
        return Simd128LowerBitSelect_NEON(instr);
    case Js::OpCode::Simd128_AnyTrue_B4:
    case Js::OpCode::Simd128_AnyTrue_B8:
    case Js::OpCode::Simd128_AnyTrue_B16:
        return Simd128LowerAnyTrue_NEON(instr, false);
    case Js::OpCode::Simd128_AllTrue_B4:
    case Js::OpCode::Simd128_AllTrue_B8:
    case Js::OpCode::Simd128_AllTrue_B16:
        return Simd128LowerAnyTrue_NEON(instr, true);
    default:
        break;
    }
    AssertOrFailFastMsg(false, "W8-NEON slices 1+2a+2b+2c+2d+2e: unlowered Simd128 opcode, refusing to miscompile");
    return nullptr;
}

IR::Instr *
LowererMD::Simd128LowerCompareInverted(IR::Instr *instr, Js::OpCode cmpOpcode)
{
    // Slice 2a: Ne(a,b) == NOT(Eq(a,b)). NEON compares already produce
    // all-ones/all-zero masks, so bitwise NOT (PMVN, vector MVN) inverts the
    // mask: no all-ones constant load like x64 Simd128LowerNotEqual.
    Assert(instr->GetDst() && instr->GetDst()->IsRegOpnd() && instr->GetDst()->IsSimd128());
    Assert(instr->GetSrc1() && instr->GetSrc1()->IsRegOpnd() && instr->GetSrc1()->IsSimd128());
    Assert(instr->GetSrc2() && instr->GetSrc2()->IsRegOpnd() && instr->GetSrc2()->IsSimd128());
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    IR::Instr *cmpInstr = IR::Instr::New(cmpOpcode, dst, src1, src2, m_func);
    instr->InsertBefore(cmpInstr);
    LegalizeMD::LegalizeInstr(cmpInstr);
    IR::Instr *notInstr = IR::Instr::New(Js::OpCode::PMVN, dst, dst, m_func);
    instr->InsertBefore(notInstr);
    LegalizeMD::LegalizeInstr(notInstr);
    IR::Instr *instrPrev = instr->m_prev;
    instr->Remove();
    return instrPrev;
}

// Slice 2e: bitselect. Local mirror of the shared
// LowererMD::LowerSimd128BitSelect shape (LowerMDSharedSimd128.cpp is not
// compiled for ARM64, so the shared helper is unavailable here):
// dst = ((src1 ^ src2) & mask) ^ src2, three bitwise instructions. PAND and
// PXOR encode fixed SIZE_16B (bitwise, lane-agnostic), so the I4 lane type
// needs no special handling.
IR::Instr *
LowererMD::Simd128LowerBitSelect_NEON(IR::Instr *instr)
{
    SList<IR::Opnd*> *args = Simd128GetExtendedArgs(instr);
    IR::Opnd *dst = args->Pop();
    IR::Opnd *src1 = args->Pop();
    IR::Opnd *src2 = args->Pop();
    IR::Opnd *mask = args->Pop();

    IR::Instr *xorInstr = IR::Instr::New(Js::OpCode::PXOR, dst, src1, src2, m_func);
    instr->InsertBefore(xorInstr);
    LegalizeMD::LegalizeInstr(xorInstr);

    IR::Instr *andInstr = IR::Instr::New(Js::OpCode::PAND, dst, dst, mask, m_func);
    instr->InsertBefore(andInstr);
    LegalizeMD::LegalizeInstr(andInstr);

    IR::Instr *xor2Instr = IR::Instr::New(Js::OpCode::PXOR, dst, dst, src2, m_func);
    instr->InsertBefore(xor2Instr);
    LegalizeMD::LegalizeInstr(xor2Instr);

    IR::Instr *instrPrev = instr->m_prev;
    instr->Remove();
    return instrPrev;
}

// Slice 2e: v128.any_true / v128.all_true for B4/B8/B16. NEON has no
// PMOVMSK-style mask extraction, so canonicalize each lane to all-ones /
// all-zero and reduce across lanes:
//   zero  = PXOR(src, src)      // all-zero vector (xor-zeroing idiom)
//   eq    = CMEQ(src, zero)     // all-ones where the lane is zero
//   canon = PMVN(eq)            // all-ones where the lane is non-zero
//   red   = UMAXV/UMINV(canon)  // any: max is all-ones iff some lane true;
//                               // all: min is all-ones iff every lane true
//   w     = UMOV(red, lane 0)   // scalar 0 or all-ones, zero-extended to W
//   dst   = LSR(w, #shift)      // top bit down: 31 (B4), 15 (B8), 7 (B16)
// All temporaries carry the matching INT lane type (I4/I8/I16), not the B
// bool type: every encoder in this chain derives its lane size from the IR
// type, and UMOVV only accepts the I/U spellings (the bits are identical to
// the B lanes, and CMEQ/PMVN/UMAXV/UMINV are lane-content-agnostic here).
IR::Instr *
LowererMD::Simd128LowerAnyTrue_NEON(IR::Instr *instr, bool isAll)
{
    Assert(instr->GetDst() && instr->GetDst()->IsRegOpnd());
    Assert(instr->GetSrc1() && instr->GetSrc1()->IsRegOpnd() && instr->GetSrc1()->IsSimd128());
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();

    IRType laneType;
    uint32 shift;
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_AnyTrue_B4:
    case Js::OpCode::Simd128_AllTrue_B4:
        laneType = TySimd128I4;
        shift = 31;
        break;
    case Js::OpCode::Simd128_AnyTrue_B8:
    case Js::OpCode::Simd128_AllTrue_B8:
        laneType = TySimd128I8;
        shift = 15;
        break;
    case Js::OpCode::Simd128_AnyTrue_B16:
    case Js::OpCode::Simd128_AllTrue_B16:
        laneType = TySimd128I16;
        shift = 7;
        break;
    default:
        AssertMsg(UNREACHED, "Simd128LowerAnyTrue_NEON: unexpected opcode");
        return nullptr;
    }

    IR::RegOpnd *zeroVec = IR::RegOpnd::New(laneType, m_func);
    IR::Instr *zeroInstr = IR::Instr::New(Js::OpCode::PXOR, zeroVec, src1, src1, m_func);
    instr->InsertBefore(zeroInstr);
    LegalizeMD::LegalizeInstr(zeroInstr);

    IR::RegOpnd *eqMask = IR::RegOpnd::New(laneType, m_func);
    IR::Instr *cmpInstr = IR::Instr::New(Js::OpCode::CMEQ, eqMask, src1, zeroVec, m_func);
    instr->InsertBefore(cmpInstr);
    LegalizeMD::LegalizeInstr(cmpInstr);

    IR::RegOpnd *canon = IR::RegOpnd::New(laneType, m_func);
    IR::Instr *notInstr = IR::Instr::New(Js::OpCode::PMVN, canon, eqMask, m_func);
    instr->InsertBefore(notInstr);
    LegalizeMD::LegalizeInstr(notInstr);

    IR::RegOpnd *reduced = IR::RegOpnd::New(laneType, m_func);
    IR::Instr *reduceInstr = IR::Instr::New(isAll ? Js::OpCode::UMINV : Js::OpCode::UMAXV, reduced, canon, m_func);
    instr->InsertBefore(reduceInstr);
    LegalizeMD::LegalizeInstr(reduceInstr);

    IR::Instr *movInstr = IR::Instr::New(Js::OpCode::UMOVV, dst, reduced,
        IR::IntConstOpnd::New(0, TyUint8, m_func, true), m_func);
    instr->InsertBefore(movInstr);
    LegalizeMD::LegalizeInstr(movInstr);

    IR::Instr *shiftInstr = IR::Instr::New(Js::OpCode::LSR, dst, dst,
        IR::IntConstOpnd::New(shift, TyUint8, m_func, true), m_func);
    instr->InsertBefore(shiftInstr);
    LegalizeMD::LegalizeInstr(shiftInstr);

    IR::Instr *instrPrev = instr->m_prev;
    instr->Remove();
    return instrPrev;
}

// Slice 2b: unpack the ExtendArg_A chain. Local equivalent of the shared x86
// helper (LowerMDSharedSimd128.cpp is not compiled for ARM64).
SList<IR::Opnd*> *
LowererMD::Simd128GetExtendedArgs(IR::Instr *instr)
{
    SList<IR::Opnd*> *args = JitAnew(m_lowerer->m_alloc, SList<IR::Opnd*>, m_lowerer->m_alloc);
    IR::Instr *pInstr = instr;
    IR::Opnd *dst = nullptr, *src1 = nullptr, *src2 = nullptr;

    if (pInstr->GetDst())
    {
        dst = pInstr->UnlinkDst();
    }

    src1 = pInstr->UnlinkSrc1();
    Assert(src1->GetStackSym()->IsSingleDef());

    pInstr = src1->GetStackSym()->GetInstrDef();

    while (pInstr && pInstr->m_opcode == Js::OpCode::ExtendArg_A)
    {
        Assert(pInstr->GetSrc1());
        src1 = pInstr->GetSrc1()->Copy(this->m_func);
        if (src1->IsRegOpnd())
        {
            this->m_lowerer->addToLiveOnBackEdgeSyms->Set(src1->AsRegOpnd()->m_sym->m_id);
        }
        args->Push(src1);

        if (pInstr->GetSrc2())
        {
            src2 = pInstr->GetSrc2();
            Assert(src2->GetStackSym()->IsSingleDef());
            pInstr = src2->GetStackSym()->GetInstrDef();
        }
        else
        {
            pInstr = nullptr;
        }
    }
    args->Push(dst);
    Assert(args->Count() > 3);
    return args;
}

// Slice 2b: materialize a 16-byte SIMD constant (replicated u32 or 16 lane
// bytes) via the function constant pool, like Simd128LoadConst.
IR::RegOpnd *
LowererMD::Simd128MaterializeConst(AsmJsSIMDValue value, IRType type, IR::Instr *insertBeforeInstr)
{
    void *pValue = NativeCodeDataNewNoFixup(this->m_func->GetNativeCodeDataAllocator(), SIMDType<DataDesc_LowererMD_Simd128LoadConst>, value);
    IR::Opnd *simdRef;
    if (!m_func->IsOOPJIT())
    {
        simdRef = IR::MemRefOpnd::New((void *)pValue, type, m_func);
    }
    else
    {
        int offset = NativeCodeData::GetDataTotalOffset(pValue);
        simdRef = IR::IndirOpnd::New(IR::RegOpnd::New(m_func->GetTopFunc()->GetNativeCodeDataSym(), TyVar, m_func), offset, type,
#if DBG
            NativeCodeData::GetDataDescription(pValue, m_func->m_alloc),
#endif
            m_func, true);
        m_lowerer->addToLiveOnBackEdgeSyms->Set(m_func->GetTopFunc()->GetNativeCodeDataSym()->m_id);
    }
    IR::RegOpnd *tmp = IR::RegOpnd::New(type, m_func);
    IR::Instr *load = IR::Instr::New(Js::OpCode::MOVQ, tmp, simdRef, m_func);
    insertBeforeInstr->InsertBefore(load);
    LegalizeMD::LegalizeInstr(load);
    return tmp;
}

// Slice 2b: trap (RangeError, like x64) if any of the 4 mask lanes is
// all-ones. Reduces via UMOVV lane extracts + scalar ORR, then CBZ skips
// over the runtime call on the all-zero (no-trap) path.
void
LowererMD::Simd128TrapIfMaskNonZero(IR::RegOpnd *mask, IR::Instr *instr)
{
    Assert(mask->IsSimd128());
    IR::RegOpnd *reduced = IR::RegOpnd::New(TyInt32, m_func);
    IR::RegOpnd *lane = IR::RegOpnd::New(TyInt32, m_func);
    for (int i = 0; i < 4; i++)
    {
        IR::Instr *extract = IR::Instr::New(Js::OpCode::UMOVV, i == 0 ? (IR::Opnd*)reduced : (IR::Opnd*)lane,
            mask, IR::IntConstOpnd::New(i, TyInt32, m_func), m_func);
        instr->InsertBefore(extract);
        LegalizeMD::LegalizeInstr(extract);
        if (i > 0)
        {
            IR::Instr *orr = IR::Instr::New(Js::OpCode::ORR, reduced, reduced, lane, m_func);
            instr->InsertBefore(orr);
            LegalizeMD::LegalizeInstr(orr);
        }
    }
    IR::LabelInstr *doneLabel = IR::LabelInstr::New(Js::OpCode::Label, m_func);
    IR::Instr *branch = IR::BranchInstr::New(Js::OpCode::CBZ, doneLabel, m_func);
    branch->SetSrc1(reduced);
    instr->InsertBefore(branch);
    m_lowerer->GenerateRuntimeError(instr, JSERR_ArgumentOutOfRange, IR::HelperOp_RuntimeRangeError);
    instr->InsertBefore(doneLabel);
}

// Slice 2b: splat one scalar to all lanes (NEON INS lane 0, then DUP-element
// lane 0 to all lanes, as two MD instrs since each encodes one machine
// word). Float scalars are bitcast via FMOV_GEN first (f64→f32 demoted via
// FCVT, mirroring x64's CVTSD2SS step); 8/16-bit truncation is inherent in
// the lane-sized INS, matching x64's truncate-then-broadcast.
IR::Instr *
LowererMD::Simd128LowerSplat(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    Assert(dst && dst->IsRegOpnd() && dst->IsSimd128());
    Assert(src1 && src1->IsRegOpnd() && !src1->IsSimd128());
    Assert(!instr->GetSrc2());

    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_Splat_F4:
        if (src1->GetType() == TyFloat64)
        {
            IR::RegOpnd *f32tmp = IR::RegOpnd::New(TyFloat32, m_func);
            IR::Instr *cvt = IR::Instr::New(Js::OpCode::FCVT, f32tmp, src1, m_func);
            instr->InsertBefore(cvt);
            LegalizeMD::LegalizeInstr(cvt);
            src1 = f32tmp;
        }
        Assert(src1->GetType() == TyFloat32);
        {
            IR::RegOpnd *bits = IR::RegOpnd::New(TyInt32, m_func);
            IR::Instr *bitcast = IR::Instr::New(Js::OpCode::FMOV_GEN, bits, src1, m_func);
            instr->InsertBefore(bitcast);
            LegalizeMD::LegalizeInstr(bitcast);
            src1 = bits;
        }
        break;
    case Js::OpCode::Simd128_Splat_D2:
        Assert(src1->GetType() == TyFloat64);
        {
            IR::RegOpnd *bits = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *bitcast = IR::Instr::New(Js::OpCode::FMOV_GEN, bits, src1, m_func);
            instr->InsertBefore(bitcast);
            LegalizeMD::LegalizeInstr(bitcast);
            src1 = bits;
        }
        break;
    case Js::OpCode::Simd128_Splat_I4:
    case Js::OpCode::Simd128_Splat_U4:
    case Js::OpCode::Simd128_Splat_I8:
    case Js::OpCode::Simd128_Splat_U8:
    case Js::OpCode::Simd128_Splat_I16:
    case Js::OpCode::Simd128_Splat_U16:
        Assert(src1->GetType() == TyInt32 || src1->GetType() == TyUint32);
        break;
    case Js::OpCode::Simd128_Splat_I2:
        Assert(src1->GetType() == TyInt64);
        break;
    default:
        AssertOrFailFastMsg(false, "W8-NEON slice 2b: unlowered splat (bool splats)");
        return nullptr;
    }

    if (src1->GetSize() == 8)
    {
        // I2/D2: single DUP-general (the 64-bit scalar selects 2D lanes;
        // the F4-typed D2 destination cannot).
        Assert(instr->m_opcode == Js::OpCode::Simd128_Splat_I2 || instr->m_opcode == Js::OpCode::Simd128_Splat_D2);
        IR::Instr *dup = IR::Instr::New(Js::OpCode::DUPGV, dst, src1, m_func);
        instr->InsertBefore(dup);
        LegalizeMD::LegalizeInstr(dup);
    }
    else
    {
        IR::Opnd *laneZero = IR::IntConstOpnd::New(0, TyInt32, m_func);
        IR::Instr *ins = IR::Instr::New(Js::OpCode::INSV, dst, laneZero, src1, m_func);
        instr->InsertBefore(ins);
        LegalizeMD::LegalizeInstr(ins);
        IR::Instr *dup = IR::Instr::New(Js::OpCode::DUPELV, dst, dst, laneZero, m_func);
        instr->InsertBefore(dup);
        LegalizeMD::LegalizeInstr(dup);
    }
    instr->Remove();
    return instrPrev;
}

// Slice 2b: one lane to a scalar register. UMOV zero-extends (unsigned and
// 32/64-bit lanes); SMOV sign-extends 8/16-bit lanes. Float lanes go through
// a general register and back via FMOV_GEN. Bool extracts stay FailFast.
IR::Instr *
LowererMD::Simd128LowerExtractLane(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    Assert(dst && dst->IsRegOpnd() && !dst->IsSimd128());
    Assert(src1 && src1->IsRegOpnd() && src1->IsSimd128());
    Assert(src2 && src2->IsIntConstOpnd());
    uint lane = src2->AsIntConstOpnd()->AsUint32();

    Js::OpCode movOpcode = Js::OpCode::UMOVV;
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_ExtractLane_I4:
    case Js::OpCode::Simd128_ExtractLane_U4:
    case Js::OpCode::Simd128_ExtractLane_F4:
        Assert(lane < 4);
        break;
    case Js::OpCode::Simd128_ExtractLane_I8:
        Assert(lane < 8);
        movOpcode = Js::OpCode::SMOVV;
        break;
    case Js::OpCode::Simd128_ExtractLane_U8:
        Assert(lane < 8);
        break;
    case Js::OpCode::Simd128_ExtractLane_I16:
        Assert(lane < 16);
        movOpcode = Js::OpCode::SMOVV;
        break;
    case Js::OpCode::Simd128_ExtractLane_U16:
        Assert(lane < 16);
        break;
    case Js::OpCode::Simd128_ExtractLane_I2:
    case Js::OpCode::Simd128_ExtractLane_D2:
        Assert(lane < 2);
        break;
    default:
        AssertOrFailFastMsg(false, "W8-NEON slice 2b: unlowered extract-lane (bool lanes)");
        return nullptr;
    }

    IR::Opnd *intDst = dst;
    IR::RegOpnd *tmp = nullptr;
    if (instr->m_opcode == Js::OpCode::Simd128_ExtractLane_F4)
    {
        tmp = IR::RegOpnd::New(TyInt32, m_func);
        intDst = tmp;
    }
    else if (instr->m_opcode == Js::OpCode::Simd128_ExtractLane_D2)
    {
        tmp = IR::RegOpnd::New(TyInt64, m_func);
        intDst = tmp;
    }
    IR::Instr *extract = IR::Instr::New(movOpcode, intDst, src1, src2, m_func);
    instr->InsertBefore(extract);
    LegalizeMD::LegalizeInstr(extract);
    if (tmp)
    {
        IR::Instr *bitcast = IR::Instr::New(Js::OpCode::FMOV_GEN, dst, tmp, m_func);
        instr->InsertBefore(bitcast);
        LegalizeMD::LegalizeInstr(bitcast);
    }
    instr->Remove();
    return instrPrev;
}

// Slice 2b: scalar into one lane. The untouched lanes preserve (copy first,
// then INS), matching x64. Float scalars are bitcast via FMOV_GEN.
IR::Instr *
LowererMD::Simd128LowerReplaceLane(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    SList<IR::Opnd*> *args = Simd128GetExtendedArgs(instr);
    IR::Opnd *dst = args->Pop();
    IR::Opnd *src1 = args->Pop();
    IR::Opnd *laneOpnd = args->Pop();
    IR::Opnd *scalarOpnd = args->Pop();
    Assert(dst->IsSimd128() && src1->IsSimd128());
    Assert(laneOpnd->IsIntConstOpnd());
    uint lane = laneOpnd->AsIntConstOpnd()->AsUint32();

    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_ReplaceLane_I4:
    case Js::OpCode::Simd128_ReplaceLane_U4:
    case Js::OpCode::Simd128_ReplaceLane_F4:
        Assert(lane < 4);
        break;
    case Js::OpCode::Simd128_ReplaceLane_I8:
    case Js::OpCode::Simd128_ReplaceLane_U8:
        Assert(lane < 8);
        break;
    case Js::OpCode::Simd128_ReplaceLane_I16:
    case Js::OpCode::Simd128_ReplaceLane_U16:
        Assert(lane < 16);
        break;
    case Js::OpCode::Simd128_ReplaceLane_I2:
    case Js::OpCode::Simd128_ReplaceLane_D2:
        Assert(lane < 2);
        break;
    default:
        AssertOrFailFastMsg(false, "W8-NEON slice 2b: unlowered replace-lane (bool lanes)");
        return nullptr;
    }

    IR::Instr *copy = IR::Instr::New(Js::OpCode::MOVQ, dst, src1, m_func);
    instr->InsertBefore(copy);
    LegalizeMD::LegalizeInstr(copy);

    IR::Opnd *scalar = scalarOpnd;
    if (scalar->IsFloat())
    {
        IR::RegOpnd *bits = IR::RegOpnd::New(scalar->GetSize() == 8 ? TyInt64 : TyInt32, m_func);
        IR::Instr *bitcast = IR::Instr::New(Js::OpCode::FMOV_GEN, bits, scalar, m_func);
        instr->InsertBefore(bitcast);
        LegalizeMD::LegalizeInstr(bitcast);
        scalar = bits;
    }
    else if (!scalar->IsRegOpnd())
    {
        IR::RegOpnd *tmp = IR::RegOpnd::New(scalar->GetType(), m_func);
        m_lowerer->InsertMove(tmp, scalar, instr);
        scalar = tmp;
    }
    IR::Instr *insert = IR::Instr::New(Js::OpCode::INSV, dst, laneOpnd, scalar, m_func);
    instr->InsertBefore(insert);
    LegalizeMD::LegalizeInstr(insert);
    instr->Remove();
    return instrPrev;
}

// Slice 2b: i8x16.shuffle. Two single-table TBLVs (the second table is src2
// indexed by lane-16) ORed together. TBL writes 0 for out-of-range indices,
// so lanes >= 16 vanish from the first table and lanes < 16 vanish from the
// second (lane-16 wraps to >= 0xF0); lanes >= 32 vanish from both. Lanes
// 16-31 select src2 bytes, exactly like the interpreter.
IR::Instr *
LowererMD::Simd128LowerShuffle(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    SList<IR::Opnd*> *args = Simd128GetExtendedArgs(instr);
    Assert(args->Count() == 19);
    IR::Opnd *dst = args->Pop();
    IR::Opnd *src1 = args->Pop();
    IR::Opnd *src2 = args->Pop();
    Assert(dst->IsSimd128() && src1->IsSimd128() && src2->IsSimd128());

    AsmJsSIMDValue mask1 = {};
    AsmJsSIMDValue mask2 = {};
    for (uint i = 0; i < 16; i++)
    {
        IR::Opnd *laneOpnd = args->Pop();
        Assert(laneOpnd->IsIntConstOpnd());
        uint lane = laneOpnd->AsIntConstOpnd()->AsUint32();
        mask1.u8[i] = (uint8)lane;
        mask2.u8[i] = (uint8)(lane - 16);
    }
    IR::RegOpnd *indices1 = Simd128MaterializeConst(mask1, TySimd128U16, instr);
    IR::RegOpnd *indices2 = Simd128MaterializeConst(mask2, TySimd128U16, instr);
    IR::RegOpnd *sel1 = IR::RegOpnd::New(TySimd128U16, m_func);
    IR::Instr *tbl1 = IR::Instr::New(Js::OpCode::TBLV, sel1, src1, indices1, m_func);
    instr->InsertBefore(tbl1);
    LegalizeMD::LegalizeInstr(tbl1);
    IR::RegOpnd *sel2 = IR::RegOpnd::New(TySimd128U16, m_func);
    IR::Instr *tbl2 = IR::Instr::New(Js::OpCode::TBLV, sel2, src2, indices2, m_func);
    instr->InsertBefore(tbl2);
    LegalizeMD::LegalizeInstr(tbl2);
    IR::Instr *orr = IR::Instr::New(Js::OpCode::POR, dst, sel1, sel2, m_func);
    instr->InsertBefore(orr);
    LegalizeMD::LegalizeInstr(orr);
    instr->Remove();
    return instrPrev;
}

// Slice 2b: i32x4.trunc_sat_f32x4_s. FCVTZS truncates toward zero but
// saturates (NaN->0, OOB->clamp) where x64 CVTTPS2DQ yields INT_MIN, so the
// x64 range checks are reproduced explicitly: NaN traps (wasm only, like
// x64; asm.js NaN is fixed up to INT_MIN instead), lanes >= 2^31 or
// < -2^31 trap. Result lanes are bit-identical to x64 in every case.
IR::Instr *
LowererMD::Simd128LowerInt32x4FromFloat32x4(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src = instr->GetSrc1();
    Assert(dst && dst->IsSimd128() && src && src->IsSimd128());

    IR::Instr *convert = IR::Instr::New(Js::OpCode::FCVTZSV, dst, src, m_func);
    instr->InsertBefore(convert);
    LegalizeMD::LegalizeInstr(convert);

    IR::RegOpnd *nanMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpSelf = IR::Instr::New(Js::OpCode::FCMEQV, nanMask, src, src, m_func);
    instr->InsertBefore(cmpSelf);
    LegalizeMD::LegalizeInstr(cmpSelf);
    if (m_func->GetJITFunctionBody()->IsWasmFunction())
    {
        IR::Instr *notNan = IR::Instr::New(Js::OpCode::PMVN, nanMask, nanMask, m_func);
        instr->InsertBefore(notNan);
        LegalizeMD::LegalizeInstr(notNan);
        Simd128TrapIfMaskNonZero(nanMask, instr);
    }
    else
    {
        // asm.js: CVTTPS2DQ yields INT_MIN for NaN; reproduce with an OR.
        AsmJsSIMDValue intMin = {};
        intMin.i32[0] = intMin.i32[1] = intMin.i32[2] = intMin.i32[3] = (int32)0x80000000;
        IR::RegOpnd *intMinVec = Simd128MaterializeConst(intMin, TySimd128I4, instr);
        IR::RegOpnd *nanBits = IR::RegOpnd::New(TySimd128I4, m_func);
        IR::Instr *notNan = IR::Instr::New(Js::OpCode::PMVN, nanBits, nanMask, m_func);
        instr->InsertBefore(notNan);
        LegalizeMD::LegalizeInstr(notNan);
        IR::Instr *andOp = IR::Instr::New(Js::OpCode::PAND, nanBits, nanBits, intMinVec, m_func);
        instr->InsertBefore(andOp);
        LegalizeMD::LegalizeInstr(andOp);
        IR::Instr *orOp = IR::Instr::New(Js::OpCode::POR, dst, dst, nanBits, m_func);
        instr->InsertBefore(orOp);
        LegalizeMD::LegalizeInstr(orOp);
    }

    AsmJsSIMDValue two31 = {};
    two31.i32[0] = two31.i32[1] = two31.i32[2] = two31.i32[3] = (int32)0x4F000000;
    IR::RegOpnd *two31Vec = Simd128MaterializeConst(two31, TySimd128F4, instr);
    IR::RegOpnd *geMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpGe = IR::Instr::New(Js::OpCode::FCMGEV, geMask, src, two31Vec, m_func);
    instr->InsertBefore(cmpGe);
    LegalizeMD::LegalizeInstr(cmpGe);

    AsmJsSIMDValue negTwo31 = {};
    negTwo31.i32[0] = negTwo31.i32[1] = negTwo31.i32[2] = negTwo31.i32[3] = (int32)0xCF000000;
    IR::RegOpnd *negTwo31Vec = Simd128MaterializeConst(negTwo31, TySimd128F4, instr);
    IR::RegOpnd *ltMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpGt = IR::Instr::New(Js::OpCode::FCMGTV, ltMask, negTwo31Vec, src, m_func);
    instr->InsertBefore(cmpGt);
    LegalizeMD::LegalizeInstr(cmpGt);

    IR::RegOpnd *oobMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *orMasks = IR::Instr::New(Js::OpCode::POR, oobMask, geMask, ltMask, m_func);
    instr->InsertBefore(orMasks);
    LegalizeMD::LegalizeInstr(orMasks);
    Simd128TrapIfMaskNonZero(oobMask, instr);

    instr->Remove();
    return instrPrev;
}

// Slice 2b: i32x4.trunc_sat_f32x4_u. Mirrors the x64 sequence (NaN trap,
// lanes <= -1 trap, subtract 2^31 from lanes >= 2^31, truncate, INT_MIN
// check catches originals >= 2^32, add 2^31 back), with NEON compares. The
// only structural change: the >= 2^32 check compares the adjusted float
// against 2^31 (FCVTZS saturates to INT_MAX where CVTTPS2DQ marks INT_MIN),
// which traps exactly the same lanes as x64.
IR::Instr *
LowererMD::Simd128LowerUint32x4FromFloat32x4(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src = instr->GetSrc1();
    Assert(dst && dst->IsSimd128() && src && src->IsSimd128());

    // NaN traps (x64 traps via its explicit check on wasm and via the
    // INT_MIN marker on asm.js; trap unconditionally here).
    IR::RegOpnd *nanMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpSelf = IR::Instr::New(Js::OpCode::FCMEQV, nanMask, src, src, m_func);
    instr->InsertBefore(cmpSelf);
    LegalizeMD::LegalizeInstr(cmpSelf);
    IR::Instr *notNan = IR::Instr::New(Js::OpCode::PMVN, nanMask, nanMask, m_func);
    instr->InsertBefore(notNan);
    LegalizeMD::LegalizeInstr(notNan);
    Simd128TrapIfMaskNonZero(nanMask, instr);

    // Lanes <= -1 trap: saturating-convert then trap on negative results.
    // (Covers exactly the x64 CMPLEPS(src, -1.0) lanes; (-1, 0) converts to
    // 0 and passes on both.)
    IR::RegOpnd *signCheck = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *signConvert = IR::Instr::New(Js::OpCode::FCVTZSV, signCheck, src, m_func);
    instr->InsertBefore(signConvert);
    LegalizeMD::LegalizeInstr(signConvert);
    // Zero from the constant pool (a PXOR zero-idiom with one reg three
    // times over does not complete the backend; use distinct operands).
    AsmJsSIMDValue zeroBits = {};
    zeroBits.i32[0] = zeroBits.i32[1] = zeroBits.i32[2] = zeroBits.i32[3] = 0;
    IR::RegOpnd *zero = Simd128MaterializeConst(zeroBits, TySimd128I4, instr);
    IR::RegOpnd *negMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpGeZero = IR::Instr::New(Js::OpCode::CMGE, negMask, signCheck, zero, m_func);
    instr->InsertBefore(cmpGeZero);
    LegalizeMD::LegalizeInstr(cmpGeZero);
    IR::Instr *notNeg = IR::Instr::New(Js::OpCode::PMVN, negMask, negMask, m_func);
    instr->InsertBefore(notNeg);
    LegalizeMD::LegalizeInstr(notNeg);
    Simd128TrapIfMaskNonZero(negMask, instr);

    AsmJsSIMDValue two31f = {};
    two31f.i32[0] = two31f.i32[1] = two31f.i32[2] = two31f.i32[3] = (int32)0x4F000000;
    IR::RegOpnd *two31fVec = Simd128MaterializeConst(two31f, TySimd128F4, instr);
    IR::RegOpnd *geMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpGe = IR::Instr::New(Js::OpCode::FCMGEV, geMask, src, two31fVec, m_func);
    instr->InsertBefore(cmpGe);
    LegalizeMD::LegalizeInstr(cmpGe);

    IR::RegOpnd *sub = IR::RegOpnd::New(TySimd128F4, m_func);
    IR::Instr *andSub = IR::Instr::New(Js::OpCode::PAND, sub, two31fVec, geMask, m_func);
    instr->InsertBefore(andSub);
    LegalizeMD::LegalizeInstr(andSub);
    IR::RegOpnd *adjusted = IR::RegOpnd::New(TySimd128F4, m_func);
    IR::Instr *subInstr = IR::Instr::New(Js::OpCode::SUBPS, adjusted, src, sub, m_func);
    instr->InsertBefore(subInstr);
    LegalizeMD::LegalizeInstr(subInstr);

    IR::Instr *convert = IR::Instr::New(Js::OpCode::FCVTZUV, dst, adjusted, m_func);
    instr->InsertBefore(convert);
    LegalizeMD::LegalizeInstr(convert);

    // Adjusted lanes still >= 2^31 were >= 2^32 originally: trap.
    IR::RegOpnd *oobMask = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpOob = IR::Instr::New(Js::OpCode::FCMGEV, oobMask, adjusted, two31fVec, m_func);
    instr->InsertBefore(cmpOob);
    LegalizeMD::LegalizeInstr(cmpOob);
    Simd128TrapIfMaskNonZero(oobMask, instr);

    // Add 2^31 back to adjusted lanes: (2^31 & ge) added to dst.
    AsmJsSIMDValue two31i = {};
    two31i.i32[0] = two31i.i32[1] = two31i.i32[2] = two31i.i32[3] = (int32)0x80000000;
    IR::RegOpnd *two31iVec = Simd128MaterializeConst(two31i, TySimd128I4, instr);
    IR::RegOpnd *addBack = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *andAdd = IR::Instr::New(Js::OpCode::PAND, addBack, two31iVec, geMask, m_func);
    instr->InsertBefore(andAdd);
    LegalizeMD::LegalizeInstr(andAdd);
    IR::Instr *addInstr = IR::Instr::New(Js::OpCode::PADDD, dst, dst, addBack, m_func);
    instr->InsertBefore(addInstr);
    LegalizeMD::LegalizeInstr(addInstr);

    instr->Remove();
    return instrPrev;
}

// Slice 2b: f32x4.convert_i32x4_s. SCVTF is correctly-rounded (round to
// nearest), exactly matching x64 CVTDQ2PS lane for lane.
IR::Instr *
LowererMD::Simd128LowerFloat32x4FromInt32x4(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src = instr->GetSrc1();
    Assert(dst && dst->IsSimd128() && src && src->IsSimd128());
    IR::Instr *convert = IR::Instr::New(Js::OpCode::SCVTFV, dst, src, m_func);
    instr->InsertBefore(convert);
    LegalizeMD::LegalizeInstr(convert);
    instr->Remove();
    return instrPrev;
}

// Slice 2b: f32x4.convert_i32x4_u. UCVTF handles the full u32 range natively
// with round-to-nearest, exactly matching x64's CVTDQ2PS-plus-2^32-fixup.
IR::Instr *
LowererMD::Simd128LowerFloat32x4FromUint32x4(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src = instr->GetSrc1();
    Assert(dst && dst->IsSimd128() && src && src->IsSimd128());
    IR::Instr *convert = IR::Instr::New(Js::OpCode::UCVTFV, dst, src, m_func);
    instr->InsertBefore(convert);
    LegalizeMD::LegalizeInstr(convert);
    instr->Remove();
    return instrPrev;
}

// Slice 2c: variable-count shifts. The wasm count is a scalar i32 masked to
// the lane width (count & (laneBits - 1)), matching the interpreter and x64;
// negative counts wrap per C++ & semantics. The masked count is splatted to
// a vector (INS lane 0 + DUP-element, like splat; UBFX + DUP-general for
// 64-bit lanes, since a 32-bit GPR cannot feed a 64-bit INS lane) and the
// shift runs per lane: SSHL for left and arithmetic-right (a negative shift
// vector element shifts right), USHL for logical-right. Lane size comes from
// the vector IR type. Unlike x64, which helpers the i64x2 arithmetic-right
// shift for lack of a 64-bit PSRAD, NEON SSHL handles 2D lanes natively, so
// all 15 shift opcodes lower inline with no helper.
IR::Instr *
LowererMD::Simd128LowerShift(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    Assert(dst && dst->IsRegOpnd() && dst->IsSimd128());
    Assert(src1 && src1->IsRegOpnd() && src1->IsSimd128());
    Assert(src2);

    uint32 countMask = 0;
    bool isRight = false;
    bool isUnsignedRight = false;
    bool is64Bit = false;
    switch (instr->m_opcode)
    {
    case Js::OpCode::Simd128_ShLtByScalar_I4:
    case Js::OpCode::Simd128_ShLtByScalar_U4:
        countMask = 31;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_I4:
        countMask = 31;
        isRight = true;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_U4:
        countMask = 31;
        isRight = true;
        isUnsignedRight = true;
        break;
    case Js::OpCode::Simd128_ShLtByScalar_I8:
    case Js::OpCode::Simd128_ShLtByScalar_U8:
        countMask = 15;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_I8:
        countMask = 15;
        isRight = true;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_U8:
        countMask = 15;
        isRight = true;
        isUnsignedRight = true;
        break;
    case Js::OpCode::Simd128_ShLtByScalar_I16:
    case Js::OpCode::Simd128_ShLtByScalar_U16:
        countMask = 7;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_I16:
        countMask = 7;
        isRight = true;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_U16:
        countMask = 7;
        isRight = true;
        isUnsignedRight = true;
        break;
    case Js::OpCode::Simd128_ShLtByScalar_I2:
        countMask = 63;
        is64Bit = true;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_I2:
        countMask = 63;
        isRight = true;
        is64Bit = true;
        break;
    case Js::OpCode::Simd128_ShRtByScalar_U2:
        countMask = 63;
        isRight = true;
        isUnsignedRight = true;
        is64Bit = true;
        break;
    default:
        AssertOrFailFastMsg(false, "W8-NEON slice 2c: unlowered shift opcode");
        return nullptr;
    }

    IR::RegOpnd *countReg;
    if (src2->IsRegOpnd())
    {
        countReg = src2->AsRegOpnd();
    }
    else
    {
        countReg = IR::RegOpnd::New(TyInt32, m_func);
        Lowerer::InsertMove(countReg, src2, instr);
    }

    IR::RegOpnd *masked = IR::RegOpnd::New(TyInt32, m_func);
    IR::Instr *maskInstr = IR::Instr::New(Js::OpCode::AND, masked, countReg,
        IR::IntConstOpnd::New(countMask, TyInt32, m_func), m_func);
    instr->InsertBefore(maskInstr);
    LegalizeMD::LegalizeInstr(maskInstr);

    // The shift-vector element is signed: negate the masked count for right
    // shifts (0 - count), so SSHL/USHL shift right by the masked amount.
    IR::RegOpnd *shiftScalar32 = masked;
    IR::RegOpnd *shiftScalar64 = nullptr;
    if (isRight)
    {
        if (is64Bit)
        {
            IR::RegOpnd *wide = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *widen = IR::Instr::New(Js::OpCode::UBFX, wide, masked,
                IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func), m_func);
            instr->InsertBefore(widen);
            LegalizeMD::LegalizeInstr(widen);
            shiftScalar64 = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *neg = IR::Instr::New(Js::OpCode::SUB, shiftScalar64,
                IR::RegOpnd::New(nullptr, RegZR, TyInt64, m_func), wide, m_func);
            instr->InsertBefore(neg);
            LegalizeMD::LegalizeInstr(neg);
        }
        else
        {
            IR::RegOpnd *neg = IR::RegOpnd::New(TyInt32, m_func);
            IR::Instr *negInstr = IR::Instr::New(Js::OpCode::SUB, neg,
                IR::RegOpnd::New(nullptr, RegZR, TyInt32, m_func), masked, m_func);
            instr->InsertBefore(negInstr);
            LegalizeMD::LegalizeInstr(negInstr);
            shiftScalar32 = neg;
        }
    }
    else if (is64Bit)
    {
        shiftScalar64 = IR::RegOpnd::New(TyInt64, m_func);
        IR::Instr *widen = IR::Instr::New(Js::OpCode::UBFX, shiftScalar64, masked,
            IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func), m_func);
        instr->InsertBefore(widen);
        LegalizeMD::LegalizeInstr(widen);
    }

    IR::RegOpnd *countVec = IR::RegOpnd::New(src1->GetType(), m_func);
    if (is64Bit)
    {
        Assert(shiftScalar64);
        IR::Instr *dup = IR::Instr::New(Js::OpCode::DUPGV, countVec, shiftScalar64, m_func);
        instr->InsertBefore(dup);
        LegalizeMD::LegalizeInstr(dup);
    }
    else
    {
        IR::Opnd *laneZero = IR::IntConstOpnd::New(0, TyInt32, m_func);
        IR::Instr *ins = IR::Instr::New(Js::OpCode::INSV, countVec, laneZero, shiftScalar32, m_func);
        instr->InsertBefore(ins);
        LegalizeMD::LegalizeInstr(ins);
        IR::Instr *dup = IR::Instr::New(Js::OpCode::DUPELV, countVec, countVec, laneZero, m_func);
        instr->InsertBefore(dup);
        LegalizeMD::LegalizeInstr(dup);
    }

    Js::OpCode shiftOpcode = isUnsignedRight ? Js::OpCode::USHLV : Js::OpCode::SSHLV;
    IR::Instr *shift = IR::Instr::New(shiftOpcode, dst, src1, countVec, m_func);
    instr->InsertBefore(shift);
    LegalizeMD::LegalizeInstr(shift);
    instr->Remove();
    return instrPrev;
}

// Slice 2c: f32x4.min/max. The interpreter propagates NaN lanes bit-for-bit
// (the first NaN operand wins, payload preserved) and folds -0/+0 to -0
// (min) or +0 (max), while NEON FMIN/FMAX produce a default NaN whenever
// either lane is NaN. So the lowerer computes the numeric min/max
// (FMINNMV/FMAXNMV, which agree with the interpreter on all non-NaN lanes)
// and blends the original NaN lanes back explicitly:
//   result = (a & aNaN) | (b & bNaN & ~aNaN) | (numeric & ~aNaN & ~bNaN).
// FCMEQV yields all-ones exactly where a lane is NOT NaN (sNaN compares
// false too, and cannot trap: FP traps stay off), so PMVN gives the NaN
// masks. PAND/POR are bitwise and preserve payloads and sNaN bits, unlike
// a bare FMIN/FMAX. (x64's MINPS/MAXPS sequences differ from the
// interpreter when both lanes are NaN with different payloads; this
// sequence matches the interpreter in every case.)
IR::Instr *
LowererMD::Simd128LowerMinMax_F4(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    Assert(dst && dst->IsRegOpnd() && dst->IsSimd128());
    Assert(src1 && src1->IsRegOpnd() && src1->IsSimd128());
    Assert(src2 && src2->IsRegOpnd() && src2->IsSimd128());
    Assert(instr->m_opcode == Js::OpCode::Simd128_Min_F4 || instr->m_opcode == Js::OpCode::Simd128_Max_F4);

    Js::OpCode minmaxOpcode = (instr->m_opcode == Js::OpCode::Simd128_Min_F4) ? Js::OpCode::FMINNMV : Js::OpCode::FMAXNMV;
    IR::RegOpnd *numeric = IR::RegOpnd::New(TySimd128F4, m_func);
    IR::Instr *minmax = IR::Instr::New(minmaxOpcode, numeric, src1, src2, m_func);
    instr->InsertBefore(minmax);
    LegalizeMD::LegalizeInstr(minmax);

    IR::RegOpnd *eqA = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpA = IR::Instr::New(Js::OpCode::FCMEQV, eqA, src1, src1, m_func);
    instr->InsertBefore(cmpA);
    LegalizeMD::LegalizeInstr(cmpA);
    IR::RegOpnd *nanA = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *notA = IR::Instr::New(Js::OpCode::PMVN, nanA, eqA, m_func);
    instr->InsertBefore(notA);
    LegalizeMD::LegalizeInstr(notA);

    IR::RegOpnd *eqB = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *cmpB = IR::Instr::New(Js::OpCode::FCMEQV, eqB, src2, src2, m_func);
    instr->InsertBefore(cmpB);
    LegalizeMD::LegalizeInstr(cmpB);
    IR::RegOpnd *nanB = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *notB = IR::Instr::New(Js::OpCode::PMVN, nanB, eqB, m_func);
    instr->InsertBefore(notB);
    LegalizeMD::LegalizeInstr(notB);

    IR::RegOpnd *bOnly = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *bOnlyInstr = IR::Instr::New(Js::OpCode::PAND, bOnly, nanB, eqA, m_func);
    instr->InsertBefore(bOnlyInstr);
    LegalizeMD::LegalizeInstr(bOnlyInstr);
    IR::RegOpnd *neither = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *neitherInstr = IR::Instr::New(Js::OpCode::PAND, neither, eqA, eqB, m_func);
    instr->InsertBefore(neitherInstr);
    LegalizeMD::LegalizeInstr(neitherInstr);

    IR::RegOpnd *t1 = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *t1Instr = IR::Instr::New(Js::OpCode::PAND, t1, src1, nanA, m_func);
    instr->InsertBefore(t1Instr);
    LegalizeMD::LegalizeInstr(t1Instr);
    IR::RegOpnd *t2 = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *t2Instr = IR::Instr::New(Js::OpCode::PAND, t2, src2, bOnly, m_func);
    instr->InsertBefore(t2Instr);
    LegalizeMD::LegalizeInstr(t2Instr);
    IR::RegOpnd *t3 = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *t3Instr = IR::Instr::New(Js::OpCode::PAND, t3, numeric, neither, m_func);
    instr->InsertBefore(t3Instr);
    LegalizeMD::LegalizeInstr(t3Instr);

    IR::RegOpnd *t12 = IR::RegOpnd::New(TySimd128I4, m_func);
    IR::Instr *or12 = IR::Instr::New(Js::OpCode::POR, t12, t1, t2, m_func);
    instr->InsertBefore(or12);
    LegalizeMD::LegalizeInstr(or12);
    IR::Instr *blend = IR::Instr::New(Js::OpCode::POR, dst, t12, t3, m_func);
    instr->InsertBefore(blend);
    LegalizeMD::LegalizeInstr(blend);

    instr->Remove();
    return instrPrev;
}

// Slice 2c: f64x2.min/max. The interpreter implements the C++ ternary
// (a<b)?a:b / (a>b)?a:b per lane, so any NaN comparison is false and the
// second operand wins bit-for-bit (payload preserved), and ties (-0/+0,
// equal values) also take the second operand. The lowerer mirrors that
// exactly: mask=(b>a) for min [(a>b) for max], result=(a&mask)|(b&~mask).
// FCMGTV yields all-ones on true and all-zero on false-or-NaN (no trap),
// so the blend selects original lane bits in every case, including NaN
// payloads and sNaN. (x64 maps these to MINPD/MAXPD, whose
// second-operand-wins NaN semantics coincide with the ternary.)
IR::Instr *
LowererMD::Simd128LowerMinMax_D2(IR::Instr *instr)
{
    IR::Instr *instrPrev = instr->m_prev;
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    Assert(dst && dst->IsRegOpnd() && dst->IsSimd128());
    Assert(src1 && src1->IsRegOpnd() && src1->IsSimd128());
    Assert(src2 && src2->IsRegOpnd() && src2->IsSimd128());
    Assert(instr->m_opcode == Js::OpCode::Simd128_Min_D2 || instr->m_opcode == Js::OpCode::Simd128_Max_D2);

    IR::Opnd *cmpSrc1 = (instr->m_opcode == Js::OpCode::Simd128_Min_D2) ? src2 : src1;
    IR::Opnd *cmpSrc2 = (instr->m_opcode == Js::OpCode::Simd128_Min_D2) ? src1 : src2;
    IR::RegOpnd *mask = IR::RegOpnd::New(TySimd128I2, m_func);
    IR::Instr *cmp = IR::Instr::New(Js::OpCode::FCMGTV, mask, cmpSrc1, cmpSrc2, m_func);
    instr->InsertBefore(cmp);
    LegalizeMD::LegalizeInstr(cmp);
    IR::RegOpnd *nmask = IR::RegOpnd::New(TySimd128I2, m_func);
    IR::Instr *notMask = IR::Instr::New(Js::OpCode::PMVN, nmask, mask, m_func);
    instr->InsertBefore(notMask);
    LegalizeMD::LegalizeInstr(notMask);

    IR::RegOpnd *t1 = IR::RegOpnd::New(TySimd128I2, m_func);
    IR::Instr *t1Instr = IR::Instr::New(Js::OpCode::PAND, t1, src1, mask, m_func);
    instr->InsertBefore(t1Instr);
    LegalizeMD::LegalizeInstr(t1Instr);
    IR::RegOpnd *t2 = IR::RegOpnd::New(TySimd128I2, m_func);
    IR::Instr *t2Instr = IR::Instr::New(Js::OpCode::PAND, t2, src2, nmask, m_func);
    instr->InsertBefore(t2Instr);
    LegalizeMD::LegalizeInstr(t2Instr);
    IR::Instr *blend = IR::Instr::New(Js::OpCode::POR, dst, t1, t2, m_func);
    instr->InsertBefore(blend);
    LegalizeMD::LegalizeInstr(blend);

    instr->Remove();
    return instrPrev;
}

IR::Instr *
LowererMD::Simd128ConvertToLoad(IR::Opnd * dst, IR::Opnd * src, IR::Instr * instr)
{
    // Slice 1 supports only full 16-byte loads (dataWidth 16); partial loads
    // (12/8/4) need MOVSD/MOVSS sequences that are not part of this slice.
    Assert(instr->dataWidth == 16);
    IR::Instr * instrPrev = instr->m_prev;
    IR::Instr * newInstr = IR::Instr::New(GetAssignOp(src->GetType()), dst, src, instr->m_func);
    instr->InsertBefore(newInstr);
    LegalizeMD::LegalizeInstr(newInstr);
    instr->Remove();
    return instrPrev;
}

IR::Instr *
LowererMD::Simd128ConvertToStore(IR::Opnd * dst, IR::Opnd * src, IR::Instr * instr)
{
    Assert(instr->dataWidth == 16);
    IR::Instr * instrPrev = instr->m_prev;
    IR::Instr * newInstr = IR::Instr::New(GetAssignOp(dst->GetType()), dst, src, instr->m_func);
    // GetAssignOp returns MOVQ (dst mem, src reg) for stores.
    newInstr->m_opcode = GetAssignOp(dst->GetType());
    // For stores the IR shape is (dst mem, src reg); MOVQ handles mem dst.
    // Rebuild in (dst,src) order for the encoder store path.
    instr->InsertBefore(newInstr);
    LegalizeMD::LegalizeInstr(newInstr);
    instr->Remove();
    return instrPrev;
}

IR::Instr *
LowererMD::Simd128AsmJsLowerLoadElem(IR::Instr * instr)
{
    IR::Instr * instrPrev = instr->m_prev;
    IR::Opnd * dst = instr->GetDst();
    IR::Opnd * src1 = instr->GetSrc1();
    Assert(dst && dst->IsSimd128() && src1->IsSimd128() && src1->IsIndirOpnd());
    // Bounds: trap iff effectiveEnd = index + offset + 16 > length (the
    // access is 16 bytes; comparing index >= length misses the [len-16,
    // len) window). Mirrors LowerWasmArrayBoundsCheck's 64-bit sum shape;
    // SIMD OOB is a trap like x64's isSimdLoad path.
    IR::IndirOpnd *indirOpnd = src1->AsIndirOpnd();
    IR::RegOpnd *indexOpnd = indirOpnd->GetIndexOpnd();
    int32 offset = indirOpnd->GetOffset();
#if ENABLE_FAST_ARRAYBUFFER
    const bool isWasmFast = CONFIG_FLAG(WasmFastArray) && m_func->GetJITFunctionBody()->IsWasmFunction();
#else
    const bool isWasmFast = false;
#endif
    // The ARM64 encoder emits a single DWORD per instruction, so a
    // base+index MOVQ would lose its address word. Materialize
    // EA = base + index in IR (zero-extended) and use the direct form.
    IR::RegOpnd *extIndex = nullptr;
    if (!isWasmFast)
    {
        IR::LabelInstr * helperLabel = Lowerer::InsertLabel(true, instr);
        IR::LabelInstr * loadLabel = Lowerer::InsertLabel(false, instr);
        IR::LabelInstr * doneLabel = Lowerer::InsertLabel(false, instr);
        IR::Int64ConstOpnd *constEndOff =
            IR::Int64ConstOpnd::New((int64)16 + (int64)offset, TyInt64, m_func);
        IR::Opnd *cmpOpnd;
        if (indexOpnd != nullptr)
        {
            // Wasm addresses are 32-bit unsigned: zero-extend (same
            // rationale as LowerWasmArrayBoundsCheck).
            extIndex = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *ubfx = IR::Instr::New(Js::OpCode::UBFX, extIndex,
                indexOpnd->UseWithNewType(TyInt32, m_func),
                IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func, true), m_func);
            helperLabel->InsertBefore(ubfx);
            LegalizeMD::LegalizeInstr(ubfx);
            cmpOpnd = IR::RegOpnd::New(TyInt64, m_func);
            Lowerer::InsertAdd(true, cmpOpnd, extIndex, constEndOff, helperLabel);
        }
        else
        {
            cmpOpnd = constEndOff;
        }
        m_lowerer->InsertCompareBranch(cmpOpnd, instr->UnlinkSrc2(), Js::OpCode::BrGt_A, true, helperLabel, helperLabel);
        Lowerer::InsertBranch(Js::OpCode::Br, loadLabel, helperLabel);
        // SIMD OOB load is a trap (mirrors x64 LowerAsmJsLdElemHelper
        // isSimdLoad). Insert at loadLabel so the call lands at the end of
        // the helper block, before the in-bounds load.
        m_lowerer->GenerateRuntimeError(loadLabel, JSERR_ArgumentOutOfRange, IR::HelperOp_RuntimeRangeError);
        Lowerer::InsertBranch(Js::OpCode::Br, doneLabel, loadLabel);
        (void)doneLabel;
    }
    if (indexOpnd != nullptr)
    {
        if (extIndex == nullptr)
        {
            extIndex = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *ubfx = IR::Instr::New(Js::OpCode::UBFX, extIndex,
                indexOpnd->UseWithNewType(TyInt32, m_func),
                IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func, true), m_func);
            instr->InsertBefore(ubfx);
            LegalizeMD::LegalizeInstr(ubfx);
        }
        IR::RegOpnd *eaBase = IR::RegOpnd::New(TyMachPtr, m_func);
        IR::Instr *eaAdd = IR::Instr::New(Js::OpCode::Add_A, eaBase,
            indirOpnd->GetBaseOpnd(),
            extIndex->UseWithNewType(TyMachPtr, m_func), m_func);
        LowererMD::ChangeToAdd(eaAdd, false);
        instr->InsertBefore(eaAdd);
        LegalizeMD::LegalizeInstr(eaAdd);
        indirOpnd->ReplaceBaseOpnd(eaBase);
        indirOpnd->SetIndexOpnd(nullptr);
    }
    return Simd128ConvertToLoad(dst, src1, instr);
}

IR::Instr *
LowererMD::Simd128AsmJsLowerStoreElem(IR::Instr * instr)
{
    IR::Instr * instrPrev = instr->m_prev;
    IR::Opnd * dst = instr->GetDst();
    IR::Opnd * src1 = instr->GetSrc1();
    Assert(dst && dst->IsIndirOpnd() && dst->IsSimd128());
    Assert(src1 && src1->IsSimd128() && src1->IsRegOpnd());
    // Bounds: trap iff effectiveEnd = index + offset + 16 > length (see the
    // load path above for the window rationale). The encoder emits a single
    // DWORD per instruction, so materialize EA = base + index in IR and use
    // the direct form here too.
    IR::IndirOpnd *indirOpnd = dst->AsIndirOpnd();
    IR::RegOpnd *indexOpnd = indirOpnd->GetIndexOpnd();
    int32 offset = indirOpnd->GetOffset();
#if ENABLE_FAST_ARRAYBUFFER
    const bool isWasmFast = CONFIG_FLAG(WasmFastArray) && m_func->GetJITFunctionBody()->IsWasmFunction();
#else
    const bool isWasmFast = false;
#endif
    IR::RegOpnd *extIndex = nullptr;
    if (!isWasmFast)
    {
        IR::LabelInstr * helperLabel = Lowerer::InsertLabel(true, instr);
        IR::LabelInstr * storeLabel = Lowerer::InsertLabel(false, instr);
        IR::LabelInstr * doneLabel = Lowerer::InsertLabel(false, instr);
        IR::Int64ConstOpnd *constEndOff =
            IR::Int64ConstOpnd::New((int64)16 + (int64)offset, TyInt64, m_func);
        IR::Opnd *cmpOpnd;
        if (indexOpnd != nullptr)
        {
            extIndex = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *ubfx = IR::Instr::New(Js::OpCode::UBFX, extIndex,
                indexOpnd->UseWithNewType(TyInt32, m_func),
                IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func, true), m_func);
            helperLabel->InsertBefore(ubfx);
            LegalizeMD::LegalizeInstr(ubfx);
            cmpOpnd = IR::RegOpnd::New(TyInt64, m_func);
            Lowerer::InsertAdd(true, cmpOpnd, extIndex, constEndOff, helperLabel);
        }
        else
        {
            cmpOpnd = constEndOff;
        }
        m_lowerer->InsertCompareBranch(cmpOpnd, instr->UnlinkSrc2(), Js::OpCode::BrGt_A, true, helperLabel, helperLabel);
        Lowerer::InsertBranch(Js::OpCode::Br, storeLabel, helperLabel);
        // SIMD OOB store is a trap (mirrors x64 LowerAsmJsStElemHelper
        // isSimdStore). Insert at storeLabel so the call lands at the end
        // of the helper block, before the in-bounds store.
        m_lowerer->GenerateRuntimeError(storeLabel, JSERR_ArgumentOutOfRange, IR::HelperOp_RuntimeRangeError);
        Lowerer::InsertBranch(Js::OpCode::Br, doneLabel, storeLabel);
        (void)doneLabel;
    }
    if (indexOpnd != nullptr)
    {
        if (extIndex == nullptr)
        {
            extIndex = IR::RegOpnd::New(TyInt64, m_func);
            IR::Instr *ubfx = IR::Instr::New(Js::OpCode::UBFX, extIndex,
                indexOpnd->UseWithNewType(TyInt32, m_func),
                IR::IntConstOpnd::New(BITFIELD(0, 32), TyMachReg, m_func, true), m_func);
            instr->InsertBefore(ubfx);
            LegalizeMD::LegalizeInstr(ubfx);
        }
        IR::RegOpnd *eaBase = IR::RegOpnd::New(TyMachPtr, m_func);
        IR::Instr *eaAdd = IR::Instr::New(Js::OpCode::Add_A, eaBase,
            indirOpnd->GetBaseOpnd(),
            extIndex->UseWithNewType(TyMachPtr, m_func), m_func);
        LowererMD::ChangeToAdd(eaAdd, false);
        instr->InsertBefore(eaAdd);
        LegalizeMD::LegalizeInstr(eaAdd);
        indirOpnd->ReplaceBaseOpnd(eaBase);
        indirOpnd->SetIndexOpnd(nullptr);
    }
    return Simd128ConvertToStore(dst, src1, instr);
}

IR::Instr *
LowererMD::Simd128LoadConst(IR::Instr *instr)
{
    Assert(instr->GetDst() && instr->m_opcode == Js::OpCode::Simd128_LdC);
    Assert(instr->GetDst()->IsSimd128());
    Assert(instr->GetSrc1()->IsSimd128());
    Assert(instr->GetSrc1()->IsSimd128ConstOpnd());
    Assert(instr->GetSrc2() == nullptr);
    AsmJsSIMDValue value = instr->GetSrc1()->AsSimd128ConstOpnd()->m_value;
    void *pValue = NativeCodeDataNewNoFixup(this->m_func->GetNativeCodeDataAllocator(), SIMDType<DataDesc_LowererMD_Simd128LoadConst>, value);
    IR::Opnd *simdRef;
    if (!m_func->IsOOPJIT())
    {
        simdRef = IR::MemRefOpnd::New((void *)pValue, instr->GetDst()->GetType(), instr->m_func);
    }
    else
    {
        int offset = NativeCodeData::GetDataTotalOffset(pValue);
        simdRef = IR::IndirOpnd::New(IR::RegOpnd::New(m_func->GetTopFunc()->GetNativeCodeDataSym(), TyVar, m_func), offset, instr->GetDst()->GetType(),
#if DBG
            NativeCodeData::GetDataDescription(pValue, m_func->m_alloc),
#endif
            m_func, true);
        m_lowerer->addToLiveOnBackEdgeSyms->Set(m_func->GetTopFunc()->GetNativeCodeDataSym()->m_id);
    }
    instr->ReplaceSrc1(simdRef);
    instr->m_opcode = GetAssignOp(instr->GetDst()->GetType());
    LegalizeMD::LegalizeInstr(instr);
    return instr->m_prev;
}
#endif

///----------------------------------------------------------------------------
///
/// LowererMD::LoadInputParamPtr
///
///     Load the address of the start of the passed-in parameters not including
///     the this parameter.
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LoadInputParamPtr(IR::Instr * instrInsert, IR::RegOpnd * optionalDstOpnd /* = nullptr */)
{
    if (this->m_func->GetJITFunctionBody()->IsCoroutine())
    {
        IR::RegOpnd * argPtrRegOpnd = Lowerer::LoadGeneratorArgsPtr(instrInsert);
        IR::IndirOpnd * indirOpnd = IR::IndirOpnd::New(argPtrRegOpnd, 1 * MachPtr, TyMachPtr, this->m_func);
        IR::RegOpnd * dstOpnd = optionalDstOpnd != nullptr ? optionalDstOpnd : IR::RegOpnd::New(TyMachPtr, this->m_func);

        return Lowerer::InsertLea(dstOpnd, indirOpnd, instrInsert);
    }
    else
    {
        StackSym * paramSym = GetImplicitParamSlotSym(3);
        IR::Instr * instr = this->m_lowerer->InsertLoadStackAddress(paramSym, instrInsert);
#if defined(__APPLE__)
        // InsertLoadStackAddress points at the homed copy of the first user
        // argument. LoadHeapArguments walks that pointer as one contiguous
        // argv. On Apple the contiguous image starts 8 register slots later.
        // Creating the arguments object is a call, so the frame homes all 8.
        IR::RegOpnd * ptrOpnd = instr->GetDst()->AsRegOpnd();
        instr = Lowerer::InsertAdd(false, ptrOpnd, ptrOpnd,
            IR::IntConstOpnd::New(NUM_INT_ARG_REGS * MachRegInt, TyMachReg, this->m_func), instrInsert);
#endif
        return instr;
    }
}

///----------------------------------------------------------------------------
///
/// LowererMD::LoadInputParamCount
///
///     Load the passed-in parameter count from the appropriate slot.
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LoadInputParamCount(IR::Instr * instrInsert, int adjust, bool needFlags)
{
    //  LDR  Rz, CallInfo
    //  UBFX Rx, Rz, 27, #1 // Get CallEval bit.
    //  UBFX Rz, Rz, 0, #24 // Extract call count 
    //  SUB  Rz, Rz, Rx     // Now Rz has the right number of parameters

    IR::SymOpnd * srcOpnd = Lowerer::LoadCallInfo(instrInsert);
    IR::RegOpnd * dstOpnd = IR::RegOpnd::New(TyMachReg,  this->m_func);

    IR::Instr *instr = IR::Instr::New(Js::OpCode::LDR, dstOpnd, srcOpnd,  this->m_func);
    instrInsert->InsertBefore(instr);

    // Get the actual call count. On ARM64 top 32 bits are unused
    instr = IR::Instr::New(Js::OpCode::UBFX, dstOpnd, dstOpnd, IR::IntConstOpnd::New(BITFIELD(0, Js::CallInfo::ksizeofCount), TyMachReg, this->m_func), this->m_func);
    instrInsert->InsertBefore(instr);

    return Lowerer::InsertSub(needFlags, dstOpnd, dstOpnd, IR::IntConstOpnd::New(-adjust, TyUint32, this->m_func), instrInsert);
}

IR::Instr *
LowererMD::LoadStackArgPtr(IR::Instr * instr)
{
    if (this->m_func->IsLoopBody())
    {
        // Get the first user param from the interpreter frame instance that was passed in.
        // These args don't include the func object and callinfo; we just need to advance past "this".

        // t1 = LDR [prm1 + m_inParams]
        // dst = ADD t1, sizeof(var)

        Assert(this->m_func->GetLoopParamSym());
        IR::RegOpnd *baseOpnd = IR::RegOpnd::New(this->m_func->GetLoopParamSym(), TyMachReg, this->m_func);
        size_t offset = Js::InterpreterStackFrame::GetOffsetOfInParams();
        IR::IndirOpnd *indirOpnd = IR::IndirOpnd::New(baseOpnd, (int32)offset, TyMachReg, this->m_func);
        IR::RegOpnd *tmpOpnd = IR::RegOpnd::New(TyMachReg, this->m_func);
        Lowerer::InsertMove(tmpOpnd, indirOpnd, instr);

        instr->SetSrc1(tmpOpnd);
        instr->SetSrc2(IR::IntConstOpnd::New(sizeof(Js::Var), TyMachReg, this->m_func));
    }
    else if (this->m_func->GetJITFunctionBody()->IsCoroutine())
    {
        IR::Instr *instr2 = LoadInputParamPtr(instr, instr->UnlinkDst()->AsRegOpnd());
        instr->Remove();
        instr = instr2;
    }
    else
    {
        // Get the args pointer relative to fp. We assume that fp is set up, since we'll only be looking
        // for the stack arg pointer in a non-leaf.

        // dst = ADD r11, "this" offset + sizeof(var)

        instr->SetSrc1(IR::RegOpnd::New(nullptr, FRAME_REG, TyMachReg, this->m_func));
        int32 stackArgBias = 0;
#if defined(__APPLE__)
        // The caller writes every JS slot at [sp + slot*8]. That image sits
        // above the 8 homed register slots, so the first user arg is 64 bytes
        // past the homed copy.
        stackArgBias = NUM_INT_ARG_REGS * MachRegInt;
#endif
        instr->SetSrc2(IR::IntConstOpnd::New((ArgOffsetFromFramePtr + Js::JavascriptFunctionArgIndex_SecondScriptArg) * sizeof(Js::Var) + stackArgBias, TyMachReg, this->m_func));
    }

    instr->m_opcode = Js::OpCode::ADD;
    Legalize(instr);

    return instr->m_prev;
}

IR::Instr *
LowererMD::LoadArgumentsFromFrame(IR::Instr * instr)
{
    IR::RegOpnd *baseOpnd;
    int32 offset;

    if (this->m_func->IsLoopBody())
    {
        // Get the arguments ptr from the interpreter frame instance that was passed in.
        Assert(this->m_func->GetLoopParamSym());
        baseOpnd = IR::RegOpnd::New(this->m_func->GetLoopParamSym(), TyMachReg, this->m_func);
        offset = Js::InterpreterStackFrame::GetOffsetOfArguments();
    }
    else
    {
        // Get the arguments relative to the frame pointer.
        baseOpnd = IR::RegOpnd::New(nullptr, FRAME_REG, TyMachReg, this->m_func);
        offset = -MachArgsSlotOffset;
    }

    instr->SetSrc1(IR::IndirOpnd::New(baseOpnd, offset, TyMachReg, this->m_func));
    this->ChangeToAssign(instr);

    return instr->m_prev;
}

// load argument count as I4
IR::Instr *
LowererMD::LoadArgumentCount(IR::Instr * instr)
{
    IR::RegOpnd *baseOpnd;
    int32 offset;

    if (this->m_func->IsLoopBody())
    {
        // Pull the arg count from the interpreter frame instance that was passed in.
        // (The callinfo in the loop body's frame just shows the single parameter, the interpreter frame.)
        Assert(this->m_func->GetLoopParamSym());
        baseOpnd = IR::RegOpnd::New(this->m_func->GetLoopParamSym(), TyMachReg, this->m_func);
        offset = Js::InterpreterStackFrame::GetOffsetOfInSlotsCount();
    }
    else
    {
        baseOpnd = IR::RegOpnd::New(nullptr, FRAME_REG, TyMachReg, this->m_func);
        offset = (ArgOffsetFromFramePtr + Js::JavascriptFunctionArgIndex_CallInfo) * sizeof(Js::Var);
    }

    instr->SetSrc1(IR::IndirOpnd::New(baseOpnd, offset, TyInt32, this->m_func));
    this->ChangeToAssign(instr);

    return instr->m_prev;
}

///----------------------------------------------------------------------------
///
/// LowererMD::LoadHeapArguments
///
///     Load the arguments object
///     NOTE: The same caveat regarding arguments passed on the stack applies here
///           as in LoadInputParamCount above.
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LoadHeapArguments(IR::Instr * instrArgs)
{
    ASSERT_INLINEE_FUNC(instrArgs);
    Func *func = instrArgs->m_func;
    IR::Instr * instrPrev = instrArgs->m_prev;

    if (func->IsStackArgsEnabled())
    {
        // The initial args slot value is zero.
        instrArgs->m_opcode = Js::OpCode::LDIMM;
        instrArgs->ReplaceSrc1(IR::AddrOpnd::NewNull(func));
        if (PHASE_TRACE1(Js::StackArgFormalsOptPhase) && func->GetJITFunctionBody()->GetInParamsCount() > 1)
        {
            Output::Print(_u("StackArgFormals : %s (%d) :Removing Heap Arguments object creation in Lowerer. \n"), instrArgs->m_func->GetJITFunctionBody()->GetDisplayName(), instrArgs->m_func->GetFunctionNumber());
            Output::Flush();
        }
    }
    else
    {
        // s7 = formals are let decls
        // s6 = memory context
        // s5 = array of property ID's
        // s4 = local frame instance
        // s3 = address of first actual argument (after "this")
        // s2 = actual argument count
        // s1 = current function
        // dst = JavascriptOperators::LoadHeapArguments(s1, s2, s3, s4, s5, s6, s7)

        // s7 = formals are let decls
        this->LoadHelperArgument(instrArgs, IR::IntConstOpnd::New(instrArgs->m_opcode == Js::OpCode::LdLetHeapArguments ? TRUE : FALSE, TyUint8, func));

        // s6 = memory context
        this->m_lowerer->LoadScriptContext(instrArgs);

        // s5 = array of property ID's

        intptr_t formalsPropIdArray = instrArgs->m_func->GetJITFunctionBody()->GetFormalsPropIdArrayAddr();
        if (!formalsPropIdArray)
        {
            formalsPropIdArray = instrArgs->m_func->GetScriptContextInfo()->GetNullAddr();
        }

        IR::Opnd * argArray = IR::AddrOpnd::New(formalsPropIdArray, IR::AddrOpndKindDynamicMisc, m_func);
        this->LoadHelperArgument(instrArgs, argArray);

        // s4 = local frame instance
        IR::Opnd * frameObj = instrArgs->UnlinkSrc1();
        this->LoadHelperArgument(instrArgs, frameObj);

        if (func->IsInlinee())
        {
            // s3 = address of first actual argument (after "this").
            StackSym *firstRealArgSlotSym = func->GetInlineeArgvSlotOpnd()->m_sym->AsStackSym();
            this->m_func->SetArgOffset(firstRealArgSlotSym, firstRealArgSlotSym->m_offset + MachPtr);
            IR::Instr *instr = this->m_lowerer->InsertLoadStackAddress(firstRealArgSlotSym, instrArgs);
            this->LoadHelperArgument(instrArgs, instr->GetDst());

            // s2 = actual argument count (without counting "this").
            this->LoadHelperArgument(instrArgs, IR::IntConstOpnd::New(func->actualCount - 1, TyUint32, func));

            // s1 = current function.
            this->LoadHelperArgument(instrArgs, func->GetInlineeFunctionObjectSlotOpnd());

            // Save the newly-created args object to its dedicated stack slot.
            IR::SymOpnd *argObjSlotOpnd = func->GetInlineeArgumentsObjectSlotOpnd();
            Lowerer::InsertMove(argObjSlotOpnd,instrArgs->GetDst(), instrArgs->m_next);
        }
        else
        {
            // s3 = address of first actual argument (after "this")
            // Stack looks like (function object)+0, (arg count)+4, (this)+8, actual args
            IR::Instr * instr = this->LoadInputParamPtr(instrArgs);
            this->LoadHelperArgument(instrArgs, instr->GetDst());

            // s2 = actual argument count (without counting "this")
            instr = this->LoadInputParamCount(instrArgs, -1);
            IR::Opnd * opndInputParamCount = instr->GetDst();
            
            this->LoadHelperArgument(instrArgs, opndInputParamCount);

            // s1 = current function
            StackSym * paramSym = GetImplicitParamSlotSym(0);
            IR::Opnd * srcOpnd = IR::SymOpnd::New(paramSym, TyMachReg, func);

            if (this->m_func->GetJITFunctionBody()->IsCoroutine())
            {
                // the function object for generator calls is a GeneratorVirtualScriptFunction object
                // and we need to pass the real JavascriptGeneratorFunction object so grab it instead
                IR::RegOpnd *tmpOpnd = IR::RegOpnd::New(TyMachReg, func);
                Lowerer::InsertMove(tmpOpnd, srcOpnd, instrArgs);

                srcOpnd = IR::IndirOpnd::New(tmpOpnd, Js::GeneratorVirtualScriptFunction::GetRealFunctionOffset(), TyMachPtr, func);
            }

            this->LoadHelperArgument(instrArgs, srcOpnd);

            // Save the newly-created args object to its dedicated stack slot.
            Lowerer::InsertMove(LowererMD::CreateStackArgumentsSlotOpnd(func), instrArgs->GetDst(), instrArgs->m_next);
        }
        this->ChangeToHelperCall(instrArgs, IR::HelperOp_LoadHeapArguments);
    }
    return instrPrev;
}

///----------------------------------------------------------------------------
///
/// LowererMD::LoadHeapArgsCached
///
///     Load the heap-based arguments object using a cached scope
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LoadHeapArgsCached(IR::Instr * instrArgs)
{
    Assert(!this->m_func->GetJITFunctionBody()->IsGenerator());
    ASSERT_INLINEE_FUNC(instrArgs);
    Func *func = instrArgs->m_func;
    IR::Instr * instrPrev = instrArgs->m_prev;

    if (instrArgs->m_func->IsStackArgsEnabled())
    {
        instrArgs->m_opcode = Js::OpCode::LDIMM;
        instrArgs->ReplaceSrc1(IR::AddrOpnd::NewNull(func));

        if (PHASE_TRACE1(Js::StackArgFormalsOptPhase) && func->GetJITFunctionBody()->GetInParamsCount() > 1)
        {
            Output::Print(_u("StackArgFormals : %s (%d) :Removing Heap Arguments object creation in Lowerer. \n"), instrArgs->m_func->GetJITFunctionBody()->GetDisplayName(), instrArgs->m_func->GetFunctionNumber());
            Output::Flush();
        }
    }
    else
    {
        // s7 = formals are let decls
        // s6 = memory context
        // s5 = local frame instance
        // s4 = address of first actual argument (after "this")
        // s3 = formal argument count
        // s2 = actual argument count
        // s1 = current function
        // dst = JavascriptOperators::LoadHeapArgsCached(s1, s2, s3, s4, s5, s6, s7)


        // s7 = formals are let decls
        IR::Opnd * formalsAreLetDecls = IR::IntConstOpnd::New((IntConstType)(instrArgs->m_opcode == Js::OpCode::LdLetHeapArgsCached), TyUint8, func);
        this->LoadHelperArgument(instrArgs, formalsAreLetDecls);

        // s6 = memory context
        this->m_lowerer->LoadScriptContext(instrArgs);

        // s5 = local frame instance
        IR::Opnd * frameObj = instrArgs->UnlinkSrc1();
        this->LoadHelperArgument(instrArgs, frameObj);

        if (func->IsInlinee())
        {
            // s4 = address of first actual argument (after "this").
            StackSym *firstRealArgSlotSym = func->GetInlineeArgvSlotOpnd()->m_sym->AsStackSym();
            this->m_func->SetArgOffset(firstRealArgSlotSym, firstRealArgSlotSym->m_offset + MachPtr);

            IR::Instr *instr = this->m_lowerer->InsertLoadStackAddress(firstRealArgSlotSym, instrArgs);
            this->LoadHelperArgument(instrArgs, instr->GetDst());

            // s3 = formal argument count (without counting "this").
        uint32 formalsCount = func->GetJITFunctionBody()->GetInParamsCount() - 1;
            this->LoadHelperArgument(instrArgs, IR::IntConstOpnd::New(formalsCount, TyUint32, func));

            // s2 = actual argument count (without counting "this").
            this->LoadHelperArgument(instrArgs, IR::IntConstOpnd::New(func->actualCount - 1, TyUint32, func));

            // s1 = current function.
            this->LoadHelperArgument(instrArgs, func->GetInlineeFunctionObjectSlotOpnd());

            // Save the newly-created args object to its dedicated stack slot.
            IR::SymOpnd *argObjSlotOpnd = func->GetInlineeArgumentsObjectSlotOpnd();
            Lowerer::InsertMove(argObjSlotOpnd, instrArgs->GetDst(), instrArgs->m_next);
        }
        else
        {
            // s4 = address of first actual argument (after "this")
            IR::Instr * instr = this->LoadInputParamPtr(instrArgs);
            this->LoadHelperArgument(instrArgs, instr->GetDst());

            // s3 = formal argument count (without counting "this")
            uint32 formalsCount = func->GetInParamsCount() - 1;
            this->LoadHelperArgument(instrArgs, IR::IntConstOpnd::New(formalsCount, TyMachReg, func));

            // s2 = actual argument count (without counting "this")
            instr = this->LoadInputParamCount(instrArgs, -1);
            this->LoadHelperArgument(instrArgs, instr->GetDst());

            // s1 = current function
            StackSym * paramSym = GetImplicitParamSlotSym(0);
            IR::Opnd * srcOpnd = IR::SymOpnd::New(paramSym, TyMachReg, func);
            this->LoadHelperArgument(instrArgs, srcOpnd);


            // Save the newly-created args object to its dedicated stack slot.
            Lowerer::InsertMove(LowererMD::CreateStackArgumentsSlotOpnd(func), instrArgs->GetDst(), instrArgs->m_next);

        }

        this->ChangeToHelperCall(instrArgs, IR::HelperOp_LoadHeapArgsCached);
    }
    return instrPrev;
}

///----------------------------------------------------------------------------
///
/// LowererMD::ChangeToHelperCall
///
///     Change the current instruction to a call to the given helper.
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::ChangeToHelperCall(IR::Instr * callInstr, IR::JnHelperMethod helperMethod, IR::LabelInstr *labelBailOut,
                              IR::Opnd *opndInstance, IR::PropertySymOpnd *propSymOpnd, bool isHelperContinuation)
{
#if DBG
    this->m_lowerer->ReconcileWithLowererStateOnHelperCall(callInstr, helperMethod);
#endif
    IR::Instr * bailOutInstr = callInstr;
    if (callInstr->HasBailOutInfo())
    {
        const IR::BailOutKind bailOutKind = callInstr->GetBailOutKind();
        if (bailOutKind == IR::BailOutOnNotPrimitive)
        {
            callInstr = IR::Instr::New(callInstr->m_opcode, callInstr->m_func);
            bailOutInstr->TransferTo(callInstr);
            bailOutInstr->InsertBefore(callInstr);

            bailOutInstr->m_opcode = Js::OpCode::BailOnNotPrimitive;
            bailOutInstr->SetSrc1(opndInstance);
        }
        else if (BailOutInfo::IsBailOutOnImplicitCalls(bailOutKind))
        {
            bailOutInstr = this->m_lowerer->SplitBailOnImplicitCall(callInstr);
        }
        else
        {
            AssertMsg(false, "Unexpected BailOutKind, are we adding new BailOutKind on instructions?");
        }
    }

    IR::HelperCallOpnd *helperCallOpnd = Lowerer::CreateHelperCallOpnd(helperMethod, this->GetHelperArgsCount(), m_func);
    if (helperCallOpnd->IsDiagHelperCallOpnd())
    {
        // Load arguments for the wrapper.
        this->LoadHelperArgument(callInstr, IR::AddrOpnd::New((Js::Var)IR::GetMethodOriginalAddress(m_func->GetThreadContextInfo(), helperMethod), IR::AddrOpndKindDynamicMisc, m_func));
        this->m_lowerer->LoadScriptContext(callInstr);
    }
    callInstr->SetSrc1(helperCallOpnd);

    IR::Instr * instrRet = this->LowerCall(callInstr, 0);

    if (bailOutInstr != callInstr)
    {
        // The bailout needs to be lowered after we lower the helper call because the helper argument
        // has already been loaded. We need to drain them on AMD64 before starting another helper call
        if (bailOutInstr->m_opcode == Js::OpCode::BailOnNotPrimitive)
        {
            this->m_lowerer->LowerBailOnTrue(bailOutInstr, labelBailOut);
        }
        else if (bailOutInstr->m_opcode == Js::OpCode::BailOnNotEqual)
        {
            // `SplitBailOnImplicitCall` above changes the opcode to BailOnNotEqual
            Assert(BailOutInfo::IsBailOutOnImplicitCalls(bailOutInstr->GetBailOutKind()));
            this->m_lowerer->LowerBailOnEqualOrNotEqual(bailOutInstr, nullptr, labelBailOut, propSymOpnd, isHelperContinuation);
        }
        else
        {
            AssertMsg(false, "Unexpected OpCode for BailOutInstruction");
        }
    }

    return instrRet;
}

IR::Instr* LowererMD::ChangeToHelperCallMem(IR::Instr * instr,  IR::JnHelperMethod helperMethod)
{
    this->m_lowerer->LoadScriptContext(instr);

    return this->ChangeToHelperCall(instr, helperMethod);
}

///----------------------------------------------------------------------------
///
/// LowererMD::ChangeToAssign
///
///     Change to a copy. Handle riscification of operands.
///
///----------------------------------------------------------------------------

// ToDo (SaAgarwa) Copied from ARM32 to compile. Validate is this correct
IR::Instr *
LowererMD::ChangeToAssignNoBarrierCheck(IR::Instr * instr)
{
    return ChangeToAssign(instr, instr->GetDst()->GetType());
}

IR::Instr *
LowererMD::ChangeToAssign(IR::Instr * instr)
{
    return ChangeToAssign(instr, instr->GetDst()->GetType());
}

IR::Instr *
LowererMD::ChangeToAssign(IR::Instr * instr, IRType destType)
{
    Assert(!instr->HasBailOutInfo() || instr->GetBailOutKind() == IR::BailOutExpectingInteger
                                       || instr->GetBailOutKind() == IR::BailOutExpectingString);

    IR::Opnd *src = instr->GetSrc1();
    IRType srcType = src->GetType();
    if (src->IsImmediateOpnd() || src->IsLabelOpnd())
    {
        instr->m_opcode = Js::OpCode::LDIMM;
    }
    else if(destType == TyFloat32 && instr->GetDst()->IsRegOpnd())
    {
        Assert(instr->GetSrc1()->IsFloat32());
        instr->m_opcode = Js::OpCode::FLDR;

        // Note that we allocate double register for single precision floats as well, as the register allocator currently
        // does not support 32-bit float registers
        instr->ReplaceDst(instr->GetDst()->UseWithNewType(TyFloat64, instr->m_func));
        if(instr->GetSrc1()->IsRegOpnd())
        {
            instr->ReplaceSrc1(instr->GetSrc1()->UseWithNewType(TyFloat64, instr->m_func));
        }
    }
    else if (!src->IsIndirOpnd() && TySize[destType] > TySize[srcType] && (IRType_IsSignedInt(destType) || IRType_IsUnsignedInt(destType)))
    {
        // If we're moving between different lengths of registers, we need to use the
        // right operator - sign extend if the source is int, zero extend if uint.
        if (IRType_IsSignedInt(srcType))
        {
            instr->ReplaceSrc1(src->UseWithNewType(IRType_EnsureSigned(destType), instr->m_func));
            instr->SetSrc2(IR::IntConstOpnd::New(BITFIELD(0, TySize[srcType] * MachBits), TyMachReg, instr->m_func, true));
            instr->m_opcode = Js::OpCode::SBFX;
        }
        else if (IRType_IsUnsignedInt(srcType))
        {
            instr->ReplaceSrc1(src->UseWithNewType(IRType_EnsureUnsigned(destType), instr->m_func));
            instr->SetSrc2(IR::IntConstOpnd::New(BITFIELD(0, TySize[srcType] * MachBits), TyMachReg, instr->m_func, true));
            instr->m_opcode = Js::OpCode::UBFX;
        }
        else
        {
            AssertMsg(false, "argument size mismatch for mov instruction, with non int/uint types!");
        }
    }
    else
    {
        instr->m_opcode = IRType_IsFloat(destType) ? Js::OpCode::FMOV : Js::OpCode::MOV;
    }

    AutoRestoreLegalize restore(instr->m_func, false);
    LegalizeMD::LegalizeInstr(instr);

    return instr;
}

IR::Instr *
LowererMD::ChangeToWriteBarrierAssign(IR::Instr * assignInstr, const Func* func)
{
    // Mirrors LowerMDShared::ChangeToWriteBarrierAssign. Without the card
    // byte, a concurrent marker that already scanned the destination object
    // never revisits it and sweeps the stored target (full-JIT stores were
    // invisible to concurrent rescan on software-write-barrier platforms;
    // Octane splay crashed this way). Windows write-watch covered the
    // missing barrier historically, which is why the ARM stub went unnoticed.
#ifdef RECYCLER_WRITE_BARRIER_JIT
    IR::Opnd* dest = assignInstr->GetDst();

    auto threadContextInfo = func->GetTopFunc()->GetThreadContextInfo();
    void* destAddr = nullptr;
    bool isPossibleBarrieredDest = false;

    if (TySize[dest->GetType()] == sizeof(void*))
    {
        if (dest->IsIndirOpnd())
        {
            Assert(!dest->AsIndirOpnd()->HasAddrKind());
            isPossibleBarrieredDest = true;
        }
        else if (dest->IsMemRefOpnd())
        {
            // looks all thread context field access are from MemRefOpnd
            destAddr = (void*)dest->AsMemRefOpnd()->GetMemLoc();
            isPossibleBarrieredDest = destAddr != nullptr
                && ((intptr_t)destAddr % sizeof(void*)) == 0
                && destAddr != (void*)threadContextInfo->GetImplicitCallFlagsAddr()
                && destAddr != (void*)threadContextInfo->GetDisableImplicitFlagsAddr()
                && destAddr != (void*)threadContextInfo->GetBailOutRegisterSaveSpaceAddr();

            if (isPossibleBarrieredDest)
            {
                Assert(Recycler::WBCheckIsRecyclerAddress((char*)destAddr));
            }
        }
    }
#endif

    IR::Instr * instr = ChangeToAssignNoBarrierCheck(assignInstr);

    // Now insert write barrier if necessary
#ifdef RECYCLER_WRITE_BARRIER_JIT
    if (isPossibleBarrieredDest
        && assignInstr->m_opcode == Js::OpCode::MOV // ignore SSE instructions like MOVSD
        && assignInstr->GetSrc1()->IsWriteBarrierTriggerableValue())
    {
        instr = LowererMD::GenerateWriteBarrier(assignInstr);
    }
#endif

    return instr;
}

#ifdef RECYCLER_WRITE_BARRIER_JIT
// Materialize the effective address of an Indir/MemRef operand into a
// register. Do not use LEA here: EncoderMD::CanonicalizeLea asserts
// scale==0 ("NYI Needs shifted register support for ADD") and silently
// drops the scale in non-assert builds, computing base+index instead of
// base+(index<<scale) for slot operands.
static IR::Instr * GenerateWriteBarrierAddress(IR::RegOpnd * dstReg, IR::Opnd * opnd, IR::Instr * insertBeforeInstr)
{
    Func * func = insertBeforeInstr->m_func;
    IR::Instr * firstInstr = nullptr;
    auto insert = [&](IR::Instr * instr)
    {
        insertBeforeInstr->InsertBefore(instr);
        if (firstInstr == nullptr)
        {
            firstInstr = instr;
        }
    };

    if (opnd->IsMemRefOpnd())
    {
        insert(IR::Instr::New(Js::OpCode::LDIMM, dstReg,
            IR::AddrOpnd::New((void *)opnd->AsMemRefOpnd()->GetMemLoc(), IR::AddrOpndKindDynamicMisc, func), func));
        return firstInstr;
    }

    IR::IndirOpnd * indirOpnd = opnd->AsIndirOpnd();
    IR::RegOpnd * baseOpnd = indirOpnd->GetBaseOpnd();
    IR::RegOpnd * indexOpnd = indirOpnd->GetIndexOpnd();
    const int32 offset = indirOpnd->GetOffset();
    const byte scale = indirOpnd->GetScale();

    if (indexOpnd == nullptr)
    {
        if (offset == 0)
        {
            return Lowerer::InsertMove(dstReg, baseOpnd, insertBeforeInstr, false);
        }
        insert(IR::Instr::New(Js::OpCode::ADD, dstReg, baseOpnd,
            IR::IntConstOpnd::New(offset, TyMachReg, func), func));
        return firstInstr;
    }

    IR::RegOpnd * scaledIndex = indexOpnd;
    if (scale != 0)
    {
        scaledIndex = IR::RegOpnd::New(TyMachReg, func);
        insert(IR::Instr::New(Js::OpCode::LSL, scaledIndex, indexOpnd,
            IR::IntConstOpnd::New(scale, TyUint8, func, true), func));
    }
    insert(IR::Instr::New(Js::OpCode::ADD, dstReg, baseOpnd, scaledIndex, func));
    if (offset != 0)
    {
        insert(IR::Instr::New(Js::OpCode::ADD, dstReg, dstReg,
            IR::IntConstOpnd::New(offset, TyMachReg, func), func));
    }
    return firstInstr;
}
#endif

void
LowererMD::GenerateWriteBarrierAssign(IR::MemRefOpnd * opndDst, IR::Opnd * opndSrc, IR::Instr * insertBeforeInstr)
{
    Lowerer::InsertMove(opndDst, opndSrc, insertBeforeInstr);
#ifdef RECYCLER_WRITE_BARRIER_JIT
    if (opndSrc->IsWriteBarrierTriggerableValue())
    {
        void * address = (void *)opndDst->AsMemRefOpnd()->GetMemLoc();
#ifdef RECYCLER_WRITE_BARRIER_BYTE
        IR::MemRefOpnd * cardTableEntry = IR::MemRefOpnd::New(
            &RecyclerWriteBarrierManager::GetAddressOfCardTable()[RecyclerWriteBarrierManager::GetCardTableIndex(address)], TyInt8, insertBeforeInstr->m_func);

        // Explicit STR from a full-width temp; see GenerateWriteBarrier
        // for why a TyInt8-immediate MOV is unsafe on ARM64.
        IR::RegOpnd * oneRegOpnd = IR::RegOpnd::New(TyMachReg, insertBeforeInstr->m_func);
        IR::Instr * oneInstr = IR::Instr::New(Js::OpCode::LDIMM, oneRegOpnd,
            IR::IntConstOpnd::New(1, TyMachReg, insertBeforeInstr->m_func), insertBeforeInstr->m_func);
        insertBeforeInstr->InsertBefore(oneInstr);
        IR::Instr * movInstr = IR::Instr::New(Js::OpCode::STR, cardTableEntry, oneRegOpnd, insertBeforeInstr->m_func);
        insertBeforeInstr->InsertBefore(movInstr);
#else
        CompileAssert(false); // non-byte card table is x86 bitmask code in LowerMDShared; ARM64 builds define RECYCLER_WRITE_BARRIER_BYTE
#endif
    }
#endif
}

void
LowererMD::GenerateWriteBarrierAssign(IR::IndirOpnd * opndDst, IR::Opnd * opndSrc, IR::Instr * insertBeforeInstr)
{
#ifdef RECYCLER_WRITE_BARRIER_JIT
    if (opndSrc->IsWriteBarrierTriggerableValue())
    {
        IR::RegOpnd * writeBarrierAddrRegOpnd = IR::RegOpnd::New(TyMachPtr, insertBeforeInstr->m_func);
        GenerateWriteBarrierAddress(writeBarrierAddrRegOpnd, opndDst, insertBeforeInstr);

        IR::Instr* movInstr = IR::Instr::New(Js::OpCode::MOV,
            IR::IndirOpnd::New(writeBarrierAddrRegOpnd, 0, TyMachReg, insertBeforeInstr->m_func), opndSrc, insertBeforeInstr->m_func);
        insertBeforeInstr->InsertBefore(movInstr);
        GenerateWriteBarrier(movInstr);

        // The mov happens above, and it's slightly faster doing it that way since we've already calculated the address we're writing to
        return;
    }
#endif
    Lowerer::InsertMove(opndDst, opndSrc, insertBeforeInstr);
    return;
}

#ifdef RECYCLER_WRITE_BARRIER_JIT
IR::Instr*
LowererMD::GenerateWriteBarrier(IR::Instr * assignInstr)
{
#if defined(RECYCLER_WRITE_BARRIER_BYTE)
    PHASE_PRINT_TRACE(Js::JitWriteBarrierPhase, assignInstr->m_func, _u("Generating write barrier\n"));
    IR::RegOpnd * indexOpnd = IR::RegOpnd::New(TyMachPtr, assignInstr->m_func);
    IR::Instr * loadIndexInstr = GenerateWriteBarrierAddress(indexOpnd, assignInstr->GetDst(), assignInstr);
    // ARM64 uses LSR where the shared x86/x64 lowering uses SHR. The shift
    // is log2(s_BytesPerCard): 14 on Apple ARM64 (16K cards), not the x64
    // hardcoded 12 -- with 12 the index is 4x too large and the card store
    // lands in uncommitted card-table reservation (SIGBUS).
    const uint32 cardShift = RecyclerWriteBarrierManager::s_BitArrayCardTableShift + 5;
    IR::Instr * shiftBitInstr = IR::Instr::New(Js::OpCode::LSR, indexOpnd, indexOpnd,
        IR::IntConstOpnd::New(cardShift, TyUint8, assignInstr->m_func, true), assignInstr->m_func);
    // Card byte is set AFTER the assign, matching WriteBarrierPtr::
    // WriteBarrierSet ("set the barrier bit after updating the reference
    // to prevent a race issue that background thread is resetting all
    // the dirty pages") and the shared x86/x64 sequence.
    assignInstr->InsertAfter(shiftBitInstr);

    // The cardtable address is likely 64 bits already so we have to load it to a register
    //  LEA reg1, targetOfWrite
    //  LSR reg1, 12
    //  MOV reg2, cardTableAddress
    //  MOV [reg2 + reg1], 1
    IR::RegOpnd * cardTableRegOpnd = IR::RegOpnd::New(TyMachReg, assignInstr->m_func);
    // LDIMM (not MOV): ARM64 legalizes LDIMM of a 64-bit address into the
    // MOVZ/MOVK chunk sequence; a raw MOV with an AddrOpnd source encodes
    // truncated and the card store lands near address zero.
    IR::Instr * cardTableAddrInstr = IR::Instr::New(Js::OpCode::LDIMM, cardTableRegOpnd,
        IR::AddrOpnd::New(RecyclerWriteBarrierManager::GetAddressOfCardTable(), IR::AddrOpndKindWriteBarrierCardTable, assignInstr->m_func),
        assignInstr->m_func);
    shiftBitInstr->InsertAfter(cardTableAddrInstr);

    IR::IndirOpnd * cardTableEntryOpnd = IR::IndirOpnd::New(cardTableRegOpnd, indexOpnd,
        TyInt8, assignInstr->m_func);
    // Store the card byte with an explicit STR from a full-width temp
    // holding 1. A MOV with a TyInt8 immediate instead legalizes through
    // a TyInt8 sink temp whose live range the register allocator aliased
    // onto the card-table base register under pressure (strb w15,[x15,x30]
    // with x15==1), corrupting the card address.
    IR::RegOpnd * oneRegOpnd = IR::RegOpnd::New(TyMachReg, assignInstr->m_func);
    IR::Instr * oneInstr = IR::Instr::New(Js::OpCode::LDIMM, oneRegOpnd,
        IR::IntConstOpnd::New(1, TyMachReg, assignInstr->m_func), assignInstr->m_func);
    cardTableAddrInstr->InsertAfter(oneInstr);
    IR::Instr * movInstr = IR::Instr::New(Js::OpCode::STR, cardTableEntryOpnd, oneRegOpnd, assignInstr->m_func);
    oneInstr->InsertAfter(movInstr);
    return loadIndexInstr;
#else
    CompileAssert(false); // non-byte card table is x86 bitmask code in LowerMDShared; ARM64 builds define RECYCLER_WRITE_BARRIER_BYTE
    return assignInstr;
#endif
}
#endif

///----------------------------------------------------------------------------
///
/// LowererMD::LowerRet
///
///     Lower Ret to "MOV EAX, src"
///     The real RET is inserted at the exit of the function when emitting the
///     epilog.
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LowerRet(IR::Instr * retInstr)
{
    // W4 slice 1: asm.js returns use the internal convention (ABI section 3):
    // i32/i64 in x0, f32 in s0 (low half of d0, section 4), f64 in d0.
#if defined(ENABLE_WASM)
    if (m_func->GetJITFunctionBody()->IsAsmJsMode() && !m_func->IsLoopBody())
    {
        Js::AsmJsRetType::Which asmType = m_func->GetJITFunctionBody()->GetAsmJsInfo()->GetRetType();
        if (asmType == Js::AsmJsRetType::Void)
        {
            return retInstr;
        }
        IRType regType = TyMachReg;
        RegNum regNum = RETURN_REG;
        switch (asmType)
        {
        case Js::AsmJsRetType::Double:
            regType = TyFloat64;
            regNum = RETURN_DBL_REG;
            break;
        case Js::AsmJsRetType::Float:
            regType = TyFloat32;
            regNum = RETURN_DBL_REG;
            break;
        case Js::AsmJsRetType::Signed:
        case Js::AsmJsRetType::Int64:
            regType = TyMachReg;
            regNum = RETURN_REG;
            break;
#ifdef ENABLE_WASM_SIMD
        // W8-NEON slice 1: 128-bit returns in Q0 (GetRegReturnAsmJs).
        case Js::AsmJsRetType::Int32x4:
            regType = TySimd128I4;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Int16x8:
            regType = TySimd128I8;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Int8x16:
            regType = TySimd128I16;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Uint32x4:
            regType = TySimd128U4;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Uint16x8:
            regType = TySimd128U8;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Uint8x16:
            regType = TySimd128U16;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Bool32x4:
            regType = TySimd128B4;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Bool16x8:
            regType = TySimd128B8;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Bool8x16:
            regType = TySimd128B16;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Float32x4:
            regType = TySimd128F4;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Float64x2:
            regType = TySimd128D2;
            regNum = GetRegReturnAsmJs(regType);
            break;
        case Js::AsmJsRetType::Int64x2:
            regType = TySimd128I2;
            regNum = GetRegReturnAsmJs(regType);
            break;
#endif
        default:
            Assert(UNREACHED);
            break;
        }
        IR::RegOpnd *retReg = IR::RegOpnd::New(regType, m_func);
        retReg->SetReg(regNum);
        Lowerer::InsertMove(retReg, retInstr->UnlinkSrc1(), retInstr);
        retInstr->SetSrc1(retReg);
        return retInstr;
    }
#endif
    IR::RegOpnd *retReg = IR::RegOpnd::New(TyMachReg, m_func);
    retReg->SetReg(RETURN_REG);
    Lowerer::InsertMove(retReg, retInstr->UnlinkSrc1(), retInstr);

    retInstr->SetSrc1(retReg);

    return retInstr;
}

///----------------------------------------------------------------------------
///
/// LowererMD::MDBranchOpcode
///
///     Map HIR branch opcode to machine-dependent equivalent.
///
///----------------------------------------------------------------------------

Js::OpCode
LowererMD::MDBranchOpcode(Js::OpCode opcode)
{
    switch (opcode)
    {
    case Js::OpCode::BrEq_A:
    case Js::OpCode::BrSrEq_A:
    case Js::OpCode::BrNotNeq_A:
    case Js::OpCode::BrSrNotNeq_A:
    case Js::OpCode::BrAddr_A:
        return Js::OpCode::BEQ;

    case Js::OpCode::BrNeq_A:
    case Js::OpCode::BrSrNeq_A:
    case Js::OpCode::BrNotEq_A:
    case Js::OpCode::BrSrNotEq_A:
    case Js::OpCode::BrNotAddr_A:
        return Js::OpCode::BNE;

    case Js::OpCode::BrLt_A:
    case Js::OpCode::BrNotGe_A:
        return Js::OpCode::BLT;

    case Js::OpCode::BrLe_A:
    case Js::OpCode::BrNotGt_A:
        return Js::OpCode::BLE;

    case Js::OpCode::BrGt_A:
    case Js::OpCode::BrNotLe_A:
        return Js::OpCode::BGT;

    case Js::OpCode::BrGe_A:
    case Js::OpCode::BrNotLt_A:
        return Js::OpCode::BGE;

    case Js::OpCode::BrUnGt_A:
        return Js::OpCode::BHI;

    case Js::OpCode::BrUnGe_A:
        return Js::OpCode::BCS;

    case Js::OpCode::BrUnLt_A:
        return Js::OpCode::BCC;

    case Js::OpCode::BrUnLe_A:
        return Js::OpCode::BLS;

    default:
        AssertMsg(0, "NYI");
        return opcode;
    }
}

Js::OpCode
LowererMD::MDUnsignedBranchOpcode(Js::OpCode opcode)
{
    switch (opcode)
    {
    case Js::OpCode::BrEq_A:
    case Js::OpCode::BrSrEq_A:
    case Js::OpCode::BrSrNotNeq_A:
    case Js::OpCode::BrNotNeq_A:
    case Js::OpCode::BrAddr_A:
        return Js::OpCode::BEQ;

    case Js::OpCode::BrNeq_A:
    case Js::OpCode::BrSrNeq_A:
    case Js::OpCode::BrSrNotEq_A:
    case Js::OpCode::BrNotEq_A:
    case Js::OpCode::BrNotAddr_A:
        return Js::OpCode::BNE;

    case Js::OpCode::BrLt_A:
    case Js::OpCode::BrNotGe_A:
        return Js::OpCode::BCC;

    case Js::OpCode::BrLe_A:
    case Js::OpCode::BrNotGt_A:
        return Js::OpCode::BLS;

    case Js::OpCode::BrGt_A:
    case Js::OpCode::BrNotLe_A:
        return Js::OpCode::BHI;

    case Js::OpCode::BrGe_A:
    case Js::OpCode::BrNotLt_A:
        return Js::OpCode::BCS;

    default:
        AssertMsg(0, "NYI");
        return opcode;
    }
}

Js::OpCode LowererMD::MDCompareWithZeroBranchOpcode(Js::OpCode opcode)
{
    Assert(opcode == Js::OpCode::BrLt_A || opcode == Js::OpCode::BrGe_A);
    return opcode == Js::OpCode::BrLt_A ? Js::OpCode::BMI : Js::OpCode::BPL;
}

void LowererMD::ChangeToAdd(IR::Instr *const instr, const bool needFlags)
{
    Assert(instr);
    Assert(instr->GetDst());
    Assert(instr->GetSrc1());
    Assert(instr->GetSrc2());

    if(instr->GetDst()->IsFloat64())
    {
        Assert(instr->GetSrc1()->IsFloat64());
        Assert(instr->GetSrc2()->IsFloat64());
        Assert(!needFlags);
        instr->m_opcode = Js::OpCode::FADD;
        return;
    }

    instr->m_opcode = needFlags ? Js::OpCode::ADDS : Js::OpCode::ADD;
}

void LowererMD::ChangeToSub(IR::Instr *const instr, const bool needFlags)
{
    Assert(instr);
    Assert(instr->GetDst());
    Assert(instr->GetSrc1());
    Assert(instr->GetSrc2());

    if(instr->GetDst()->IsFloat64())
    {
        Assert(instr->GetSrc1()->IsFloat64());
        Assert(instr->GetSrc2()->IsFloat64());
        Assert(!needFlags);
        instr->m_opcode = Js::OpCode::FSUB;
        return;
    }

    instr->m_opcode = needFlags ? Js::OpCode::SUBS : Js::OpCode::SUB;
}

void LowererMD::ChangeToShift(IR::Instr *const instr, const bool needFlags)
{
    Assert(instr);
    Assert(instr->GetDst());
    Assert(instr->GetSrc1());
    Assert(instr->GetSrc2());

    Func *const func = instr->m_func;

    switch(instr->m_opcode)
    {
        case Js::OpCode::Shl_A:
        case Js::OpCode::Shl_I4:
            Assert(!needFlags); // not implemented
            instr->m_opcode = Js::OpCode::LSL;
            break;

        case Js::OpCode::Shr_A:
        case Js::OpCode::Shr_I4:
            Assert(!needFlags); // not implemented
            instr->m_opcode = Js::OpCode::ASR;
            break;

        case Js::OpCode::ShrU_A:
        case Js::OpCode::ShrU_I4:
            Assert(!needFlags); // not implemented
            instr->m_opcode = Js::OpCode::LSR;
            break;

        default:
            Assert(false);
            __assume(false);
    }

    // Javascript requires the ShiftCount is masked to the bottom 5 bits.
    uint8 mask = TySize[instr->GetDst()->GetType()] == 8 ? 63 : 31;
    if (instr->GetSrc2()->IsIntConstOpnd())
    {
        // In the constant case, do the mask manually.
        IntConstType immed = instr->GetSrc2()->AsIntConstOpnd()->GetValue() & mask;
        if (immed == 0)
        {
            // Shift by zero is just a move, and the shift-right instructions
            // don't permit encoding of a zero shift amount.
            instr->m_opcode = Js::OpCode::MOV;
            instr->FreeSrc2();
        }
        else
        {
            instr->GetSrc2()->AsIntConstOpnd()->SetValue(immed);
        }
    }
    else
    {
        // In the variable case, generate code to do the mask.
        IR::Opnd *const src2 = instr->UnlinkSrc2();
        instr->SetSrc2(IR::RegOpnd::New(src2->GetType(), func));
        IR::Instr *const newInstr = IR::Instr::New(
            Js::OpCode::AND, instr->GetSrc2(), src2, IR::IntConstOpnd::New(mask, TyInt8, func), func);
        instr->InsertBefore(newInstr);
    }
}

const uint16
LowererMD::GetFormalParamOffset()
{
    //In ARM formal params are offset into the param area.
    //So we only count the non-user params (Function object & CallInfo and let the encoder account for the saved R11 and LR
    return 2;
}

///----------------------------------------------------------------------------
///
/// LowererMD::LowerCondBranch
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::LowerCondBranch(IR::Instr * instr)
{
    AssertMsg(instr->GetSrc1() != nullptr, "Expected src opnds on conditional branch");

    IR::Opnd *  opndSrc1 = instr->UnlinkSrc1();
    IR::Instr * instrPrev = nullptr;

    switch (instr->m_opcode)
    {
        case Js::OpCode::BrTrue_A:
        case Js::OpCode::BrOnNotEmpty:
        case Js::OpCode::BrNotNull_A:
        case Js::OpCode::BrOnObject_A:
        case Js::OpCode::BrOnObjectOrNull_A:
        case Js::OpCode::BrOnConstructor_A:
        case Js::OpCode::BrOnClassConstructor:
        case Js::OpCode::BrOnBaseConstructorKind:
            Assert(!opndSrc1->IsFloat64());
            AssertMsg(opndSrc1->IsRegOpnd(),"NYI for other operands");
            AssertMsg(instr->GetSrc2() == nullptr, "Expected 1 src on boolean branch");
            instrPrev = IR::Instr::New(Js::OpCode::CMP, this->m_func);
            instrPrev->SetSrc1(opndSrc1);
            instrPrev->SetSrc2(IR::IntConstOpnd::New(0, TyInt32, m_func));
            instr->InsertBefore(instrPrev);
            LegalizeMD::LegalizeInstr(instrPrev);

            instr->m_opcode = Js::OpCode::BNE;

            break;
        case Js::OpCode::BrFalse_A:
        case Js::OpCode::BrOnEmpty:
            Assert(!opndSrc1->IsFloat64());
            AssertMsg(opndSrc1->IsRegOpnd(),"NYI for other operands");
            AssertMsg(instr->GetSrc2() == nullptr, "Expected 1 src on boolean branch");
            instrPrev = IR::Instr::New(Js::OpCode::CMP, this->m_func);
            instrPrev->SetSrc1(opndSrc1);
            instrPrev->SetSrc2(IR::IntConstOpnd::New(0, TyInt32, m_func));
            instr->InsertBefore(instrPrev);
            LegalizeMD::LegalizeInstr(instrPrev);

            instr->m_opcode = Js::OpCode::BEQ;

            break;

        default:
            IR::Opnd *  opndSrc2 = instr->UnlinkSrc2();
            AssertMsg(opndSrc2 != nullptr, "Expected 2 src's on non-boolean branch");

            if (opndSrc1->IsFloat64())
            {
                AssertMsg(opndSrc1->IsRegOpnd(),"NYI for other operands");
                Assert(opndSrc2->IsFloat64());
                Assert(opndSrc2->IsRegOpnd() && opndSrc1->IsRegOpnd());
                //This comparison updates the FPSCR - floating point status control register
                instrPrev = IR::Instr::New(Js::OpCode::FCMP, this->m_func);
                instrPrev->SetSrc1(opndSrc1);
                instrPrev->SetSrc2(opndSrc2);
                instr->InsertBefore(instrPrev);
                LegalizeMD::LegalizeInstr(instrPrev);

                instr->m_opcode = LowererMD::MDBranchOpcode(instr->m_opcode);
            }
            else
            {
                AssertMsg(opndSrc2->IsRegOpnd() || opndSrc2->IsIntConstOpnd()  || (opndSrc2->IsAddrOpnd()), "NYI for other operands");

                instrPrev = IR::Instr::New(Js::OpCode::CMP, this->m_func);
                instrPrev->SetSrc1(opndSrc1);
                instrPrev->SetSrc2(opndSrc2);
                instr->InsertBefore(instrPrev);

                LegalizeMD::LegalizeInstr(instrPrev);

                instr->m_opcode = MDBranchOpcode(instr->m_opcode);
            }
            break;
    }
    return instr;
}

///----------------------------------------------------------------------------
///
/// LowererMD::ForceDstToReg
///
///----------------------------------------------------------------------------

IR::Instr*
LowererMD::ForceDstToReg(IR::Instr *instr)
{
    IR::Opnd * dst = instr->GetDst();

    if (dst->IsRegOpnd())
    {
        return instr;
    }

    IR::Instr * newInstr = instr->SinkDst(Js::OpCode::Ld_A);
    LowererMD::ChangeToAssign(newInstr);
    return newInstr;
}

IR::Instr *
LowererMD::LoadFunctionObjectOpnd(IR::Instr *instr, IR::Opnd *&functionObjOpnd)
{
    IR::Opnd * src1 = instr->GetSrc1();
    IR::Instr * instrPrev = instr->m_prev;
    if (src1 == nullptr)
    {
        IR::RegOpnd * regOpnd = IR::RegOpnd::New(TyMachPtr, m_func);
        //function object is first argument and mark it as IsParamSlotSym.
        StackSym *paramSym = GetImplicitParamSlotSym(0);
        IR::SymOpnd *paramOpnd = IR::SymOpnd::New(paramSym, TyMachPtr, m_func);

        instrPrev = Lowerer::InsertMove(regOpnd, paramOpnd, instr);
        functionObjOpnd = instrPrev->GetDst();
    }
    else
    {
        // Inlinee LdHomeObj, use the function object opnd on the instruction
        functionObjOpnd = instr->UnlinkSrc1();
        if (!functionObjOpnd->IsRegOpnd())
        {
            Assert(functionObjOpnd->IsAddrOpnd());
        }
    }

    return instrPrev;
}
bool
LowererMD::GenerateFastDivAndRem(IR::Instr *instrDiv, IR::LabelInstr* bailOutLabel)
{
    return false;
}
void
LowererMD::GenerateFastDivByPow2(IR::Instr *instrDiv)
{
    //// Given:
    //// dst = Div_A src1, src2
    //// where src2 == power of 2
    ////
    //// Generate:
    ////       (observation: positive q divides by p equally, where p = power of 2, if q's binary representation
    ////       has all zeroes to the right of p's power 2 bit, try to see if that is the case)
    //// s1 =  AND  src1, 0x80000001 | ((src2Value - 1) << 1)
    ////       CMP  s1, 1
    ////       BNE  $doesntDivideEqually
    //// s1  = ASR  src1, log2(src2Value)  -- do the equal divide
    //// dst = EOR   s1, 1                 -- restore tagged int bit
    ////       B  $done
    //// $doesntDivideEqually:
    ////       (now check if it divides with the remainder of 1, for which we can do integer divide and accommodate with +0.5
    ////       note that we need only the part that is to the left of p's power 2 bit)
    //// s1 =  AND  s1, 0x80000001 | (src2Value - 1)
    ////       CMP  s1, 1
    ////       BNE  $helper
    //// s1 =  ASR  src1, log2(src2Value) + 1 -- do the integer divide and also shift out the tagged int bit
    ////       PUSH 0xXXXXXXXX (ScriptContext)
    ////       PUSH s1
    //// dst = CALL Op_FinishOddDivByPow2     -- input: actual value, scriptContext; output: JavascriptNumber with 0.5 added to the input
    ////       JMP $done
    //// $helper:
    ////     ...
    //// $done:

    //if (instrDiv->GetSrc1()->IsRegOpnd() && instrDiv->GetSrc1()->AsRegOpnd()->m_sym->m_isNotInt)
    //{
    //    return;
    //}

    //IR::Opnd       *dst    = instrDiv->GetDst();
    //IR::Opnd       *src1   = instrDiv->GetSrc1();
    //IR::AddrOpnd   *src2   = instrDiv->GetSrc2()->IsAddrOpnd() ? instrDiv->GetSrc2()->AsAddrOpnd() : nullptr;
    //IR::LabelInstr *doesntDivideEqually = IR::LabelInstr::New(Js::OpCode::Label, m_func);
    //IR::LabelInstr *helper = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);
    //IR::LabelInstr *done   = IR::LabelInstr::New(Js::OpCode::Label, m_func);
    //IR::RegOpnd    *s1     = IR::RegOpnd::New(TyVar, m_func);
    //IR::Instr      *instr;

    //Assert(src2 && src2->IsVar() && Js::TaggedInt::Is(src2->m_address) && (Math::IsPow2(Js::TaggedInt::ToInt32(src2->m_address))));
    //int32          src2Value = Js::TaggedInt::ToInt32(src2->m_address);

    //// s1 =  AND  src1, 0x80000001 | ((src2Value - 1) << 1)
    //instr = IR::Instr::New(Js::OpCode::AND, s1, src1, IR::IntConstOpnd::New((0x80000001 | ((src2Value - 1) << 1)), TyInt32, m_func), m_func);
    //instrDiv->InsertBefore(instr);
    //LegalizeMD::LegalizeInstr(instr);

    ////       CMP s1, 1
    //instr = IR::Instr::New(Js::OpCode::CMP, m_func);
    //instr->SetSrc1(s1);
    //instr->SetSrc2(IR::IntConstOpnd::New(1, TyInt32, m_func));
    //instrDiv->InsertBefore(instr);

    ////       BNE  $doesntDivideEqually
    //instr = IR::BranchInstr::New(Js::OpCode::BNE, doesntDivideEqually, m_func);
    //instrDiv->InsertBefore(instr);

    //// s1  = ASR  src1, log2(src2Value)  -- do the equal divide
    //instr = IR::Instr::New(Js::OpCode::ASR, s1, src1, IR::IntConstOpnd::New(Math::Log2(src2Value), TyInt32, m_func), m_func);
    //instrDiv->InsertBefore(instr);
    //LegalizeMD::LegalizeInstr(instr);

    //// dst = ORR   s1, 1                 -- restore tagged int bit
    //instr = IR::Instr::New(Js::OpCode::ORR, dst, s1, IR::IntConstOpnd::New(1, TyInt32, m_func), m_func);
    //instrDiv->InsertBefore(instr);
    //LegalizeMD::LegalizeInstr(instr);
    //
    ////       B  $done
    //instr = IR::BranchInstr::New(Js::OpCode::B, done, m_func);
    //instrDiv->InsertBefore(instr);

    //// $doesntDivideEqually:
    //instrDiv->InsertBefore(doesntDivideEqually);

    //// s1 =  AND  s1, 0x80000001 | (src2Value - 1)
    //instr = IR::Instr::New(Js::OpCode::AND, s1, s1, IR::IntConstOpnd::New((0x80000001 | (src2Value - 1)), TyInt32, m_func), m_func);
    //instrDiv->InsertBefore(instr);

    ////       CMP  s1, 1
    //instr = IR::Instr::New(Js::OpCode::CMP, m_func);
    //instr->SetSrc1(s1);
    //instr->SetSrc2(IR::IntConstOpnd::New(1, TyInt32, m_func));
    //instrDiv->InsertBefore(instr);

    ////       BNE  $helper
    //instrDiv->InsertBefore(IR::BranchInstr::New(Js::OpCode::BNE, helper, m_func));

    //// s1 =  ASR  src1, log2(src2Value) + 1 -- do the integer divide and also shift out the tagged int bit
    //instr = IR::Instr::New(Js::OpCode::ASR, s1, src1, IR::IntConstOpnd::New(Math::Log2(src2Value) + 1, TyInt32, m_func), m_func);
    //instrDiv->InsertBefore(instr);
    //LegalizeMD::LegalizeInstr(instr);

    //// Arg2: scriptContext
    //IR::JnHelperMethod helperMethod;
    //if (instrDiv->dstIsTempNumber)
    //{
    //    // Var JavascriptMath::FinishOddDivByPow2_InPlace(uint32 value, ScriptContext *scriptContext, _Out_ JavascriptNumber* result)
    //    helperMethod = IR::HelperOp_FinishOddDivByPow2InPlace;
    //    Assert(dst->IsRegOpnd());
    //    StackSym * tempNumberSym = this->m_lowerer->GetTempNumberSym(dst, instr->dstIsTempNumberTransferred);

    //    instr = this->m_lowerer->InsertLoadStackAddress(tempNumberSym, instrDiv);
    //    LegalizeMD::LegalizeInstr(instr);

    //    this->LoadHelperArgument(instrDiv, instr->GetDst());
    //}
    //else
    //{
    //    // Var JavascriptMath::FinishOddDivByPow2(uint32 value, ScriptContext *scriptContext)
    //    helperMethod = IR::HelperOp_FinishOddDivByPow2;
    //}
    //this->m_lowerer->LoadScriptContext(instrDiv);

    //// Arg1: value
    //this->LoadHelperArgument(instrDiv, s1);

    //// dst = CALL Op_FinishOddDivByPow2     -- input: actual value, output: JavascriptNumber with 0.5 added to the input
    //instr = IR::Instr::New(Js::OpCode::Call, dst, IR::HelperCallOpnd::New(helperMethod, m_func), m_func);
    //instrDiv->InsertBefore(instr);
    //this->LowerCall(instr, 0);

    ////       JMP $done
    //instrDiv->InsertBefore(IR::BranchInstr::New(Js::OpCode::B, done, m_func));

    //// $helper:
    //instrDiv->InsertBefore(helper);

    //// $done:
    //instrDiv->InsertAfter(done);

    return;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastCmSrEqConst
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastCmSrXxConst(IR::Instr *instr)
{
    //
    // Given:
    // s1 = CmSrXX_A s2, s3
    // where either s2 or s3 is 'null', 'true' or 'false'
    //
    // Generate:
    //
    //     CMP s2, s3
    //     JEQ $mov_res
    //     MOV s1, eq ? Library.GetFalse() : Library.GetTrue()
    //     JMP $done
    // $mov_res:
    //     MOV s1, eq ? Library.GetTrue() : Library.GetFalse()
    // $done:
    //

    Assert(m_lowerer->IsConstRegOpnd(instr->GetSrc2()->AsRegOpnd()));
    return false;
}

void LowererMD::GenerateFastCmXxI4(IR::Instr *instr)
{
    this->GenerateFastCmXx(instr);
}

void LowererMD::GenerateFastCmXxR8(IR::Instr *instr)
{
    this->GenerateFastCmXx(instr);
}

void LowererMD::GenerateFastCmXx(IR::Instr *instr)
{
    // For float src:
    // LDIMM dst, trueResult
    // FCMP src1, src2
    // - BVS $done (NaN check iff B.cond is BNE)
    // B.cond $done
    // LDIMM dst, falseResult
    // $done

    // For Int src:
    // LDIMM dst, trueResult
    // CMP src1, src2
    // B.cond $done
    // LDIMM dst, falseResult
    // $done:

    IR::Opnd * src1 = instr->UnlinkSrc1();
    IR::Opnd * src2 = instr->UnlinkSrc2();
    IR::Opnd * dst = instr->UnlinkDst();
    // Decide the int (1/0) vs Var (ValueTrue/ValueFalse library pointers)
    // branch from the destination operand's type, not the symbol's type.
    // A Var-typed slot used for a 32-bit compare result has a TyInt32 opnd
    // wrapping a non-int32 sym; choosing the Var branch then LDIMMs a 64-bit
    // library address into the 32-bit destination, which LegalizeLDIMM
    // encodes as MOVK(LSL #32) on a W register - an unallocated encoding
    // (SIGILL on ARM64). Matches GenerateFastCmXxTaggedInt above.
    bool isIntDst = dst->IsInt32();
    bool isFloatSrc = src1->IsFloat();
    Assert(!isFloatSrc || src2->IsFloat());
    Assert(!src1->IsInt64() || src2->IsInt64());
    Assert(!isFloatSrc || AutoSystemInfo::Data.SSE2Available());
    Assert(src1->IsRegOpnd());
    IR::Opnd * opndTrue;
    IR::Opnd * opndFalse;
    IR::Instr * newInstr;
    IR::LabelInstr * done = IR::LabelInstr::New(Js::OpCode::Label, m_func);

    if (dst->IsEqual(src1))
    {
        IR::RegOpnd *newSrc1 = IR::RegOpnd::New(src1->GetType(), m_func);
        Lowerer::InsertMove(newSrc1, src1, instr);
        src1 = newSrc1;
    }

    if (dst->IsEqual(src2))
    {
        IR::RegOpnd *newSrc2 = IR::RegOpnd::New(src1->GetType(), m_func);
        Lowerer::InsertMove(newSrc2, src2, instr);
        src2 = newSrc2;
    }

    if (isIntDst)
    {
        opndTrue = IR::IntConstOpnd::New(1, TyInt32, this->m_func);
        opndFalse = IR::IntConstOpnd::New(0, TyInt32, this->m_func);
    }
    else
    {
        opndTrue = this->m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueTrue);
        opndFalse = this->m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueFalse);
    }

    Lowerer::InsertMove(dst, opndTrue, instr);

    // CMP src1, src2
    newInstr = IR::Instr::New(isFloatSrc ? Js::OpCode::FCMP : Js::OpCode::CMP, this->m_func);
    newInstr->SetSrc1(src1);
    newInstr->SetSrc2(src2);
    instr->InsertBefore(newInstr);
    LowererMD::Legalize(newInstr);

    bool addNaNCheck = false;
    Js::OpCode opcode = Js::OpCode::InvalidOpCode;

    switch (instr->m_opcode)
    {
        case Js::OpCode::CmEq_A:
        case Js::OpCode::CmSrEq_A:
        case Js::OpCode::CmEq_I4:
            opcode = Js::OpCode::BEQ;
            break;

        case Js::OpCode::CmNeq_A:
        case Js::OpCode::CmSrNeq_A:
        case Js::OpCode::CmNeq_I4:
            opcode = Js::OpCode::BNE;
            addNaNCheck = isFloatSrc;
            break;

        case Js::OpCode::CmGt_A:
        case Js::OpCode::CmGt_I4:
            opcode = Js::OpCode::BGT;
            break;

        case Js::OpCode::CmGe_A:
        case Js::OpCode::CmGe_I4:
            opcode = Js::OpCode::BGE;
            break;

        case Js::OpCode::CmLt_A:
        case Js::OpCode::CmLt_I4:
            //Can't use BLT  as is set when the operands are unordered (NaN).
            opcode = isFloatSrc ? Js::OpCode::BCC : Js::OpCode::BLT;
            break;

        case Js::OpCode::CmLe_A:
        case Js::OpCode::CmLe_I4:
            //Can't use BLE as it is set when the operands are unordered (NaN).
            opcode = isFloatSrc ? Js::OpCode::BLS : Js::OpCode::BLE;
            break;

        case Js::OpCode::CmUnGt_A:
        case Js::OpCode::CmUnGt_I4:
            opcode = Js::OpCode::BHI;
            break;

        case Js::OpCode::CmUnGe_A:
        case Js::OpCode::CmUnGe_I4:
            opcode = Js::OpCode::BCS;
            break;

        case Js::OpCode::CmUnLt_A:
        case Js::OpCode::CmUnLt_I4:
            opcode = Js::OpCode::BCC;
            break;

        case Js::OpCode::CmUnLe_A:
        case Js::OpCode::CmUnLe_I4:
            opcode = Js::OpCode::BLS;
            break;

        default: Assert(false);
    }

    if (addNaNCheck)
    {
        newInstr = IR::BranchInstr::New(Js::OpCode::BVS, done, m_func);
        instr->InsertBefore(newInstr);
    }

    newInstr = IR::BranchInstr::New(opcode, done, m_func);
    instr->InsertBefore(newInstr);

    Lowerer::InsertMove(dst, opndFalse, instr);
    instr->InsertBefore(done);
    instr->Remove();
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastCmXxTaggedInt
///
///----------------------------------------------------------------------------
bool LowererMD::GenerateFastCmXxTaggedInt(IR::Instr *instr, bool isInHelper  /* = false */)
{
    // The idea is to do an inline compare if we can prove that both sources
    // are tagged ints (i.e., are vars with the low bit set).
    //
    // Given:
    //
    //      Cmxx_A dst, src1, src2
    //
    // Generate:
    //
    // (If not Int31's, goto $helper)
    //      LDIMM dst, trueResult
    //      CMP src1, src2
    //      BEQ $fallthru
    //      LDIMM dst, falseResult
    //      B $fallthru
    // $helper:
    //      (caller will generate normal helper call sequence)
    // $fallthru:
    IR::Opnd * src1 = instr->GetSrc1();
    IR::Opnd * src2 = instr->GetSrc2();
    IR::Opnd * dst = instr->GetDst();
    IR::LabelInstr * helper = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);
    IR::LabelInstr * fallthru = IR::LabelInstr::New(Js::OpCode::Label, m_func, isInHelper);

    Assert(src1 && src2 && dst);

    // Not tagged ints?
    if (src1->IsRegOpnd() && src1->AsRegOpnd()->m_sym->m_isNotNumber)
    {
        return false;
    }
    if (src2->IsRegOpnd() && src2->AsRegOpnd()->m_sym->m_isNotNumber)
    {
        return false;
    }

    Js::OpCode opcode = Js::OpCode::InvalidOpCode;
    switch ( instr->m_opcode)
    {
        case Js::OpCode::CmEq_A:
        case Js::OpCode::CmSrEq_A:
        case Js::OpCode::CmEq_I4:
            opcode = Js::OpCode::BEQ;
            break;

        case Js::OpCode::CmNeq_A:
        case Js::OpCode::CmSrNeq_A:
        case Js::OpCode::CmNeq_I4:
            opcode = Js::OpCode::BNE;
            break;

        case Js::OpCode::CmGt_A:
        case Js::OpCode::CmGt_I4:
            opcode = Js::OpCode::BGT;
            break;

        case Js::OpCode::CmGe_A:
        case Js::OpCode::CmGe_I4:
            opcode = Js::OpCode::BGE;
            break;

        case Js::OpCode::CmLt_A:
        case Js::OpCode::CmLt_I4:
            opcode = Js::OpCode::BLT;
            break;

        case Js::OpCode::CmLe_A:
        case Js::OpCode::CmLe_I4:
            opcode = Js::OpCode::BLE;
            break;

        case Js::OpCode::CmUnGt_A:
        case Js::OpCode::CmUnGt_I4:
            opcode = Js::OpCode::BHI;
            break;

        case Js::OpCode::CmUnGe_A:
        case Js::OpCode::CmUnGe_I4:
            opcode = Js::OpCode::BCS;
            break;

        case Js::OpCode::CmUnLt_A:
        case Js::OpCode::CmUnLt_I4:
            opcode = Js::OpCode::BCC;
            break;

        case Js::OpCode::CmUnLe_A:
        case Js::OpCode::CmUnLe_I4:
            opcode = Js::OpCode::BLS;
            break;

        default: Assert(false);
    }

    // Tagged ints?
    bool isTaggedInts = false;
    if (src1->IsTaggedInt() || src1->IsInt32())
    {
        if (src2->IsTaggedInt() || src2->IsInt32())
        {
            isTaggedInts = true;
        }
    }

    if (!isTaggedInts)
    {
        this->GenerateSmIntPairTest(instr, src1, src2, helper);
    }

    if (dst->IsEqual(src1))
    {
        IR::RegOpnd *newSrc1 = IR::RegOpnd::New(TyMachReg, m_func);
        Lowerer::InsertMove(newSrc1, src1, instr);
        src1 = newSrc1;
    }

    if (dst->IsEqual(src2))
    {
        IR::RegOpnd *newSrc2 = IR::RegOpnd::New(TyMachReg, m_func);
        Lowerer::InsertMove(newSrc2, src2, instr);
        src2 = newSrc2;
    }

    IR::Opnd *opndTrue, *opndFalse;

    if (dst->IsInt32())
    {
        opndTrue = IR::IntConstOpnd::New(1, TyMachReg, this->m_func);
        opndFalse = IR::IntConstOpnd::New(0, TyMachReg, this->m_func);
    }
    else
    {
        opndTrue = m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueTrue);
        opndFalse = m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueFalse);
    }

    //      LDIMM dst, trueResult
    //      CMP src1, src2
    //      BEQ $fallthru
    //      LDIMM dst, falseResult
    //      B $fallthru

    src1 = src1->UseWithNewType(TyInt32, m_func);
    src2 = src2->UseWithNewType(TyInt32, m_func);

    instr->InsertBefore(IR::Instr::New(Js::OpCode::LDIMM, dst, opndTrue, m_func));
    IR::Instr *instrCmp = IR::Instr::New(Js::OpCode::CMP, m_func);
    instrCmp->SetSrc1(src1);
    instrCmp->SetSrc2(src2);
    instr->InsertBefore(instrCmp);
    LegalizeMD::LegalizeInstr(instrCmp);

    instr->InsertBefore(IR::BranchInstr::New(opcode, fallthru, m_func));
    instr->InsertBefore(IR::Instr::New(Js::OpCode::LDIMM, dst, opndFalse, m_func));

    if (isTaggedInts)
    {
        instr->InsertAfter(fallthru);
        instr->Remove();
        return true;
    }

    // B $fallthru
    instr->InsertBefore(IR::BranchInstr::New(Js::OpCode::B, fallthru, m_func));

    instr->InsertBefore(helper);
    instr->InsertAfter(fallthru);
    return false;
}

IR::Instr * LowererMD::GenerateConvBool(IR::Instr *instr)
{
    // dst = LDIMM true
    // TST src1, src2
    // BNE fallthrough
    // dst = LDIMM false
    // fallthrough:

    IR::RegOpnd *dst = instr->GetDst()->AsRegOpnd();
    IR::RegOpnd *src1 = instr->GetSrc1()->AsRegOpnd();
    IR::Opnd *opndTrue = m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueTrue);
    IR::Opnd *opndFalse = m_lowerer->LoadLibraryValueOpnd(instr, LibraryValue::ValueFalse);
    IR::LabelInstr *fallthru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    // dst = LDIMM true
    IR::Instr *instrFirst = IR::Instr::New(Js::OpCode::LDIMM, dst, opndTrue, m_func);
    instr->InsertBefore(instrFirst);

    // TST src1, src2
    IR::Instr *instrTst = IR::Instr::New(Js::OpCode::TST, m_func);
    instrTst->SetSrc1(src1);
    instrTst->SetSrc2(src1);
    instr->InsertBefore(instrTst);
    LegalizeMD::LegalizeInstr(instrTst);

    // BNE fallthrough
    instr->InsertBefore(IR::BranchInstr::New(Js::OpCode::BNE, fallthru, m_func));

    // dst = LDIMM false
    instr->InsertBefore(IR::Instr::New(Js::OpCode::LDIMM, dst, opndFalse, m_func));

    // fallthrough:
    instr->InsertAfter(fallthru);
    instr->Remove();

    return instrFirst;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastAdd
///
/// NOTE: We assume that only the sum of two Int31's will have 0x2 set. This
/// is only true until we have a var type with tag == 0x2.
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastAdd(IR::Instr * instrAdd)
{
    // Given:
    //
    // dst = Add src1, src2
    //
    // Generate:
    //
    // (If not 2 Int31's, jump to $helper.)
    // s1 = MOV src1
    // s1 = ADDS s1, src2   -- try an inline add
    //      BVS $helper     -- bail if the add overflowed
    // s1 = ORR s1, AtomTag_IntPtr
    // dst = MOV s1
    //      B $fallthru
    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    IR::Instr *      instr;
    IR::LabelInstr * labelHelper;
    IR::LabelInstr * labelFallThru;
    IR::Opnd *       opndReg;
    IR::Opnd *       opndSrc1;
    IR::Opnd *       opndSrc2;

    opndSrc1 = instrAdd->GetSrc1();
    opndSrc2 = instrAdd->GetSrc2();
    AssertMsg(opndSrc1 && opndSrc2, "Expected 2 src opnd's on Add instruction");

    // Generate fastpath for Incr_A anyway -
    // Incrementing strings representing integers can be inter-mixed with integers e.g. "1"++ -> converts 1 to an int and thereafter, integer increment is expected.
    if (opndSrc1->IsRegOpnd() && (opndSrc1->AsRegOpnd()->IsNotInt() || opndSrc1->GetValueType().IsString()
        || (instrAdd->m_opcode != Js::OpCode::Incr_A && opndSrc1->GetValueType().IsLikelyString())))
    {
        return false;
    }

    if (opndSrc2->IsRegOpnd() && (opndSrc2->AsRegOpnd()->IsNotInt() ||
        opndSrc2->GetValueType().IsLikelyString()))
    {
        return false;
    }

    // Tagged ints?
    bool isTaggedInts = false;
    if (opndSrc1->IsTaggedInt())
    {
        if (opndSrc2->IsTaggedInt())
        {
            isTaggedInts = true;
        }
    }

    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);

    if (!isTaggedInts)
    {
        // (If not 2 Int31's, jump to $helper.)

        this->GenerateSmIntPairTest(instrAdd, opndSrc1, opndSrc2, labelHelper);
    }

    if (opndSrc1->IsImmediateOpnd())
    {
        // If opnd1 is a constant, just swap them.
        IR::Opnd *opndTmp = opndSrc1;
        opndSrc1 = opndSrc2;
        opndSrc2 = opndTmp;
    }

    //
    // For 32 bit arithmetic we copy them and set the size of operands to be 32 bits. This is
    // relevant only on ARM64.
    //

    opndSrc1 = opndSrc1->UseWithNewType(TyInt32, this->m_func);
    opndSrc2 = opndSrc2->UseWithNewType(TyInt32, this->m_func);

    // s1 = MOV src1

    opndReg = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg, opndSrc1, instrAdd);
    
    // s1 = ADDS s1, src2

    instr = IR::Instr::New(Js::OpCode::ADDS, opndReg, opndReg, opndSrc2, this->m_func);
    instrAdd->InsertBefore(instr);
    Legalize(instr);

    //      BVS $helper

    instr = IR::BranchInstr::New(Js::OpCode::BVS, labelHelper, this->m_func);
    instrAdd->InsertBefore(instr);

    //
    // Convert TyInt32 operand, back to TyMachPtr type.
    //

    if(TyMachReg != opndReg->GetType())
    {
        opndReg = opndReg->UseWithNewType(TyMachPtr, this->m_func);
    }

    // s1 = ORR s1, AtomTag_IntPtr
    GenerateInt32ToVarConversion(opndReg, instrAdd);
    
    // dst = MOV s1

    instr = IR::Instr::New(Js::OpCode::MOV, instrAdd->GetDst(), opndReg, this->m_func);
    instrAdd->InsertBefore(instr);

    //      B $fallthru

    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrAdd->InsertBefore(instr);

    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    instrAdd->InsertBefore(labelHelper);
    instrAdd->InsertAfter(labelFallThru);

    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastSub
///
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastSub(IR::Instr * instrSub)
{
    // Given:
    //
    // dst = Sub src1, src2
    //
    // Generate:
    //
    // (If not 2 Int31's, jump to $helper.)
    // s1 = MOV src1
    // s1 = SUBS s1, src2   -- try an inline sub
    //      BVS $helper     -- bail if the subtract overflowed
    //      BNE $helper
    // s1 = ORR s1, AtomTag_IntPtr
    // dst = MOV s1
    //      B $fallthru
    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    IR::Instr *      instr;
    IR::LabelInstr * labelHelper;
    IR::LabelInstr * labelFallThru;
    IR::Opnd *       opndReg;
    IR::Opnd *       opndSrc1;
    IR::Opnd *       opndSrc2;

    opndSrc1        =   instrSub->GetSrc1();
    opndSrc2        =   instrSub->GetSrc2();
    AssertMsg(opndSrc1 && opndSrc2, "Expected 2 src opnd's on Sub instruction");

    // Not tagged ints?
    if (opndSrc1->IsRegOpnd() && opndSrc1->AsRegOpnd()->IsNotInt())
    {
        return false;
    }
    if (opndSrc2->IsRegOpnd() && opndSrc2->AsRegOpnd()->IsNotInt())
    {
        return false;
    }

    // Tagged ints?
    bool isTaggedInts = false;
    if (opndSrc1->IsTaggedInt())
    {
        if (opndSrc2->IsTaggedInt())
        {
            isTaggedInts = true;
        }
    }

    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);

    if (!isTaggedInts)
    {
        // (If not 2 Int31's, jump to $helper.)

        this->GenerateSmIntPairTest(instrSub, opndSrc1, opndSrc2, labelHelper);
    }

    //
    // For 32 bit arithmetic we copy them and set the size of operands to be 32 bits. This is
    // relevant only on ARM64.
    //

    opndSrc1    = opndSrc1->UseWithNewType(TyInt32, this->m_func);
    opndSrc2    = opndSrc2->UseWithNewType(TyInt32, this->m_func);

    // s1 = MOV src1

    opndReg = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg, opndSrc1, instrSub);

    // s1 = SUBS s1, src2

    instr = IR::Instr::New(Js::OpCode::SUBS, opndReg, opndReg, opndSrc2, this->m_func);
    instrSub->InsertBefore(instr);
    Legalize(instr);

    //      BVS $helper

    instr = IR::BranchInstr::New(Js::OpCode::BVS, labelHelper, this->m_func);
    instrSub->InsertBefore(instr);

    //
    // Convert TyInt32 operand, back to TyMachPtr type.
    //

    if(TyMachReg != opndReg->GetType())
    {
        opndReg = opndReg->UseWithNewType(TyMachPtr, this->m_func);
    }

    // s1 = ORR s1, AtomTag_IntPtr
    GenerateInt32ToVarConversion(opndReg, instrSub);

    // dst = MOV s1

    instr = IR::Instr::New(Js::OpCode::MOV, instrSub->GetDst(), opndReg, this->m_func);
    instrSub->InsertBefore(instr);

    //      B $fallthru

    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrSub->InsertBefore(instr);

    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    instrSub->InsertBefore(labelHelper);
    instrSub->InsertAfter(labelFallThru);

    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastMul
///
///----------------------------------------------------------------------------
bool
LowererMD::GenerateFastMul(IR::Instr * instrMul)
{
    // Given:
    //
    // dst = Mul src1, src2
    //
    // Generate:
    //
    // (If not 2 Int31's, jump to $helper.)
    // s1 = MOV src1
    // s2 = MOV src2
    // s3 = SMULL s1, s2    -- do the signed mul
    //      CMP s3, s3 SXTW
    //      BNE $helper     -- bail if the result overflowed
    //      CBZ s3, $zero   -- Check result is 0. might be -0. Result is -0 when a negative number is multiplied with 0.
    //      B $nonzero
    // $zero:               -- result of mul was 0. try to check for -0
    // s2 = ADDS s2, src1    --check for same sign
    //      BGE $nonzero     - positive 0 if signs are equal
    // dst = ToVar(-0.0)    -- load negative 0
    //      B $fallthru
    // $nonzero:
    // s3 = ORR s3, AtomTag_IntPtr
    // dst= MOV s3
    //      B $fallthru
    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    IR::LabelInstr * labelHelper;
    IR::LabelInstr * labelFallThru;
    IR::LabelInstr * labelNonZero;
    IR::Instr *      instr;
    IR::RegOpnd *    opndReg1;
    IR::RegOpnd *    opndReg2;
    IR::RegOpnd *    s3;
    IR::Opnd *       opndSrc1;
    IR::Opnd *       opndSrc2;

    opndSrc1 = instrMul->GetSrc1();
    opndSrc2 = instrMul->GetSrc2();
    AssertMsg(opndSrc1 && opndSrc2, "Expected 2 src opnd's on mul instruction");

    if (opndSrc1->IsRegOpnd() && opndSrc1->AsRegOpnd()->IsNotInt())
    {
        return true;
    }
    if (opndSrc2->IsRegOpnd() && opndSrc2->AsRegOpnd()->IsNotInt())
    {
        return true;
    }
    // (If not 2 Int31's, jump to $helper.)

    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
    labelNonZero = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    this->GenerateSmIntPairTest(instrMul, opndSrc1, opndSrc2, labelHelper);

    //
    // For 32 bit arithmetic we copy them and set the size of operands to be 32 bits. This is
    // relevant only on ARM64.
    //

    opndSrc1    = opndSrc1->UseWithNewType(TyInt32, this->m_func);
    opndSrc2    = opndSrc2->UseWithNewType(TyInt32, this->m_func);

    if (opndSrc1->IsImmediateOpnd())
    {
        IR::Opnd * temp = opndSrc1;
        opndSrc1 = opndSrc2;
        opndSrc2 = temp;
    }

    // s1 = MOV src1

    opndReg1 = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg1, opndSrc1, instrMul);

    // s2 = MOV src2

    opndReg2 = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg2, opndSrc2, instrMul);

    // s3 = SMULL s1, s2

    s3 = IR::RegOpnd::New(TyInt64, this->m_func);
    instr = IR::Instr::New(Js::OpCode::SMULL, s3, opndReg1, opndReg2, this->m_func);
    instrMul->InsertBefore(instr);

    //      CMP s3, s3 SXTW s3

    instr = IR::Instr::New(Js::OpCode::CMP_SXTW, this->m_func);
    instr->SetSrc1(s3);
    instr->SetSrc2(s3);
    instrMul->InsertBefore(instr);

    //      BNE $helper

    instr = IR::BranchInstr::New(Js::OpCode::BNE, labelHelper, this->m_func);
    instrMul->InsertBefore(instr);

    //      CBZ s3, $zero   -- Check result is 0. might be -0. Result is -0 when a negative number is multiplied with 0.

    IR::LabelInstr *labelZero = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
    instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelZero, this->m_func);
    instr->SetSrc1(s3);
    instrMul->InsertBefore(instr);

    //      B $nonzero

    instr = IR::BranchInstr::New(Js::OpCode::B, labelNonZero, this->m_func);
    instrMul->InsertBefore(instr);

    // $zero:

    instrMul->InsertBefore(labelZero);

    // s2 = ADDS s2, s1

    instr = IR::Instr::New(Js::OpCode::ADDS, opndReg2, opndReg2, opndReg1, this->m_func);
    instrMul->InsertBefore(instr);
    Legalize(instr);

    //      BGE $nonzero
    instr = IR::BranchInstr::New(Js::OpCode::BGE, labelNonZero, this->m_func);
    instrMul->InsertBefore(instr);

    // dst = ToVar(-0.0)    -- load negative 0

    instr = IR::Instr::New(Js::OpCode::LDIMM, instrMul->GetDst(), m_lowerer->LoadLibraryValueOpnd(instrMul, LibraryValue::ValueNegativeZero), this->m_func);
    instrMul->InsertBefore(instr);

    //      B $fallthru

    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrMul->InsertBefore(instr);

    // $nonzero:

    instrMul->InsertBefore(labelNonZero);

    // dst = MOV_TRUNC s3

    instr = IR::Instr::New(Js::OpCode::MOV_TRUNC, instrMul->GetDst()->UseWithNewType(TyInt32,this->m_func), s3->UseWithNewType(TyInt32, this->m_func), this->m_func);
    instrMul->InsertBefore(instr);

    // dst = OR dst, AtomTag_IntPtr

    GenerateInt32ToVarConversion(instrMul->GetDst(), instrMul);


    //      B $fallthru

    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrMul->InsertBefore(instr);

    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    instrMul->InsertBefore(labelHelper);
    instrMul->InsertAfter(labelFallThru);

    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastAnd
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastAnd(IR::Instr * instrAnd)
{
    // Left empty to match AMD64; assuming this is not performance critical
    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastOr
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastOr(IR::Instr * instrOr)
{
    // Left empty to match AMD64; assuming this is not performance critical
    return true;
}


///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastXor
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastXor(IR::Instr * instrXor)
{
    // Left empty to match AMD64; assuming this is not performance critical
    return true;
}

//----------------------------------------------------------------------------
//
// LowererMD::GenerateFastNot
//
//----------------------------------------------------------------------------

bool
LowererMD::GenerateFastNot(IR::Instr * instrNot)
{
    // Left empty to match AMD64; assuming this is not performance critical
    return true;
}

//
// If value is zero in tagged int representation, jump to $labelHelper.
//
void
LowererMD::GenerateTaggedZeroTest( IR::Opnd * opndSrc, IR::Instr * insertInstr, IR::LabelInstr * labelHelper )
{
    // Cast the var to 32 bit integer.
    if(opndSrc->GetSize() != 4)
    {
        opndSrc = opndSrc->UseWithNewType(TyUint32, this->m_func);
    }
    AssertMsg(TySize[opndSrc->GetType()] == 4, "This technique works only on the 32-bit version");

    if(labelHelper != nullptr)
    {
        // CBZ src1, $labelHelper
        IR::Instr* instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelHelper, this->m_func);
        instr->SetSrc1(opndSrc);
        insertInstr->InsertBefore(instr);
    }
    else
    {
        // TST src1, src1
        IR::Instr* instr = IR::Instr::New(Js::OpCode::TST, this->m_func);
        instr->SetSrc1(opndSrc);
        instr->SetSrc2(opndSrc);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);
    }
}

bool
LowererMD::GenerateFastNeg(IR::Instr * instrNeg)
{
    // Given:
    //
    // dst = Not src
    //
    // Generate:
    //
    //       if not int, jump $helper
    //       if src == 0     -- test for zero (must be handled by the runtime to preserve
    //       BEQ $helper     -- Difference between +0 and -0)
    // dst = SUB dst, 0, src -- do an inline NEG
    //       BVS $helper     -- bail if the subtract overflowed
    // dst = OR dst, tag     -- restore the var tag on the result
    //       BVS $helper
    //       B $fallthru
    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    IR::Instr *      instr;
    IR::LabelInstr * labelHelper = nullptr;
    IR::LabelInstr * labelFallThru = nullptr;
    IR::Opnd *       opndSrc1;
    IR::Opnd *       opndDst;
    bool usingNewDst = false;
    opndSrc1 = instrNeg->GetSrc1();
    AssertMsg(opndSrc1, "Expected src opnd on Neg instruction");


    if (opndSrc1->IsRegOpnd() && opndSrc1->AsRegOpnd()->m_sym->IsIntConst())
    {
        IR::Opnd *newOpnd;
        IntConstType value = opndSrc1->AsRegOpnd()->m_sym->GetIntConstValue();

        if (value == 0)
        {
            // If the negate operand is zero, the result is -0.0, which is a Number rather than an Int31.
            newOpnd = m_lowerer->LoadLibraryValueOpnd(instrNeg, LibraryValue::ValueNegativeZero);
        }
        else
        {
            // negation below can overflow because max negative int32 value > max positive value by 1.
            newOpnd = IR::AddrOpnd::NewFromNumber(-(int64)value, m_func);
        }

        instrNeg->ClearBailOutInfo();
        instrNeg->FreeSrc1();
        instrNeg->SetSrc1(newOpnd);
        instrNeg = this->ChangeToAssign(instrNeg);

        // Skip lowering call to helper
        return false;
    }

    bool isInt = (opndSrc1->IsTaggedInt());

    if (opndSrc1->IsRegOpnd() && opndSrc1->AsRegOpnd()->IsNotInt())
    {
        return true;
    }

    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
    if (!isInt)
    {
        GenerateSmIntTest(opndSrc1, instrNeg, labelHelper);
    }

    // For 32 bit arithmetic we copy them and set the size of operands to be 32 bits.
    opndSrc1 = opndSrc1->UseWithNewType(TyInt32, this->m_func);
    GenerateTaggedZeroTest(opndSrc1, instrNeg, labelHelper);

    if (opndSrc1->IsEqual(instrNeg->GetDst()))
    {
        usingNewDst = true;
        opndDst = IR::RegOpnd::New(TyInt32, this->m_func);
    }
    else
    {
        opndDst = instrNeg->GetDst()->UseWithNewType(TyInt32, this->m_func);
    }

    // dst = SUBS zr, src

    instr = IR::Instr::New(Js::OpCode::SUBS, opndDst, IR::RegOpnd::New(nullptr, RegZR, TyInt32, this->m_func), opndSrc1, this->m_func);
    instrNeg->InsertBefore(instr);

    // BVS $helper

    instr = IR::BranchInstr::New(Js::OpCode::BVS, labelHelper, this->m_func);
    instrNeg->InsertBefore(instr);

    //
    // Convert TyInt32 operand, back to TyMachPtr type.
    //
    if (TyMachReg != opndDst->GetType())
    {
        opndDst = opndDst->UseWithNewType(TyMachPtr, this->m_func);
    }

    GenerateInt32ToVarConversion(opndDst, instrNeg);

    if (usingNewDst)
    {
        Lowerer::InsertMove(instrNeg->GetDst(), opndDst, instrNeg);
    }

    // B $fallthru

    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrNeg->InsertBefore(instr);

    // $helper:
    //      (caller generates helper sequence)
    // $fallthru:

    AssertMsg(labelHelper, "Should not be NULL");
    instrNeg->InsertBefore(labelHelper);
    instrNeg->InsertAfter(labelFallThru);

    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastShiftLeft
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastShiftLeft(IR::Instr * instrShift)
{
    // Left empty to match AMD64; assuming this is not performance critical
    return true;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateFastShiftRight
///
///----------------------------------------------------------------------------

bool
LowererMD::GenerateFastShiftRight(IR::Instr * instrShift)
{
    // Given:
    //
    // dst = Shr/ShrU src1, src2
    //
    // Generate:
    //
    // (If not 2 Int31's, jump to $helper.)
    // s1 = MOV src1
    // s2 = MOV src2
    //      AND s2, 0x1F               [unsigned only]  // Bail if unsigned and not shifting,
    //      BEQ $helper                [unsigned only]  // as we may not end up with a taggable int
    // s1 = ASR/LSR s1, s2
    //      ORR s1, 1 << VarTag_Shift
    //dst = MOV s1
    //      B $fallthru
    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    IR::Instr *      instr;
    IR::LabelInstr * labelHelper;
    IR::LabelInstr * labelFallThru;
    IR::Opnd *       opndReg;
    IR::Opnd *       opndSrc1;
    IR::Opnd *       opndSrc2;
    Assert(instrShift->m_opcode == Js::OpCode::ShrU_A || instrShift->m_opcode == Js::OpCode::Shr_A);
    bool             isUnsigned = (instrShift->m_opcode == Js::OpCode::ShrU_A);

    opndSrc1 = instrShift->GetSrc1();
    opndSrc2 = instrShift->GetSrc2();
    AssertMsg(opndSrc1 && opndSrc2, "Expected 2 src opnd's on Add instruction");

    // Not int?
    if (opndSrc1->IsRegOpnd() && opndSrc1->AsRegOpnd()->IsNotInt())
    {
        return true;
    }
    if (opndSrc2->IsRegOpnd() && opndSrc2->AsRegOpnd()->IsNotInt())
    {
        return true;
    }

    // Tagged ints?
    bool isTaggedInts = false;
    if (opndSrc1->IsTaggedInt())
    {
        if (opndSrc2->IsTaggedInt())
        {
            isTaggedInts = true;
        }
    }

    IntConstType s2Value = 0;
    bool src2IsIntConst = false;

    if (isUnsigned)
    {
        if (opndSrc2->IsRegOpnd())
        {
            src2IsIntConst = opndSrc2->AsRegOpnd()->m_sym->IsTaggableIntConst();
            if (src2IsIntConst)
            {
                s2Value = opndSrc2->AsRegOpnd()->m_sym->GetIntConstValue();
            }
        }
        else
        {
            AssertMsg(opndSrc2->IsAddrOpnd() && Js::TaggedInt::Is(opndSrc2->AsAddrOpnd()->m_address),
                "Expect src2 of shift right to be reg or Var.");
            src2IsIntConst = true;
            s2Value = Js::TaggedInt::ToInt32(opndSrc2->AsAddrOpnd()->m_address);
        }

        // 32-bit Shifts only uses the bottom 5 bits.
        s2Value &=  0x1F;

        // Unsigned shift by 0 could yield a value not encodable as a tagged int.
        if (isUnsigned && src2IsIntConst && s2Value == 0)
        {
            return true;
        }
    }


    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);

    if (!isTaggedInts)
    {
        // (If not 2 Int31's, jump to $helper.)

        this->GenerateSmIntPairTest(instrShift, opndSrc1, opndSrc2, labelHelper);
    }

    opndSrc1    = opndSrc1->UseWithNewType(TyInt32, this->m_func);
    if (src2IsIntConst)
    {
        opndSrc2 = IR::IntConstOpnd::New(s2Value, TyInt32, this->m_func);
    }
    else
    {
        // s2 = MOV src2
        opndReg = IR::RegOpnd::New(TyInt32, this->m_func);
        Lowerer::InsertMove(opndReg, opndSrc2, instrShift);
        opndSrc2 = opndReg;
    }

    if (!src2IsIntConst && isUnsigned)
    {
        //      s2 = AND s2, 0x1F            [unsigned only]  // Bail if unsigned and not shifting,
        instr = IR::Instr::New(Js::OpCode::AND, opndSrc2, opndSrc2, IR::IntConstOpnd::New(0x1F, TyInt32, this->m_func), this->m_func);
        instrShift->InsertBefore(instr);

        //      CBZ s2, $helper              [unsigned only]  // as we may not end up with a taggable int
        instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelHelper, this->m_func);
        instr->SetSrc1(opndSrc2);
        instrShift->InsertBefore(instr);
    }

    // s1 = MOV src1
    opndReg = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg, opndSrc1, instrShift);

    // s1 = ASR/LSR s1, RCX
    instr = IR::Instr::New(isUnsigned ? Js::OpCode::LSR : Js::OpCode::ASR, opndReg, opndReg, opndSrc2, this->m_func);
    instrShift->InsertBefore(instr);

    //
    // Convert TyInt32 operand, back to TyMachPtr type.
    //

    if(TyMachReg != opndReg->GetType())
    {
        opndReg = opndReg->UseWithNewType(TyMachPtr, this->m_func);
    }

    //    ORR s1, 1 << VarTag_Shift
    this->GenerateInt32ToVarConversion(opndReg, instrShift);

    // dst = MOV s1

    instr = IR::Instr::New(Js::OpCode::MOV, instrShift->GetDst(), opndReg, this->m_func);
    instrShift->InsertBefore(instr);

    //      B $fallthru

    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, this->m_func);
    instrShift->InsertBefore(instr);

    // $helper:
    //      (caller generates helper call)
    // $fallthru:

    instrShift->InsertBefore(labelHelper);
    instrShift->InsertAfter(labelFallThru);

    return true;
}

void
LowererMD::GenerateFastBrS(IR::BranchInstr *brInstr)
{
    IR::Opnd *src1 = brInstr->UnlinkSrc1();

    Assert(src1->IsIntConstOpnd() || src1->IsAddrOpnd() || src1->IsRegOpnd());

    m_lowerer->InsertTest(
        m_lowerer->LoadOptimizationOverridesValueOpnd(
            brInstr, OptimizationOverridesValue::OptimizationOverridesSideEffects),
        src1,
        brInstr);

    Js::OpCode opcode;

    switch(brInstr->m_opcode)
    {
    case Js::OpCode::BrHasSideEffects:
        opcode = Js::OpCode::BNE;
        break;

    case Js::OpCode::BrNotHasSideEffects:
        opcode = Js::OpCode::BEQ;
        break;

    default:
        Assert(UNREACHED);
        __assume(false);
    }

    brInstr->m_opcode = opcode;
}

///----------------------------------------------------------------------------
///
/// LowererMD::GenerateSmIntPairTest
///
///     Generate code to test whether the given operands are both Int31 vars
/// and branch to the given label if not.
///
///----------------------------------------------------------------------------

IR::Instr *
LowererMD::GenerateSmIntPairTest(
    IR::Instr * instrInsert,
    IR::Opnd * opndSrc1,
    IR::Opnd * opndSrc2,
    IR::LabelInstr * labelFail)
{
    IR::Opnd *           opndReg;
    IR::Instr *          instrPrev = instrInsert->m_prev;
    IR::Instr *          instr;

    Assert(opndSrc1->GetType() == TyVar);
    Assert(opndSrc2->GetType() == TyVar);

    if (opndSrc1->IsTaggedInt())
    {
        IR::Opnd *tempOpnd = opndSrc1;
        opndSrc1 = opndSrc2;
        opndSrc2 = tempOpnd;
    }

    if (opndSrc2->IsTaggedInt())
    {
        if (opndSrc1->IsTaggedInt())
        {
            return instrPrev;
        }

        GenerateSmIntTest(opndSrc1, instrInsert, labelFail);
        return instrPrev;
    }

    opndReg = IR::RegOpnd::New(TyMachReg, this->m_func);

    // s1 = MOV src1
    // s1 = UBFX s1, VarTagShift - 16, 64 - (VarTag_Shift - 16)
    // s2 = MOV src2
    // s1 = BFXIL s2, VarTagShift, 64 - VarTag_Shift
    // s1 = EOR s1, AtomTag_Pair  ------ compare the tags together to the expected tag pair
    // CBNZ s1, $fail

    // s1 = MOV src1

    instr = IR::Instr::New(Js::OpCode::MOV, opndReg, opndSrc1, this->m_func);
    instrInsert->InsertBefore(instr);

    // s1 = UBFX s1, VarTagShift - 16, 64 - (VarTag_Shift - 16)

    instr = IR::Instr::New(Js::OpCode::UBFX, opndReg, opndReg, IR::IntConstOpnd::New(BITFIELD(Js::VarTag_Shift - 16, 64 - (Js::VarTag_Shift - 16)), TyMachReg, this->m_func), this->m_func);
    instrInsert->InsertBefore(instr);

    // s2 = MOV src2

    IR::Opnd * opndReg1 = IR::RegOpnd::New(TyMachReg, this->m_func);
    instr = IR::Instr::New(Js::OpCode::MOV, opndReg1, opndSrc2, this->m_func);
    instrInsert->InsertBefore(instr);

    // s1 = BFXIL s2, VarTagShift, 64 - VarTag_Shift

    instr = IR::Instr::New(Js::OpCode::BFXIL, opndReg, opndReg1, IR::IntConstOpnd::New(BITFIELD(Js::VarTag_Shift, 64 - Js::VarTag_Shift), TyMachReg, this->m_func), this->m_func);
    instrInsert->InsertBefore(instr);

    opndReg = opndReg->UseWithNewType(TyInt32, this->m_func)->AsRegOpnd();

    // s1 = EOR s1, AtomTag_Pair
    instr = IR::Instr::New(Js::OpCode::EOR, opndReg, opndReg, IR::IntConstOpnd::New(Js::AtomTag_Pair, TyInt32, this->m_func, true), this->m_func);
    instrInsert->InsertBefore(instr);

    //      CBNZ s1, $fail
    instr = IR::BranchInstr::New(Js::OpCode::CBNZ, labelFail, this->m_func);
    instr->SetSrc1(opndReg);
    instrInsert->InsertBefore(instr);

    return instrPrev;
}

bool LowererMD::GenerateObjectTest(IR::Opnd * opndSrc, IR::Instr * insertInstr, IR::LabelInstr * labelTarget, bool fContinueLabel)
{
    AssertMsg(opndSrc->GetSize() == MachPtr, "64-bit register required");

    if (opndSrc->IsTaggedValue() && fContinueLabel)
    {
        // Insert delete branch opcode to tell the dbChecks not to assert on the helper label we may fall through into
        IR::Instr *fakeBr = IR::PragmaInstr::New(Js::OpCode::DeletedNonHelperBranch, 0, this->m_func);
        insertInstr->InsertBefore(fakeBr);

        return false;
    }
    else if (opndSrc->IsNotTaggedValue() && !fContinueLabel)
    {
        return false;
    }

    IR::Opnd  * opndReg = IR::RegOpnd::New(TyMachReg, this->m_func);

    // s1 = MOV src1 - Move to a temporary
    IR::Instr * instr   = IR::Instr::New(Js::OpCode::MOV, opndReg, opndSrc, this->m_func);
    insertInstr->InsertBefore(instr);

    // s1 = UBFX s1, s1, #VarTag_Shift, #64 - VarTag_Shift
    instr = IR::Instr::New(Js::OpCode::UBFX, opndReg, opndReg, IR::IntConstOpnd::New(BITFIELD(Js::VarTag_Shift, 64 - Js::VarTag_Shift), TyMachReg, this->m_func), this->m_func);
    insertInstr->InsertBefore(instr);

    if (fContinueLabel)
    {
        // CBZ s1, $labelHelper
        instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelTarget, this->m_func);
        instr->SetSrc1(opndReg);
        insertInstr->InsertBefore(instr);
        IR::LabelInstr *labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
        insertInstr->InsertBefore(labelHelper);
    }
    else
    {
        // CBNZ s1, $labelHelper
        instr = IR::BranchInstr::New(Js::OpCode::CBNZ, labelTarget, this->m_func);
        instr->SetSrc1(opndReg);
        insertInstr->InsertBefore(instr);
    }
    return true;
}

void
LowererMD::GenerateLoadTaggedType(IR::Instr * instrLdSt, IR::RegOpnd * opndType, IR::RegOpnd * opndTaggedType)
{
    // taggedType = OR type, InlineCacheAuxSlotTypeTag
    IR::IntConstOpnd * opndAuxSlotTag = IR::IntConstOpnd::New(InlineCacheAuxSlotTypeTag, TyInt8, instrLdSt->m_func);
    IR::Instr * instr = IR::Instr::New(Js::OpCode::ORR, opndTaggedType, opndType, opndAuxSlotTag, instrLdSt->m_func);
    instrLdSt->InsertBefore(instr);
}

void
LowererMD::GenerateLoadPolymorphicInlineCacheSlot(IR::Instr * instrLdSt, IR::RegOpnd * opndInlineCache, IR::RegOpnd * opndType, uint polymorphicInlineCacheSize)
{
    // Generate
    //
    // LDR r1, type
    // LSR r1, r1, #PolymorphicInlineCacheShift
    // AND r1, r1, #(size - 1)
    // LSL r1, r1, #log2(sizeof(Js::InlineCache))
    // ADD inlineCache, inlineCache, r1

    // MOV r1, type
    IR::RegOpnd * opndOffset = IR::RegOpnd::New(TyMachPtr, instrLdSt->m_func);
    IR::Instr * instr = IR::Instr::New(Js::OpCode::MOV, opndOffset, opndType, instrLdSt->m_func);
    instrLdSt->InsertBefore(instr);

    IntConstType rightShiftAmount = PolymorphicInlineCacheShift;
    IntConstType leftShiftAmount = Math::Log2(sizeof(Js::InlineCache));
    // instead of generating
    // LSR r1, r1, #PolymorphicInlineCacheShift
    // AND r1, r1, #(size - 1)
    // LSL r1, r1, #log2(sizeof(Js::InlineCache))
    //
    // we can generate:
    // LSR r1, r1, #(PolymorphicInlineCacheShift - log2(sizeof(Js::InlineCache))
    // AND r1, r1, #(size - 1) << log2(sizeof(Js::InlineCache))
    Assert(rightShiftAmount > leftShiftAmount);
    instr = IR::Instr::New(Js::OpCode::LSR, opndOffset, opndOffset, IR::IntConstOpnd::New(rightShiftAmount - leftShiftAmount, TyUint8, instrLdSt->m_func, true), instrLdSt->m_func);
    instrLdSt->InsertBefore(instr);
    Lowerer::InsertAnd(opndOffset, opndOffset, IR::IntConstOpnd::New(((IntConstType)(polymorphicInlineCacheSize - 1)) << leftShiftAmount, TyMachPtr, instrLdSt->m_func, true), instrLdSt);

    // ADD inlineCache, inlineCache, r1
    Lowerer::InsertAdd(false, opndInlineCache, opndInlineCache, opndOffset, instrLdSt);
}

//----------------------------------------------------------------------------
//
// LowererMD::GenerateFastScopedFldLookup
//
// This is a helper call which generates asm for both
// ScopedLdFld & ScopedStFld
//
//----------------------------------------------------------------------------

IR::Instr *
LowererMD::GenerateFastScopedFld(IR::Instr * instrScopedFld, bool isLoad)
{
    //      LDR s1, [base, offset(length)]
    //      CMP s1, 1                                -- get the length on array and test if it is 1.
    //      BNE $helper
    //      LDR s2, [base, offset(scopes)]           -- load the first scope
    //      LDR s3, [s2, offset(type)]
    //      LDIMM s4, inlineCache
    //      LDR s5, [s4, offset(u.local.type)]
    //      CMP s3, s5                               -- check type
    //      BNE $helper
    //      LDR s6, [s2, offset(slots)]             -- load the slots array
    //      LDR s7 , [s4, offset(u.local.slotIndex)]        -- load the cached slot index
    //
    //  if (load) {
    //      LDR dst, [s6, s7, LSL #2]                       -- load the value from the slot
    //  }
    //  else {
    //      STR src, [s6, s7, LSL #2]
    //  }
    //      B $done
    //$helper:
    //      dst = BLX PatchGetPropertyScoped(inlineCache, base, field, defaultInstance, scriptContext)
    //$done:

    IR::Instr *         instr;
    IR::Instr *         instrPrev = instrScopedFld->m_prev;

    IR::RegOpnd *       opndBase;
    IR::RegOpnd *       opndReg1; //s1
    IR::RegOpnd *       opndReg2; //s2
    IR::RegOpnd *       opndInlineCache; //s4
    IR::IndirOpnd *     indirOpnd;
    IR::Opnd *          propertyBase;

    IR::LabelInstr *    labelHelper;
    IR::LabelInstr *    labelFallThru;

    if (isLoad)
    {
        propertyBase = instrScopedFld->GetSrc1();
    }
    else
    {
        propertyBase = instrScopedFld->GetDst();
    }

    AssertMsg(propertyBase->IsSymOpnd() && propertyBase->AsSymOpnd()->IsPropertySymOpnd() && propertyBase->AsSymOpnd()->m_sym->IsPropertySym(),
        "Expected property sym operand of ScopedLdFld or ScopedStFld");

    IR::PropertySymOpnd * propertySymOpnd = propertyBase->AsPropertySymOpnd();

    opndBase = propertySymOpnd->CreatePropertyOwnerOpnd(m_func);
    const IR::AutoReuseOpnd holdAfterLegalization(opndBase, m_func);
    AssertMsg(opndBase->m_sym->m_isSingleDef, "We assume this isn't redefined");

    labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);

    // LDR s1, [base, offset(length)]     -- get the length on array and test if it is 1.
    indirOpnd = IR::IndirOpnd::New(opndBase, Js::FrameDisplay::GetOffsetOfLength(), TyInt16, this->m_func);
    opndReg1 = IR::RegOpnd::New(TyInt32, this->m_func);
    Lowerer::InsertMove(opndReg1, indirOpnd, instrScopedFld);

    // CMP s1, 1                                -- get the length on array and test if it is 1.
    instr = IR::Instr::New(Js::OpCode::CMP,  this->m_func);
    instr->SetSrc1(opndReg1);
    instr->SetSrc2(IR::IntConstOpnd::New(0x1, TyInt8, this->m_func));
    instrScopedFld->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);

    // BNE $helper
    instr = IR::BranchInstr::New(Js::OpCode::BNE, labelHelper, this->m_func);
    instrScopedFld->InsertBefore(instr);

    // LDR s2, [base, offset(scopes)]           -- load the first scope
    indirOpnd = IR::IndirOpnd::New(opndBase, Js::FrameDisplay::GetOffsetOfScopes(), TyMachReg,this->m_func);
    opndReg2 = IR::RegOpnd::New(TyMachReg, this->m_func);
    Lowerer::InsertMove(opndReg2, indirOpnd, instrScopedFld);

    // LDR s3, [s2, offset(type)]
    // LDIMM s4, inlineCache
    // LDR s5, [s4, offset(u.local.type)]
    // CMP s3, s5                               -- check type
    // BNE $helper

    opndInlineCache = IR::RegOpnd::New(TyMachReg, this->m_func);
    opndReg2->m_sym->m_isNotNumber = true;

    IR::RegOpnd * opndType = IR::RegOpnd::New(TyMachReg, this->m_func);
    this->m_lowerer->GenerateObjectTestAndTypeLoad(instrScopedFld, opndReg2, opndType, labelHelper);
    Lowerer::InsertMove(opndInlineCache, m_lowerer->LoadRuntimeInlineCacheOpnd(instrScopedFld, propertySymOpnd), instrScopedFld);

    labelFallThru = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    // Check the local cache with the tagged type
    IR::RegOpnd * opndTaggedType = IR::RegOpnd::New(TyMachReg, this->m_func);
    GenerateLoadTaggedType(instrScopedFld, opndType, opndTaggedType);
    Lowerer::GenerateLocalInlineCacheCheck(instrScopedFld, opndTaggedType, opndInlineCache, labelHelper);
    if (isLoad)
    {
        IR::Opnd *opndDst = instrScopedFld->GetDst();
        Lowerer::GenerateLdFldFromLocalInlineCache(instrScopedFld, opndReg2, opndDst, opndInlineCache, labelFallThru, false);
    }
    else
    {
        IR::Opnd *opndSrc = instrScopedFld->GetSrc1();
        GenerateStFldFromLocalInlineCache(instrScopedFld, opndReg2, opndSrc, opndInlineCache, labelFallThru, false);
    }

    // $helper:
    // if (isLoad) {
    // dst = BLX PatchGetPropertyScoped(inlineCache, opndBase, propertyId, srcBase, scriptContext)
    // }
    // else {
    //  BLX PatchSetPropertyScoped(inlineCache, base, field, value, defaultInstance, scriptContext)
    // }
    // $fallthru:
    instrScopedFld->InsertBefore(labelHelper);
    instrScopedFld->InsertAfter(labelFallThru);

    return instrPrev;
}

//----------------------------------------------------------------------------
//
// LowererMD::GenerateFastScopedLdFld
//
// Make use of the helper to cache the type and slot index used to do a ScopedLdFld
// when the scope is an array of length 1.
// Extract the only element from array and do an inline load from the appropriate slot
// if the type hasn't changed since the last time this ScopedLdFld was executed.
//
//----------------------------------------------------------------------------
IR::Instr *
LowererMD::GenerateFastScopedLdFld(IR::Instr * instrLdScopedFld)
{
    //Helper GenerateFastScopedFldLookup generates following:
    //
    //      LDR s1, [base, offset(length)]
    //      CMP s1, 1                                -- get the length on array and test if it is 1.
    //      BNE $helper
    //      LDR s2, [base, offset(scopes)]           -- load the first scope
    //      LDR s3, [s2, offset(type)]
    //      LDIMM s4, inlineCache
    //      LDR s5, [s4, offset(u.local.type)]
    //      CMP s3, s5                               -- check type
    //      BNE $helper
    //      LDR s6, [s2, offset(slots)]             -- load the slots array
    //      LDR s7 , [s4, offset(u.local.slotIndex)]        -- load the cached slot index
    //      LDR dst, [s6, s7, LSL #2]                       -- load the value from the slot
    //      B $done
    //$helper:
    //     dst = BLX PatchGetPropertyScoped(inlineCache, base, field, defaultInstance, scriptContext)
    //$done:

    return GenerateFastScopedFld(instrLdScopedFld, true);
}

//----------------------------------------------------------------------------
//
// LowererMD::GenerateFastScopedStFld
//
// Make use of the helper to cache the type and slot index used to do a ScopedStFld
// when the scope is an array of length 1.
// Extract the only element from array and do an inline load from the appropriate slot
// if the type hasn't changed since the last time this ScopedStFld was executed.
//
//----------------------------------------------------------------------------
IR::Instr *
LowererMD::GenerateFastScopedStFld(IR::Instr * instrStScopedFld)
{
    //      LDR s1, [base, offset(length)]
    //      CMP s1, 1                                -- get the length on array and test if it is 1.
    //      BNE $helper
    //      LDR s2, [base, offset(scopes)]           -- load the first scope
    //      LDR s3, [s2, offset(type)]
    //      LDIMM s4, inlineCache
    //      LDR s5, [s4, offset(u.local.type)]
    //      CMP s3, s5                               -- check type
    //      BNE $helper
    //      LDR s6, [s2, offset(slots)]             -- load the slots array
    //      LDR s7 , [s4, offset(u.local.slotIndex)]        -- load the cached slot index
    //      STR src, [s6, s7, LSL #2]                       -- store the value directly at the slot
    //  B $done
    //$helper:
    //      BLX PatchSetPropertyScoped(inlineCache, base, field, value, defaultInstance, scriptContext)
    //$done:

    return GenerateFastScopedFld(instrStScopedFld, false);
}

void
LowererMD::GenerateStFldFromLocalInlineCache(
    IR::Instr * instrStFld,
    IR::RegOpnd * opndBase,
    IR::Opnd * opndSrc,
    IR::RegOpnd * opndInlineCache,
    IR::LabelInstr * labelFallThru,
    bool isInlineSlot)
{
    IR::RegOpnd * opndSlotArray = nullptr;
    IR::IndirOpnd * opndIndir;
    IR::Instr * instr;

    if (!isInlineSlot)
    {
        // s2 = MOV base->slots -- load the slot array
        opndSlotArray = IR::RegOpnd::New(TyMachReg, instrStFld->m_func);
        opndIndir = IR::IndirOpnd::New(opndBase, Js::DynamicObject::GetOffsetOfAuxSlots(), TyMachReg, instrStFld->m_func);
        Lowerer::InsertMove(opndSlotArray, opndIndir, instrStFld);
    }

    // LDR s5, [s2, offset(u.local.slotIndex)] -- load the cached slot index
    IR::RegOpnd *opndSlotIndex = IR::RegOpnd::New(TyUint16, instrStFld->m_func);
    opndIndir = IR::IndirOpnd::New(opndInlineCache, offsetof(Js::InlineCache, u.local.slotIndex), TyUint16, instrStFld->m_func);
    Lowerer::InsertMove(opndSlotIndex, opndIndir, instrStFld);

    // Store through the write-barrier assign (as in LowerMDShared): a raw
    // STR here is invisible to concurrent rescan on software-write-barrier
    // platforms and the stored target can be swept while still reachable.
    if (isInlineSlot)
    {
        // [base, s5, LSL #3] = src  -- store the value directly to the slot
        opndIndir = IR::IndirOpnd::New(opndBase, opndSlotIndex, LowererMD::GetDefaultIndirScale(), TyMachReg, instrStFld->m_func);
    }
    else
    {
        // [slotArray, s5, LSL #3] = src  -- store the value directly to the slot
        opndIndir = IR::IndirOpnd::New(opndSlotArray, opndSlotIndex, LowererMD::GetDefaultIndirScale(), TyMachReg, instrStFld->m_func);
    }
    GenerateWriteBarrierAssign(opndIndir, opndSrc, instrStFld);

    // B $done
    instr = IR::BranchInstr::New(Js::OpCode::B, labelFallThru, instrStFld->m_func);
    instrStFld->InsertBefore(instr);
}

IR::Opnd *
LowererMD::CreateStackArgumentsSlotOpnd(Func *func)
{
    // Save the newly-created args object to its dedicated stack slot.
    IR::IndirOpnd *indirOpnd = IR::IndirOpnd::New(IR::RegOpnd::New(nullptr, FRAME_REG , TyMachReg, func),
            -MachArgsSlotOffset, TyMachPtr, func);

    return indirOpnd;
}

//
// jump to $labelHelper, based on the result of CMP
//
void LowererMD::GenerateSmIntTest(IR::Opnd *opndSrc, IR::Instr *insertInstr, IR::LabelInstr *labelHelper, IR::Instr **instrFirst, bool fContinueLabel /* = false */)
{
    AssertMsg(opndSrc->GetSize() == MachPtr, "64-bit register required");

    IR::Opnd  * opndReg = IR::RegOpnd::New(TyMachReg, this->m_func);

    // s1 = MOV src1 - Move to a temporary
    IR::Instr * instr   = IR::Instr::New(Js::OpCode::MOV, opndReg, opndSrc, this->m_func);
    insertInstr->InsertBefore(instr);

    if (instrFirst)
    {
        *instrFirst = instr;
    }

    // s1 = UBFX s1, VarTag_Shift, 64 - VarTag_Shift
    instr = IR::Instr::New(Js::OpCode::UBFX, opndReg, opndReg, IR::IntConstOpnd::New(BITFIELD(Js::VarTag_Shift, 64 - Js::VarTag_Shift), TyMachReg, this->m_func), this->m_func);
    insertInstr->InsertBefore(instr);
    Legalize(instr);

    // s1 = EOR s1, AtomTag
    instr = IR::Instr::New(Js::OpCode::EOR, opndReg, opndReg, IR::IntConstOpnd::New(Js::AtomTag, TyInt32, this->m_func, /* dontEncode = */ true), this->m_func);
    insertInstr->InsertBefore(instr);

    if(fContinueLabel)
    {
        // CBZ s1, $labelHelper
        instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelHelper, this->m_func);
    }
    else
    {
        // CBNZ s1,  $labelHelper
        instr = IR::BranchInstr::New(Js::OpCode::CBNZ, labelHelper, this->m_func);
    }
    instr->SetSrc1(opndReg);
    insertInstr->InsertBefore(instr);
}

void LowererMD::GenerateInt32ToVarConversion(IR::Opnd * opndSrc, IR::Instr * insertInstr )
{
    AssertMsg(opndSrc->IsRegOpnd(), "NYI for other types");

    IR::Instr* instr = IR::Instr::New(Js::OpCode::ORR, opndSrc, opndSrc, IR::IntConstOpnd::New(Js::AtomTag_IntPtr, TyMachReg, this->m_func), this->m_func);
    insertInstr->InsertBefore(instr);
}

IR::RegOpnd *
LowererMD::GenerateUntagVar(IR::RegOpnd * src, IR::LabelInstr * labelFail, IR::Instr * assignInstr, bool generateTagCheck)
{
    Assert(src->IsVar());

    //  MOV valueOpnd, index
    IR::RegOpnd *valueOpnd = IR::RegOpnd::New(TyInt32, this->m_func);

    //
    // Convert Index to 32 bits.
    //
    if (generateTagCheck)
    {
        IR::Opnd * opnd = src->UseWithNewType(TyMachReg, this->m_func);
        Assert(!opnd->IsTaggedInt());
        this->GenerateSmIntTest(opnd, assignInstr, labelFail);
    }

    // Doing a 32-bit MOV clears the tag bits on ARM64. Use MOV_TRUNC so it doesn't get peeped away.
    IR::Instr * instr = IR::Instr::New(Js::OpCode::MOV_TRUNC, valueOpnd, src->UseWithNewType(TyInt32, this->m_func), this->m_func);
    assignInstr->InsertBefore(instr);
    return valueOpnd;
}

IR::RegOpnd *LowererMD::LoadNonnegativeIndex(
    IR::RegOpnd *indexOpnd,
    const bool skipNegativeCheck,
    IR::LabelInstr *const notTaggedIntLabel,
    IR::LabelInstr *const negativeLabel,
    IR::Instr *const insertBeforeInstr)
{
    Assert(indexOpnd);
    Assert(indexOpnd->IsVar() || indexOpnd->GetType() == TyInt32 || indexOpnd->GetType() == TyUint32);
    Assert(indexOpnd->GetType() != TyUint32 || skipNegativeCheck);
    Assert(!indexOpnd->IsVar() || notTaggedIntLabel);
    Assert(skipNegativeCheck || negativeLabel);
    Assert(insertBeforeInstr);

    if(indexOpnd->IsVar())
    {
        if (indexOpnd->GetValueType().IsLikelyFloat())
        {
            return m_lowerer->LoadIndexFromLikelyFloat(indexOpnd, skipNegativeCheck, notTaggedIntLabel, negativeLabel, insertBeforeInstr);
        }

        indexOpnd = GenerateUntagVar(indexOpnd, notTaggedIntLabel, insertBeforeInstr, !indexOpnd->IsTaggedInt());
    }

    if(!skipNegativeCheck)
    {
        //     TBNZ index, #31, $notTaggedIntOrNegative
        IR::Instr *instr = IR::BranchInstr::New(Js::OpCode::TBNZ, negativeLabel, this->m_func);
        instr->SetSrc1(indexOpnd);
        instr->SetSrc2(IR::IntConstOpnd::New(31, TyVar, this->m_func));
        insertBeforeInstr->InsertBefore(instr);
    }
    return indexOpnd;
}

// Inlines fast-path for int Mul/Add or int Mul/Sub.  If not int, call MulAdd/MulSub helper
bool LowererMD::TryGenerateFastMulAdd(IR::Instr * instrAdd, IR::Instr ** pInstrPrev)
{
    IR::Instr *instrMul = instrAdd->GetPrevRealInstrOrLabel();
    IR::Opnd *addSrc;
    IR::RegOpnd *addCommonSrcOpnd;

    Assert(instrAdd->m_opcode == Js::OpCode::Add_A || instrAdd->m_opcode == Js::OpCode::Sub_A);

    bool isSub = (instrAdd->m_opcode == Js::OpCode::Sub_A) ? true : false;

    // Mul needs to be a single def reg
    if (instrMul->m_opcode != Js::OpCode::Mul_A || instrMul->GetDst()->IsRegOpnd() == false)
    {
        // Cannot generate MulAdd
        return false;
    }

    if (instrMul->HasBailOutInfo())
    {
        // Bailout will be generated for the Add, but not the Mul.
        // We could handle this, but this path isn't used that much anymore.
        return false;
    }

    IR::RegOpnd *regMulDst = instrMul->GetDst()->AsRegOpnd();

    if (regMulDst->m_sym->m_isSingleDef == false)
    {
        // Cannot generate MulAdd
        return false;
    }

    // Only handle a * b + c, so dst of Mul needs to match left source of Add
    if (instrMul->GetDst()->IsEqual(instrAdd->GetSrc1()))
    {
        addCommonSrcOpnd = instrAdd->GetSrc1()->AsRegOpnd();
        addSrc = instrAdd->GetSrc2();
    }
    else if (instrMul->GetDst()->IsEqual(instrAdd->GetSrc2()))
    {
        addSrc = instrAdd->GetSrc1();
        addCommonSrcOpnd = instrAdd->GetSrc2()->AsRegOpnd();
    }
    else
    {
        return false;
    }

    // Only handle a * b + c where c != a * b
    if (instrAdd->GetSrc1()->IsEqual(instrAdd->GetSrc2()))
    {
        return false;
    }

    if (addCommonSrcOpnd->m_isTempLastUse == false)
    {
        return false;
    }

    IR::Opnd *mulSrc1 = instrMul->GetSrc1();
    IR::Opnd *mulSrc2 = instrMul->GetSrc2();

    if (mulSrc1->IsRegOpnd() && mulSrc1->AsRegOpnd()->IsTaggedInt()
        && mulSrc2->IsRegOpnd() && mulSrc2->AsRegOpnd()->IsTaggedInt())
    {
        return false;
    }

    // Save prevInstr for the main lower loop
    *pInstrPrev = instrMul->m_prev;

    // Generate int31 fast-path for Mul, go to MulAdd helper if it fails, or one of the source is marked notInt
    if (!(addSrc->IsRegOpnd() && addSrc->AsRegOpnd()->IsNotInt())
        && !(mulSrc1->IsRegOpnd() && mulSrc1->AsRegOpnd()->IsNotInt())
        && !(mulSrc2->IsRegOpnd() && mulSrc2->AsRegOpnd()->IsNotInt()))
    {
        this->GenerateFastMul(instrMul);

        IR::LabelInstr *labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
        IR::Instr *instr = IR::BranchInstr::New(Js::OpCode::B, labelHelper, this->m_func);
        instrMul->InsertBefore(instr);

        // Generate int31 fast-path for Add
        bool success;
        if (isSub)
        {
            success = this->GenerateFastSub(instrAdd);
        }
        else
        {
            success = this->GenerateFastAdd(instrAdd);
        }

        if (!success)
        {
            labelHelper->isOpHelper = false;
        }
        // Generate MulAdd helper call
        instrAdd->InsertBefore(labelHelper);
    }

    if (instrAdd->dstIsTempNumber)
    {
        m_lowerer->LoadHelperTemp(instrAdd, instrAdd);
    }
    else
    {
        IR::Opnd *tempOpnd = IR::IntConstOpnd::New(0, TyInt32, this->m_func);
        this->LoadHelperArgument(instrAdd, tempOpnd);
    }
    this->m_lowerer->LoadScriptContext(instrAdd);

    IR::JnHelperMethod helper;

    if (addSrc == instrAdd->GetSrc2())
    {
        instrAdd->FreeSrc1();
        IR::Opnd *addOpnd = instrAdd->UnlinkSrc2();
        this->LoadHelperArgument(instrAdd, addOpnd);
        helper = isSub ? IR::HelperOp_MulSubRight : IR::HelperOp_MulAddRight;
    }
    else
    {
        instrAdd->FreeSrc2();
        IR::Opnd *addOpnd = instrAdd->UnlinkSrc1();
        this->LoadHelperArgument(instrAdd, addOpnd);
        helper = isSub ? IR::HelperOp_MulSubLeft : IR::HelperOp_MulAddLeft;
    }

    IR::Opnd *src2 = instrMul->UnlinkSrc2();
    this->LoadHelperArgument(instrAdd, src2);

    IR::Opnd *src1 = instrMul->UnlinkSrc1();
    this->LoadHelperArgument(instrAdd, src1);

    this->ChangeToHelperCall(instrAdd, helper);

    instrMul->Remove();

    return true;
}

IR::Instr *
LowererMD::LoadCheckedFloat(
    IR::RegOpnd *opndOrig,
    IR::RegOpnd *opndFloat,
    IR::LabelInstr *labelInline,
    IR::LabelInstr *labelHelper,
    IR::Instr *instrInsert,
    const bool checkForNullInLoopBody)
{
    //
    //   if (TaggedInt::Is(opndOrig))
    //       opndFloat = FCVT opndOrig_32
    //                   B $labelInline
    //   else
    //                   B $labelOpndIsNotInt
    //
    // $labelOpndIsNotInt:
    //   if (TaggedFloat::Is(opndOrig))
    //       s2        = MOV opndOrig
    //       s2        = EOR FloatTag_Value
    //       opndFloat = FCVT s2
    //   else
    //                   B $labelHelper
    //
    // $labelInline:
    //

    IR::Instr *instrFirst = nullptr;

    IR::LabelInstr *labelOpndIsNotInt = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    GenerateSmIntTest(opndOrig, instrInsert, labelOpndIsNotInt, &instrFirst);

    if (opndOrig->GetValueType().IsLikelyFloat())
    {
        // Make this path helper if value is likely a float
        instrInsert->InsertBefore(IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true));
    }

    IR::Opnd    *opndOrig_32 = opndOrig->UseWithNewType(TyInt32, this->m_func);
    EmitIntToFloat(opndFloat, opndOrig_32, instrInsert);

    IR::Instr   *jmpInline = IR::BranchInstr::New(Js::OpCode::B, labelInline, this->m_func);
    instrInsert->InsertBefore(jmpInline);

    instrInsert->InsertBefore(labelOpndIsNotInt);

    GenerateFloatTest(opndOrig, instrInsert, labelHelper, checkForNullInLoopBody);

    IR::RegOpnd *s2 = IR::RegOpnd::New(TyMachReg, this->m_func);
    IR::Instr   *mov = IR::Instr::New(Js::OpCode::MOV, s2, opndOrig, this->m_func);
    instrInsert->InsertBefore(mov);

    IR::Instr   *eorTag = IR::Instr::New(Js::OpCode::EOR,
        s2,
        s2,
        IR::IntConstOpnd::New(Js::FloatTag_Value,
            TyMachReg,
            this->m_func,
            /* dontEncode = */ true),
        this->m_func);
    instrInsert->InsertBefore(eorTag);

    IR::Instr   *movFloat = IR::Instr::New(Js::OpCode::FMOV_GEN, opndFloat, s2, this->m_func);
    instrInsert->InsertBefore(movFloat);

    return instrFirst;
}

void
LowererMD::EmitLoadFloatFromNumber(IR::Opnd *dst, IR::Opnd *src, IR::Instr *insertInstr)
{
    IR::LabelInstr *labelDone;
    IR::Instr *instr;
    labelDone = EmitLoadFloatCommon(dst, src, insertInstr, insertInstr->HasBailOutInfo());

    if (labelDone == nullptr)
    {
        // We're done
        insertInstr->Remove();
        return;
    }

    // $Done   note: insertAfter
    insertInstr->InsertAfter(labelDone);

    if (!insertInstr->HasBailOutInfo())
    {
        // $Done
        insertInstr->Remove();
        return;
    }

    IR::LabelInstr *labelNoBailOut = nullptr;
    IR::SymOpnd *tempSymOpnd = nullptr;

    if (insertInstr->GetBailOutKind() == IR::BailOutPrimitiveButString)
    {
        if (!this->m_func->tempSymDouble)
        {
            this->m_func->tempSymDouble = StackSym::New(TyFloat64, this->m_func);
            this->m_func->StackAllocate(this->m_func->tempSymDouble, MachDouble);
        }

        // LEA r3, tempSymDouble
        IR::RegOpnd *reg3Opnd = IR::RegOpnd::New(TyMachReg, this->m_func);
        tempSymOpnd = IR::SymOpnd::New(this->m_func->tempSymDouble, TyFloat64, this->m_func);
        Lowerer::InsertLea(reg3Opnd, tempSymOpnd, insertInstr);

        // regBoolResult = to_number_fromPrimitive(value, &dst, allowUndef, scriptContext);

        this->m_lowerer->LoadScriptContext(insertInstr);
        IR::IntConstOpnd *allowUndefOpnd;
        if (insertInstr->GetBailOutKind() == IR::BailOutPrimitiveButString)
        {
            allowUndefOpnd = IR::IntConstOpnd::New(true, TyInt32, this->m_func);
        }
        else
        {
            Assert(insertInstr->GetBailOutKind() == IR::BailOutNumberOnly);
            allowUndefOpnd = IR::IntConstOpnd::New(false, TyInt32, this->m_func);
        }
        this->LoadHelperArgument(insertInstr, allowUndefOpnd);
        this->LoadHelperArgument(insertInstr, reg3Opnd);
        this->LoadHelperArgument(insertInstr, src);

        IR::RegOpnd *regBoolResult = IR::RegOpnd::New(TyInt32, this->m_func);
        instr = IR::Instr::New(Js::OpCode::Call, regBoolResult, IR::HelperCallOpnd::New(IR::HelperOp_ConvNumber_FromPrimitive, this->m_func), this->m_func);
        insertInstr->InsertBefore(instr);

        this->LowerCall(instr, 0);

        // TEST regBoolResult, regBoolResult
        instr = IR::Instr::New(Js::OpCode::TST, this->m_func);
        instr->SetSrc1(regBoolResult);
        instr->SetSrc2(regBoolResult);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);

        // BNE $noBailOut
        labelNoBailOut = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
        instr = IR::BranchInstr::New(Js::OpCode::BNE, labelNoBailOut, this->m_func);
        insertInstr->InsertBefore(instr);
    }

    // Bailout code
    Assert(insertInstr->m_opcode == Js::OpCode::FromVar);
    insertInstr->UnlinkDst();
    insertInstr->FreeSrc1();
    IR::Instr *bailoutInstr = insertInstr;
    insertInstr = bailoutInstr->m_next;
    this->m_lowerer->GenerateBailOut(bailoutInstr);

    // $noBailOut
    if (labelNoBailOut)
    {
        insertInstr->InsertBefore(labelNoBailOut);

        Assert(dst->IsRegOpnd());

        // VLDR dst, [pResult].f64
        instr = IR::Instr::New(Js::OpCode::FLDR, dst, tempSymOpnd, this->m_func);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);
    }
}

IR::LabelInstr*
LowererMD::EmitLoadFloatCommon(IR::Opnd *dst, IR::Opnd *src, IR::Instr *insertInstr, bool needHelperLabel)
{
    IR::Instr *instr;

    Assert(src->GetType() == TyVar);
    Assert(dst->GetType() == TyFloat64 || TyFloat32);
    bool isFloatConst = false;
    IR::RegOpnd *regFloatOpnd = nullptr;

    if (src->IsRegOpnd() && src->AsRegOpnd()->m_sym->m_isFltConst)
    {
        IR::RegOpnd *regOpnd = src->AsRegOpnd();
        Assert(regOpnd->m_sym->m_isSingleDef);

        Js::Var value = regOpnd->m_sym->GetFloatConstValueAsVar_PostGlobOpt();
        IR::MemRefOpnd *memRef = IR::MemRefOpnd::New((BYTE*)value + Js::JavascriptNumber::GetValueOffset(), TyFloat64, this->m_func, IR::AddrOpndKindDynamicDoubleRef);
        regFloatOpnd = IR::RegOpnd::New(TyFloat64, this->m_func);
        instr = IR::Instr::New(Js::OpCode::FLDR, regFloatOpnd, memRef, this->m_func);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);
        isFloatConst = true;
    }
    // Src is constant?
    if (src->IsImmediateOpnd() || src->IsFloatConstOpnd())
    {
        regFloatOpnd = IR::RegOpnd::New(TyFloat64, this->m_func);
        m_lowerer->LoadFloatFromNonReg(src, regFloatOpnd, insertInstr);
        isFloatConst = true;
    }
    if (isFloatConst)
    {
        if (dst->GetType() == TyFloat32)
        {
            // FCVT.F32.F64 regOpnd32.f32, regOpnd.f64    -- Convert regOpnd from f64 to f32
            IR::RegOpnd *regOpnd32 = regFloatOpnd->UseWithNewType(TyFloat32, this->m_func)->AsRegOpnd();
            instr = IR::Instr::New(Js::OpCode::FCVT, regOpnd32, regFloatOpnd, this->m_func);
            insertInstr->InsertBefore(instr);

            // FMOV dst, regOpnd32
            instr = IR::Instr::New(Js::OpCode::FMOV, dst, regOpnd32, this->m_func);
            insertInstr->InsertBefore(instr);
        }
        else
        {
            instr = IR::Instr::New(Js::OpCode::FMOV, dst, regFloatOpnd, this->m_func);
            insertInstr->InsertBefore(instr);
        }
        LegalizeMD::LegalizeInstr(instr);
        return nullptr;
    }
    Assert(src->IsRegOpnd());

    IR::LabelInstr *labelStore = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    IR::LabelInstr *labelHelper;
    IR::LabelInstr *labelDone = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    if (needHelperLabel)
    {
        labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
    }
    else
    {
        labelHelper = labelDone;
    }
    IR::RegOpnd *reg2 = IR::RegOpnd::New(TyMachDouble, this->m_func);
    // Load the float value in reg2
    this->LoadCheckedFloat(src->AsRegOpnd(), reg2, labelStore, labelHelper, insertInstr, needHelperLabel);

    // $Store
    insertInstr->InsertBefore(labelStore);
    if (dst->GetType() == TyFloat32)
    {
        IR::RegOpnd *reg2_32 = reg2->UseWithNewType(TyFloat32, this->m_func)->AsRegOpnd();
        // FCVT.F32.F64 r2_32.f32, r2.f64    -- Convert regOpnd from f64 to f32
        instr = IR::Instr::New(Js::OpCode::FCVT, reg2_32, reg2, this->m_func);
        insertInstr->InsertBefore(instr);

        // FMOV dst, r2_32
        instr = IR::Instr::New(Js::OpCode::FMOV, dst, reg2_32, this->m_func);
        insertInstr->InsertBefore(instr);
    }
    else
    {
        // FMOV dst, r2
        instr = IR::Instr::New(Js::OpCode::FMOV, dst, reg2, this->m_func);
        insertInstr->InsertBefore(instr);
    }
    LegalizeMD::LegalizeInstr(instr);

    // B $Done
    instr = IR::BranchInstr::New(Js::OpCode::B, labelDone, this->m_func);
    insertInstr->InsertBefore(instr);

    if (needHelperLabel)
    {
        // $Helper
        insertInstr->InsertBefore(labelHelper);
    }

    return labelDone;
}

void
LowererMD::EmitLoadFloat(IR::Opnd *dst, IR::Opnd *src, IR::Instr *insertInstr, IR::Instr * instrBailOut, IR::LabelInstr * labelBailOut)
{
    IR::LabelInstr *labelDone;
    IR::Instr *instr;

    Assert(src->GetType() == TyVar);
    Assert(dst->GetType() == TyFloat64 || TyFloat32);
    Assert(src->IsRegOpnd());

    labelDone = EmitLoadFloatCommon(dst, src, insertInstr, true);

    if (labelDone == nullptr)
    {
        // We're done
        return;
    }

    IR::BailOutKind bailOutKind = instrBailOut && instrBailOut->HasBailOutInfo() ? instrBailOut->GetBailOutKind() : IR::BailOutInvalid;

    if (bailOutKind & IR::BailOutOnArrayAccessHelperCall)
    {
        // Bail out instead of making the helper call.
        Assert(labelBailOut);
        m_lowerer->InsertBranch(Js::OpCode::Br, labelBailOut, insertInstr);
        insertInstr->InsertBefore(labelDone);
        return;
    }

    IR::Opnd *memAddress = dst;
    if (dst->IsRegOpnd())
    {
        IR::SymOpnd *symOpnd = nullptr;
        if (dst->GetType() == TyFloat32)
        {
            symOpnd = IR::SymOpnd::New(StackSym::New(TyFloat32, this->m_func), TyFloat32, this->m_func);
            this->m_func->StackAllocate(symOpnd->m_sym->AsStackSym(), sizeof(float));
        }
        else
        {
            symOpnd = IR::SymOpnd::New(StackSym::New(TyFloat64,this->m_func), TyMachDouble, this->m_func);
            this->m_func->StackAllocate(symOpnd->m_sym->AsStackSym(), sizeof(double));
        }
        memAddress = symOpnd;
    }

    // LEA r3, dst
    IR::RegOpnd *reg3Opnd = IR::RegOpnd::New(TyMachReg, this->m_func);
    Lowerer::InsertLea(reg3Opnd, memAddress, insertInstr);

    // to_number_full(value, &dst, scriptContext);
    // Create dummy binary op to convert into helper

    instr = IR::Instr::New(Js::OpCode::Add_A, this->m_func);
    instr->SetSrc1(src);
    instr->SetSrc2(reg3Opnd);
    insertInstr->InsertBefore(instr);

    if (BailOutInfo::IsBailOutOnImplicitCalls(bailOutKind))
    {
        _Analysis_assume_(instrBailOut != nullptr);
        instr = instr->ConvertToBailOutInstr(instrBailOut->GetBailOutInfo(), bailOutKind);
        if (instrBailOut->GetBailOutInfo()->bailOutInstr == instrBailOut)
        {
            IR::Instr * instrShare = instrBailOut->ShareBailOut();
            m_lowerer->LowerBailTarget(instrShare);
        }
    }

    IR::JnHelperMethod helper;
    if (dst->GetType() == TyFloat32)
    {
        helper = IR::HelperOp_ConvFloat_Helper;
    }
    else
    {
        helper = IR::HelperOp_ConvNumber_Helper;
    }

    this->m_lowerer->LowerBinaryHelperMem(instr, helper);

    if (dst->IsRegOpnd())
    {
        instr = IR::Instr::New(Js::OpCode::FLDR, dst , memAddress, this->m_func);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);
    }

    // $Done
    insertInstr->InsertBefore(labelDone);
}

void
LowererMD::GenerateFastRecyclerAlloc(size_t allocSize, IR::RegOpnd* newObjDst, IR::Instr* insertionPointInstr, IR::LabelInstr* allocHelperLabel, IR::LabelInstr* allocDoneLabel)
{
    ScriptContextInfo* scriptContext = this->m_func->GetScriptContextInfo();
    void* allocatorAddress;
    uint32 endAddressOffset;
    uint32 freeListOffset;
    size_t alignedSize = HeapInfo::GetAlignedSizeNoCheck(allocSize);

    bool allowNativeCodeBumpAllocation = scriptContext->GetRecyclerAllowNativeCodeBumpAllocation();
    Recycler::GetNormalHeapBlockAllocatorInfoForNativeAllocation((void*)scriptContext->GetRecyclerAddr(), alignedSize,
        allocatorAddress, endAddressOffset, freeListOffset,
        allowNativeCodeBumpAllocation, this->m_func->IsOOPJIT());

    IR::RegOpnd * allocatorAddressRegOpnd = IR::RegOpnd::New(TyMachPtr, this->m_func);

    // LDIMM allocatorAddressRegOpnd, allocator
    IR::AddrOpnd* allocatorAddressOpnd = IR::AddrOpnd::New(allocatorAddress, IR::AddrOpndKindDynamicMisc, this->m_func);
    IR::Instr * loadAllocatorAddressInstr = IR::Instr::New(Js::OpCode::LDIMM, allocatorAddressRegOpnd, allocatorAddressOpnd, this->m_func);
    insertionPointInstr->InsertBefore(loadAllocatorAddressInstr);

    IR::IndirOpnd * endAddressOpnd = IR::IndirOpnd::New(allocatorAddressRegOpnd, endAddressOffset, TyMachPtr, this->m_func);
    IR::IndirOpnd * freeObjectListOpnd = IR::IndirOpnd::New(allocatorAddressRegOpnd, freeListOffset, TyMachPtr, this->m_func);

    // LDR newObjDst, allocator->freeObjectList
    IR::Instr * loadMemBlockInstr = IR::Instr::New(Js::OpCode::LDR, newObjDst, freeObjectListOpnd, this->m_func);
    insertionPointInstr->InsertBefore(loadMemBlockInstr);
    LegalizeMD::LegalizeInstr(loadMemBlockInstr);

    // nextMemBlock = ADD newObjDst, allocSize
    IR::RegOpnd * nextMemBlockOpnd = IR::RegOpnd::New(TyMachPtr, this->m_func);
    IR::IntConstOpnd* allocSizeOpnd = IR::IntConstOpnd::New((int32)allocSize, TyInt32, this->m_func);
    IR::Instr * loadNextMemBlockInstr = IR::Instr::New(Js::OpCode::ADD, nextMemBlockOpnd, newObjDst, allocSizeOpnd, this->m_func);
    insertionPointInstr->InsertBefore(loadNextMemBlockInstr);
    LegalizeMD::LegalizeInstr(loadNextMemBlockInstr);

    // CMP nextMemBlock, allocator->endAddress
    IR::Instr * checkInstr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
    checkInstr->SetSrc1(nextMemBlockOpnd);
    checkInstr->SetSrc2(endAddressOpnd);
    insertionPointInstr->InsertBefore(checkInstr);
    LegalizeMD::LegalizeInstr(checkInstr);

    // BHI $allocHelper
    IR::BranchInstr * branchToAllocHelperInstr = IR::BranchInstr::New(Js::OpCode::BHI, allocHelperLabel, this->m_func);
    insertionPointInstr->InsertBefore(branchToAllocHelperInstr);

    // LDR allocator->freeObjectList, nextMemBlock
    IR::Instr * setFreeObjectListInstr = IR::Instr::New(Js::OpCode::LDR, freeObjectListOpnd, nextMemBlockOpnd, this->m_func);
    insertionPointInstr->InsertBefore(setFreeObjectListInstr);
    LegalizeMD::LegalizeInstr(setFreeObjectListInstr);

    // B $allocDone
    IR::BranchInstr * branchToAllocDoneInstr = IR::BranchInstr::New(Js::OpCode::B, allocDoneLabel, this->m_func);
    insertionPointInstr->InsertBefore(branchToAllocDoneInstr);
}

void
LowererMD::GenerateClz(IR::Instr * instr)
{
    Assert(instr->GetSrc1()->IsIntegral32());
    Assert(IRType_IsNativeInt(instr->GetDst()->GetType()));
    instr->m_opcode = Js::OpCode::CLZ;
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::GenerateCtz(IR::Instr * instr)
{
    Assert(instr->GetSrc1()->IsInt32() || instr->GetSrc1()->IsUInt32() || instr->GetSrc1()->IsInt64());
    Assert(instr->GetDst()->IsInt32() || instr->GetDst()->IsUInt32() || instr->GetDst()->IsInt64());
    // ARM64 has no ctz; ctz(x) = clz(rbit(x)). RBIT reverses bit order so
    // trailing zeros become leading zeros; zero maps correctly (rbit(0)=0, clz(0)=width).
    IR::Opnd *src = instr->GetSrc1();
    IRType type = src->GetType();
    // Use signed 32/64 for intermediate (bit pattern identical for unsigned)
    IRType regType = (type == TyInt64 || type == TyUint64) ? TyInt64 : TyInt32;
    // For uint32 source with int32 dst (popcnt case uses uint32), keep regType as above
    IR::RegOpnd *reversed = IR::RegOpnd::New(regType, m_func);
    IR::Instr *rbitInstr = IR::Instr::New(Js::OpCode::RBIT, reversed, src->UseWithNewType(regType, m_func), m_func);
    instr->InsertBefore(rbitInstr);
    LegalizeMD::LegalizeInstr(rbitInstr);
    instr->SetSrc1(reversed);
    instr->m_opcode = Js::OpCode::CLZ;
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::GeneratePopCnt(IR::Instr * instr)
{
    Assert(instr->GetSrc1()->IsInt32() || instr->GetSrc1()->IsUInt32() || instr->GetSrc1()->IsInt64());
    Assert(instr->GetDst()->IsInt32() || instr->GetDst()->IsUInt32() || instr->GetDst()->IsInt64());
    // Helper-based popcount (LowerMDShared fallback path). Uses existing
    // HelperPopCnt32/64 which are implemented in the runtime and take a
    // single int arg. This avoids NEON CNT plumbing for slice 3; record
    // choice as helper per plan ("helper or NEON cnt").
    int instrSize = instr->GetSrc1()->GetSize();
    LoadHelperArgument(instr, instr->GetSrc1());
    instr->UnlinkSrc1();
    this->ChangeToHelperCall(instr, instrSize == 8 ? IR::HelperPopCnt64 : IR::HelperPopCnt32);
}

IR::RegOpnd*
LowererMD::MaterializeDoubleConst(intptr_t constAddr, IR::Instr* instr)
{
    IR::Opnd* constVal = IR::MemRefOpnd::New(constAddr, IRType::TyFloat64, this->m_func);
    IR::RegOpnd * reg = IR::RegOpnd::New(TyFloat64, this->m_func);
    this->m_lowerer->InsertMove(reg, constVal, instr);
    return reg;
}

template <bool Saturate>
IR::Opnd*
LowererMD::GenerateTruncChecks(IR::Instr* instr, IR::LabelInstr* doneLabel)
{
    AnalysisAssert(!Saturate || doneLabel);
    IR::Opnd* dst = instr->GetDst();
    Assert(dst->IsInt32() || dst->IsUInt32());
    IR::LabelInstr * nanLabel = (Saturate && dst->IsSigned()) ? IR::LabelInstr::New(Js::OpCode::Label, m_func, true) : nullptr;
    IR::LabelInstr * conversion = IR::LabelInstr::New(Js::OpCode::Label, m_func);
    IR::LabelInstr * tooSmallLabel = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);
    IR::Opnd* src1 = instr->GetSrc1();
    IR::Opnd * src64 = nullptr;
    if (src1->IsFloat32())
    {
        src64 = IR::RegOpnd::New(TyFloat64, m_func);
        EmitFloat32ToFloat64(src64, src1, instr);
    }
    else
    {
        src64 = src1;
    }
    IR::RegOpnd* limitReg = this->MaterializeDoubleConst(dst->IsUInt32() ?
        m_func->GetThreadContextInfo()->GetDoubleNegOneAddr() :
        m_func->GetThreadContextInfo()->GetDoubleIntMinMinusOneAddr(), instr);
    this->m_lowerer->InsertCompareBranch(src64, limitReg, Js::OpCode::BrLe_A, tooSmallLabel, instr);
    limitReg = this->MaterializeDoubleConst(dst->IsUInt32() ?
        m_func->GetThreadContextInfo()->GetDoubleUintMaxPlusOneAddr() :
        m_func->GetThreadContextInfo()->GetDoubleIntMaxPlusOneAddr(), instr);
    this->m_lowerer->InsertCompareBranch(limitReg, src64, Js::OpCode::BrGt_A, conversion, instr, true /*no NaN check*/);
    if (Saturate)
    {
        this->m_lowerer->InsertLabel(true, instr);
        instr->InsertBefore(IR::BranchInstr::New(Js::OpCode::BVS, dst->IsSigned() ? nanLabel : tooSmallLabel, m_func));
        this->m_lowerer->InsertMove(dst, IR::IntConstOpnd::New(dst->IsUnsigned() ? UINT32_MAX : INT32_MAX, dst->GetType(), m_func), instr);
        this->m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, instr);
        instr->InsertBefore(tooSmallLabel);
        this->m_lowerer->InsertMove(dst, IR::IntConstOpnd::New(dst->IsUnsigned() ? 0 : INT32_MIN, dst->GetType(), m_func), instr);
        this->m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, instr);
        if (dst->IsSigned())
        {
            instr->InsertBefore(nanLabel);
            this->m_lowerer->InsertMove(dst, IR::IntConstOpnd::New(0, dst->GetType(), m_func), instr);
            this->m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, instr);
        }
    }
    else
    {
        instr->InsertBefore(tooSmallLabel);
        this->m_lowerer->GenerateThrow(IR::IntConstOpnd::New(SCODE_CODE(VBSERR_Overflow), TyInt32, m_func), instr);
    }
    instr->InsertBefore(conversion);
    return src64;
}

template <bool Saturate>
void
LowererMD::GenerateTruncWithCheck(IR::Instr * instr)
{
    IR::LabelInstr * doneLabel = Saturate ? IR::LabelInstr::New(Js::OpCode::Label, m_func) : nullptr;
    IR::Opnd* src64 = GenerateTruncChecks<Saturate>(instr, doneLabel);
    IR::Opnd* dst = instr->GetDst();
    // ARM64 native unsigned FCVTZ handles unsigned directly; no 2^31 hack needed.
    instr->InsertBefore(IR::Instr::New(Js::OpCode::FCVTZ, dst, src64, m_func));
    LegalizeMD::LegalizeInstr(instr->m_prev);
    if (Saturate)
    {
        instr->InsertBefore(doneLabel);
    }
    instr->UnlinkSrc1();
    instr->UnlinkDst();
    instr->Remove();
}

template void LowererMD::GenerateTruncWithCheck<false>(IR::Instr * instr);
template void LowererMD::GenerateTruncWithCheck<true>(IR::Instr * instr);

void
LowererMD::SaveDoubleToVar(IR::RegOpnd * dstOpnd, IR::RegOpnd *opndFloat, IR::Instr *instrOrig, IR::Instr *instrInsert, bool isHelper)
{
    Assert(opndFloat->GetType() == TyFloat64);

    // Call JSNumber::ToVar to save the float operand to the result of the original (var) instruction

    // s1 = MOV opndFloat
    IR::RegOpnd *s1 = IR::RegOpnd::New(TyMachReg, m_func);
    IR::Instr *mov = IR::Instr::New(Js::OpCode::FMOV_GEN, s1, opndFloat, m_func);
    instrInsert->InsertBefore(mov);

    if (m_func->GetJITFunctionBody()->IsAsmJsMode())
    {
        // s1 = FMOV_GEN src
        // tmp = UBFX s1, #52, #11 ; extract exponent, bits 52-62
        // cmp = tmp, 0x7FF
        // beq tmp, helper
        // b done
        // helper:
        // tmp2 = tmp2 = UBFX s1, #0, #52 ; extract mantissa, bits 0-51
        // cbz tmp2, done
        // s1 = JavascriptNumber::k_Nan
        // done:

        IR::RegOpnd* tmp = IR::RegOpnd::New(TyMachReg, m_func);

        IR::Instr* newInstr = IR::Instr::New(Js::OpCode::UBFX, tmp, s1, IR::IntConstOpnd::New(BITFIELD(52, 11), TyMachReg, m_func, true), m_func);
        instrInsert->InsertBefore(newInstr);
        LowererMD::Legalize(newInstr);

        // CMP is built dst-less with both operands wired (asm64 legalize turns
        // it into SUBS RegZR, src1, src2; a dst-anchored CMP leaves src2 null
        // and faults at Encode). Use the canonical compare-with-imm idiom.
        newInstr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
        newInstr->SetSrc1(tmp);
        newInstr->SetSrc2(IR::IntConstOpnd::New(0x7FF, TyMachReg, m_func, true));
        instrInsert->InsertBefore(newInstr);
        LowererMD::Legalize(newInstr);

        IR::LabelInstr* helper = Lowerer::InsertLabel(true, instrInsert);
        IR::Instr* branch = IR::BranchInstr::New(Js::OpCode::BEQ, helper, m_func);
        helper->InsertBefore(branch);

        IR::LabelInstr* done = Lowerer::InsertLabel(isHelper, instrInsert);

        Lowerer::InsertBranch(Js::OpCode::Br, done, helper);

        IR::RegOpnd* tmp2 = IR::RegOpnd::New(TyMachReg, m_func);

        newInstr = IR::Instr::New(Js::OpCode::UBFX, tmp2, s1, IR::IntConstOpnd::New(BITFIELD(0, 52), TyMachReg, m_func, true), m_func);
        done->InsertBefore(newInstr);
        LowererMD::Legalize(newInstr);

        branch = IR::BranchInstr::New(Js::OpCode::CBZ, done, m_func);
        branch->SetSrc1(tmp2);
        done->InsertBefore(branch);

        IR::Opnd * opndNaN = IR::AddrOpnd::New((Js::Var)Js::JavascriptNumber::k_Nan, IR::AddrOpndKindConstantVar, m_func, true);
        Lowerer::InsertMove(s1, opndNaN, done);
    }

    // s1 = EOR s1, FloatTag_Value
    // dst = s1

    IR::Instr* setTag = IR::Instr::New(Js::OpCode::EOR, s1, s1, IR::AddrOpnd::New((Js::Var)Js::FloatTag_Value, IR::AddrOpndKindConstantVar, this->m_func, true), this->m_func);
    IR::Instr* movDst = IR::Instr::New(Js::OpCode::MOV, dstOpnd, s1, this->m_func);

    instrInsert->InsertBefore(setTag);
    instrInsert->InsertBefore(movDst);
    LowererMD::Legalize(setTag);
}

void
LowererMD::GenerateFastAbs(IR::Opnd *dst, IR::Opnd *src, IR::Instr *callInstr, IR::Instr *insertInstr, IR::LabelInstr *labelHelper, IR::LabelInstr *doneLabel)
{
    //      if isFloat goto $float
    // s1 = MOV src
    //      CMP s1, #0
    // s1 = CSNEGPL s1, s1
    //      TBNZ s1, #31, $labelHelper
    // s1 = ORR s1, AtomTag_IntPtr
    //      JMP $done
    // $float
    //      CMP [src], JavascriptNumber.vtable
    //      JNE $helper
    //      MOVSD r1, [src + offsetof(value)]
    //      ANDPD r1, absDoubleCst
    //      dst = DoubleToVar(r1)

    IR::Instr      *instr      = nullptr;
    IR::LabelInstr *labelFloat = nullptr;
    bool            isInt      = false;
    bool            isNotInt   = false;

    if (src->IsRegOpnd())
    {
        if (src->AsRegOpnd()->IsTaggedInt())
        {
            isInt = true;

        }
        else if (src->AsRegOpnd()->IsNotInt())
        {
            isNotInt = true;
        }
    }
    else if (src->IsAddrOpnd())
    {
        IR::AddrOpnd *varOpnd = src->AsAddrOpnd();
        Assert(varOpnd->IsVar() && Js::TaggedInt::Is(varOpnd->m_address));

        int64 absValue = ::_abs64(Js::TaggedInt::ToInt32(varOpnd->m_address));

        if (!Js::TaggedInt::IsOverflow(absValue))
        {
            varOpnd->SetAddress(Js::TaggedInt::ToVarUnchecked((int32)absValue), IR::AddrOpndKindConstantVar);

            instr = IR::Instr::New(Js::OpCode::MOV, dst, varOpnd, this->m_func);
            insertInstr->InsertBefore(instr);

            return;
        }
    }

    if (src->IsRegOpnd() == false)
    {
        IR::RegOpnd *regOpnd = IR::RegOpnd::New(TyVar, this->m_func);
        instr = IR::Instr::New(Js::OpCode::MOV, regOpnd, src, this->m_func);
        insertInstr->InsertBefore(instr);
        src = regOpnd;
    }

    bool emitFloatAbs = !isInt;

    if (!isNotInt)
    {
        if (!isInt)
        {
            IR::LabelInstr *label = labelHelper;
            if (emitFloatAbs)
            {
                label = labelFloat = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
            }

            GenerateSmIntTest(src, insertInstr, label);
        }

        // s1 = MOV src
        IR::RegOpnd *regSrc = IR::RegOpnd::New(TyInt32, this->m_func);
        Lowerer::InsertMove(regSrc, src, insertInstr);

        //      CMP s1, #0
        instr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
        instr->SetSrc1(regSrc);
        instr->SetSrc2(IR::IntConstOpnd::New(0, IRType::TyInt32, this->m_func));
        insertInstr->InsertBefore(instr);
        Legalize(instr);

        // s1 = CSNEGPL s1, s1
        instr = IR::Instr::New(Js::OpCode::CSNEGPL, regSrc, regSrc, regSrc, this->m_func);
        insertInstr->InsertBefore(instr);
        Legalize(instr);

        //      TBNZ s1, #31, $labelHelper
        instr = IR::BranchInstr::New(Js::OpCode::TBNZ, labelHelper, this->m_func);
        instr->SetSrc1(regSrc);
        instr->SetSrc2(IR::IntConstOpnd::New(31, IRType::TyInt32, this->m_func));
        insertInstr->InsertBefore(instr);

        //      MOV dst, s1
        instr = IR::Instr::New(Js::OpCode::MOV, dst, regSrc, this->m_func);
        insertInstr->InsertBefore(instr);

        GenerateInt32ToVarConversion(dst, insertInstr);
    }

    if (labelFloat)
    {
        // B $done
        instr = IR::BranchInstr::New(Js::OpCode::B, doneLabel, this->m_func);
        insertInstr->InsertBefore(instr);

        // $float
        insertInstr->InsertBefore(labelFloat);
    }

    if (emitFloatAbs)
    {
        // if (typeof(src) == double)
        IR::RegOpnd *src64 = src->AsRegOpnd();
        GenerateFloatTest(src64, insertInstr, labelHelper);

        // dst64 = MOV src64
        insertInstr->InsertBefore(IR::Instr::New(Js::OpCode::MOV, dst, src64, this->m_func));

        // Unconditionally set the sign bit. This will get XORd away when we remove the tag.
        // dst64 = ORR 0x8000000000000000
        insertInstr->InsertBefore(IR::Instr::New(Js::OpCode::ORR, dst, dst, IR::IntConstOpnd::New(MachSignBit, TyMachReg, this->m_func), this->m_func));
    }
    else if(!isInt)
    {
        // The source is not known to be a tagged int, so either it's definitely not an int (isNotInt), or the int version of
        // abs failed the tag check and jumped here. We can't emit the float version of abs (!emitFloatAbs) due to SSE2 not
        // being available, so jump straight to the helper.

        // JMP $helper
        instr = IR::BranchInstr::New(Js::OpCode::B, labelHelper, this->m_func);
        insertInstr->InsertBefore(instr);
    }
}

void
LowererMD::EmitInt4Instr(IR::Instr *instr)
{
    IR::Instr * newInstr;
    IR::Opnd *  src1;
    IR::Opnd *  src2;

    switch (instr->m_opcode)
    {
    case Js::OpCode::Neg_I4:
        instr->m_opcode = Js::OpCode::SUB;
        instr->SetSrc2(instr->UnlinkSrc1());
        instr->SetSrc1(IR::RegOpnd::New(nullptr, RegZR, TyInt32, instr->m_func));
        break;

    case Js::OpCode::Not_I4:
        instr->m_opcode = Js::OpCode::MVN;
        break;

    case Js::OpCode::Add_I4:
        instr->m_opcode = Js::OpCode::ADD;
        break;

    case Js::OpCode::Sub_I4:
        instr->m_opcode = Js::OpCode::SUB;
        break;

    case Js::OpCode::Mul_I4:
        instr->m_opcode = Js::OpCode::MUL;
        break;

    case Js::OpCode::DivU_I4:
        instr->m_opcode = Js::OpCode::UDIV;
        break;

    case Js::OpCode::Div_I4:
        instr->m_opcode = Js::OpCode::SDIV;
        break;

    case Js::OpCode::RemU_I4:
        instr->m_opcode = Js::OpCode::UREM;
        break;

    case Js::OpCode::Rem_I4:
        instr->m_opcode = Js::OpCode::REM;
        break;

    case Js::OpCode::Or_I4:
        instr->m_opcode = Js::OpCode::ORR;
        break;

    case Js::OpCode::Xor_I4:
        instr->m_opcode = Js::OpCode::EOR;
        break;

    case Js::OpCode::And_I4:
        instr->m_opcode = Js::OpCode::AND;
        break;

    case Js::OpCode::Shl_I4:
    case Js::OpCode::ShrU_I4:
    case Js::OpCode::Shr_I4:
        ChangeToShift(instr, false /* needFlags */);
        break;

    case Js::OpCode::BrTrue_I4:
        instr->m_opcode = Js::OpCode::CBNZ;
        break;

    case Js::OpCode::BrFalse_I4:
        instr->m_opcode = Js::OpCode::CBZ;
        break;

    case Js::OpCode::BrEq_I4:
        instr->m_opcode = Js::OpCode::BEQ;
        goto br2_Common;

    case Js::OpCode::BrNeq_I4:
        instr->m_opcode = Js::OpCode::BNE;
        goto br2_Common;

    case Js::OpCode::BrGt_I4:
        instr->m_opcode = Js::OpCode::BGT;
        goto br2_Common;

    case Js::OpCode::BrGe_I4:
        instr->m_opcode = Js::OpCode::BGE;
        goto br2_Common;

    case Js::OpCode::BrLe_I4:
        instr->m_opcode = Js::OpCode::BLE;
        goto br2_Common;

    case Js::OpCode::BrLt_I4:
        instr->m_opcode = Js::OpCode::BLT;
        goto br2_Common;

    case Js::OpCode::BrUnGt_I4:
        instr->m_opcode = Js::OpCode::BHI;
        goto br2_Common;

    case Js::OpCode::BrUnGe_I4:
        instr->m_opcode = Js::OpCode::BCS;
        goto br2_Common;

    case Js::OpCode::BrUnLt_I4:
        instr->m_opcode = Js::OpCode::BCC;
        goto br2_Common;

    case Js::OpCode::BrUnLe_I4:
        instr->m_opcode = Js::OpCode::BLS;
        goto br2_Common;

br2_Common:
        src1 = instr->UnlinkSrc1();
        src2 = instr->UnlinkSrc2();
        newInstr = IR::Instr::New(Js::OpCode::CMP, instr->m_func);
        instr->InsertBefore(newInstr);
        newInstr->SetSrc1(src1);
        newInstr->SetSrc2(src2);
        // Let instr point to the CMP so we can legalize it.
        instr = newInstr;
        break;

    case Js::OpCode::Rol_I4:
    case Js::OpCode::Ror_I4:
    {
        // ARM64 has no ROL; RORV (rotate right, variable) covers both:
        // ror(x,y) = RORV, rol(x,y) = ror(x,-y). RORV reads the amount
        // mod 32/64 in hardware, matching wasm rotate semantics. A const
        // amount is hoisted to a register by the RORV legalizer (LEGAL_REG3).
        IR::Opnd *dst = instr->GetDst();
        IR::Opnd *src2 = instr->UnlinkSrc2();
        IRType type = dst->GetType();
        if (instr->m_opcode == Js::OpCode::Rol_I4)
        {
            // negAmt = 0 - amt (SUB with ZR; const amt is hoisted by the
            // SUB legalizer). (-y) mod 32/64 == (w-y) mod 32/64.
            IR::RegOpnd *negReg = IR::RegOpnd::New(type, instr->m_func);
            IR::Instr *negInstr = IR::Instr::New(Js::OpCode::SUB, negReg, IR::RegOpnd::New(nullptr, RegZR, type, instr->m_func), src2, instr->m_func);
            instr->InsertBefore(negInstr);
            LegalizeMD::LegalizeInstr(negInstr);
            src2 = negReg;
        }
        instr->SetSrc2(src2);
        instr->m_opcode = Js::OpCode::RORV;
        break;
    }

    default:
        AssertMsg(UNREACHED, "NYI I4 instr");
        break;
    }

    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::LowerInt4NegWithBailOut(
    IR::Instr *const instr,
    const IR::BailOutKind bailOutKind,
    IR::LabelInstr *const bailOutLabel,
    IR::LabelInstr *const skipBailOutLabel)
{
    Assert(instr);
    Assert(instr->m_opcode == Js::OpCode::Neg_I4);
    Assert(!instr->HasBailOutInfo());
    Assert(bailOutKind & IR::BailOutOnResultConditions || bailOutKind == IR::BailOutOnFailedHoistedLoopCountBasedBoundCheck);
    Assert(bailOutLabel);
    Assert(instr->m_next == bailOutLabel);
    Assert(skipBailOutLabel);
    Assert(instr->GetDst()->IsInt32());
    Assert(instr->GetSrc1()->IsInt32());

    // SUBS dst, zr, src1
    // BVS   $bailOutLabel
    // BEQ   $bailOutLabel
    // B     $skipBailOut
    // $bailOut:
    // ...
    // $skipBailOut:

    // Lower the instruction
    instr->m_opcode = Js::OpCode::SUBS;
    instr->ReplaceDst(instr->GetDst()->UseWithNewType(TyInt32, instr->m_func));
    instr->SetSrc2(instr->UnlinkSrc1()->UseWithNewType(TyInt32, instr->m_func));
    instr->SetSrc1(IR::RegOpnd::New(nullptr, RegZR, TyInt32, instr->m_func));
    Legalize(instr);

    if(bailOutKind & IR::BailOutOnOverflow)
    {
        bailOutLabel->InsertBefore(IR::BranchInstr::New(Js::OpCode::BVS, bailOutLabel, instr->m_func));
    }

    if(bailOutKind & IR::BailOutOnNegativeZero)
    {
        bailOutLabel->InsertBefore(IR::BranchInstr::New(Js::OpCode::BEQ, bailOutLabel, instr->m_func));
    }

    // Skip bailout
    bailOutLabel->InsertBefore(IR::BranchInstr::New(LowererMD::MDUncondBranchOpcode, skipBailOutLabel, instr->m_func));
}

void
LowererMD::LowerInt4AddWithBailOut(
    IR::Instr *const instr,
    const IR::BailOutKind bailOutKind,
    IR::LabelInstr *const bailOutLabel,
    IR::LabelInstr *const skipBailOutLabel)
{
    Assert(instr);
    Assert(instr->m_opcode == Js::OpCode::Add_I4);
    Assert(!instr->HasBailOutInfo());
    Assert(
        (bailOutKind & IR::BailOutOnResultConditions) == IR::BailOutOnOverflow ||
        bailOutKind == IR::BailOutOnFailedHoistedLoopCountBasedBoundCheck);
    Assert(bailOutLabel);
    Assert(instr->m_next == bailOutLabel);
    Assert(skipBailOutLabel);
    Assert(instr->GetDst()->IsInt32());
    Assert(instr->GetSrc1()->IsInt32());
    Assert(instr->GetSrc2()->IsInt32());

    // ADDS dst, src1, src2
    // BVC skipBailOutLabel
    // fallthrough to bailout

    const auto dst = instr->GetDst(), src1 = instr->GetSrc1(), src2 = instr->GetSrc2();
    Assert(dst->IsRegOpnd());

    const bool dstEquSrc1 = dst->IsEqual(src1), dstEquSrc2 = dst->IsEqual(src2);
    if(dstEquSrc1 || dstEquSrc2)
    {
        LowererMD::ChangeToAssign(instr->SinkDst(Js::OpCode::Ld_I4, RegNOREG, skipBailOutLabel));
    }

    // Lower the instruction
    ChangeToAdd(instr, true /* needFlags */);
    Legalize(instr);

    // Skip bailout on no overflow
    bailOutLabel->InsertBefore(IR::BranchInstr::New(Js::OpCode::BVC, skipBailOutLabel, instr->m_func));

    // Fall through to bailOutLabel
}

void
LowererMD::LowerInt4SubWithBailOut(
    IR::Instr *const instr,
    const IR::BailOutKind bailOutKind,
    IR::LabelInstr *const bailOutLabel,
    IR::LabelInstr *const skipBailOutLabel)
{
    Assert(instr);
    Assert(instr->m_opcode == Js::OpCode::Sub_I4);
    Assert(!instr->HasBailOutInfo());
    Assert(
        (bailOutKind & IR::BailOutOnResultConditions) == IR::BailOutOnOverflow ||
        bailOutKind == IR::BailOutOnFailedHoistedLoopCountBasedBoundCheck);
    Assert(bailOutLabel);
    Assert(instr->m_next == bailOutLabel);
    Assert(skipBailOutLabel);
    Assert(instr->GetDst()->IsInt32());
    Assert(instr->GetSrc1()->IsInt32());
    Assert(instr->GetSrc2()->IsInt32());

    // SUBS dst, src1, src2
    // BVC skipBailOutLabel
    // fallthrough to bailout

    const auto dst = instr->GetDst(), src1 = instr->GetSrc1(), src2 = instr->GetSrc2();
    Assert(dst->IsRegOpnd());

    const bool dstEquSrc1 = dst->IsEqual(src1), dstEquSrc2 = dst->IsEqual(src2);
    if(dstEquSrc1 || dstEquSrc2)
    {
        LowererMD::ChangeToAssign(instr->SinkDst(Js::OpCode::Ld_I4, RegNOREG, skipBailOutLabel));
    }

    // Lower the instruction
    ChangeToSub(instr, true /* needFlags */);
    Legalize(instr);

    // Skip bailout on no overflow
    bailOutLabel->InsertBefore(IR::BranchInstr::New(Js::OpCode::BVC, skipBailOutLabel, instr->m_func));

    // Fall through to bailOutLabel
}

void
LowererMD::LowerInt4MulWithBailOut(
    IR::Instr *const instr,
    const IR::BailOutKind bailOutKind,
    IR::LabelInstr *const bailOutLabel,
    IR::LabelInstr *const skipBailOutLabel)
{
    Assert(instr);
    Assert(instr->m_opcode == Js::OpCode::Mul_I4);
    Assert(!instr->HasBailOutInfo());
    Assert(bailOutKind & IR::BailOutOnResultConditions || bailOutKind == IR::BailOutOnFailedHoistedLoopCountBasedBoundCheck);
    Assert(bailOutLabel);
    Assert(instr->m_next == bailOutLabel);
    Assert(skipBailOutLabel);

    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->UnlinkSrc1();
    IR::Opnd *src2 = instr->UnlinkSrc2();
    IR::Instr *insertInstr;

    Assert(dst->IsInt32());
    Assert(src1->IsInt32());
    Assert(src2->IsInt32());

    // s3 = SMULL src1, src2 // result is i64
    IR::Opnd* s3 = IR::RegOpnd::New(TyInt64, instr->m_func);
    insertInstr = IR::Instr::New(Js::OpCode::SMULL, s3, src1, src2, instr->m_func);
    instr->InsertBefore(insertInstr);
    LegalizeMD::LegalizeInstr(insertInstr);

    // dst = MOV_TRUNC s3
    instr->m_opcode = Js::OpCode::MOV_TRUNC;
    instr->SetSrc1(s3->UseWithNewType(TyInt32, instr->m_func));

    // check negative zero
    //
    // If the result is zero, we need to check and only bail out if it would be -0.
    // We know that if the result is 0/-0, at least operand should be zero.
    // We should bailout if src1 + src2 < 0, as this proves that the other operand is negative
    //
    //    CMN src1, src2
    //    BPL $skipBailOutLabel
    //
    // $bailOutLabel
    //    GenerateBailout
    //
    // $skipBailOutLabel
    IR::LabelInstr *checkForNegativeZeroLabel = nullptr;
    if(bailOutKind & IR::BailOutOnNegativeZero)
    {
        checkForNegativeZeroLabel = IR::LabelInstr::New(Js::OpCode::Label, instr->m_func, true);
        bailOutLabel->InsertBefore(checkForNegativeZeroLabel);

        Assert(dst->IsRegOpnd());
        Assert(!src1->IsEqual(src2)); // cannot result in -0 if both operands are the same; GlobOpt should have figured that out

        // CMN src1, src2
        // BPL $skipBailOutLabel
        insertInstr = IR::Instr::New(Js::OpCode::CMN, instr->m_func);
        insertInstr->SetSrc1(src1);
        insertInstr->SetSrc2(src2);
        bailOutLabel->InsertBefore(insertInstr);
        LegalizeMD::LegalizeInstr(insertInstr);
        bailOutLabel->InsertBefore(IR::BranchInstr::New(Js::OpCode::BPL, skipBailOutLabel, instr->m_func));

        // Fall through to bailOutLabel
    }

    IR::LabelInstr* insertBeforeInstr = checkForNegativeZeroLabel ? checkForNegativeZeroLabel : bailOutLabel;

    //check overflow
    if(bailOutKind & IR::BailOutOnMulOverflow || bailOutKind == IR::BailOutOnFailedHoistedLoopCountBasedBoundCheck)
    {
        insertInstr = IR::Instr::New(Js::OpCode::CMP_SXTW, instr->m_func);
        insertInstr->SetSrc1(s3);
        insertInstr->SetSrc2(s3);
        instr->InsertBefore(insertInstr);

        // BNE $bailOutHelper
        insertInstr = IR::BranchInstr::New(Js::OpCode::BNE, bailOutLabel, instr->m_func);
        instr->InsertBefore(insertInstr);
    }

    if(bailOutKind & IR::BailOutOnNegativeZero)
    {
        // TST dst, dst
        // BEQ $checkForNegativeZeroLabel
        insertInstr = IR::Instr::New(Js::OpCode::TST, instr->m_func);
        insertInstr->SetSrc1(dst);
        insertInstr->SetSrc2(dst);
        insertBeforeInstr->InsertBefore(insertInstr);
        LegalizeMD::LegalizeInstr(insertInstr);

        insertBeforeInstr->InsertBefore(IR::BranchInstr::New(Js::OpCode::BEQ, checkForNegativeZeroLabel, instr->m_func));
    }

    insertBeforeInstr->InsertBefore(IR::BranchInstr::New(Js::OpCode::B, skipBailOutLabel, instr->m_func));
}

void
LowererMD::LowerInt4RemWithBailOut(
    IR::Instr *const instr,
    const IR::BailOutKind bailOutKind,
    IR::LabelInstr *const bailOutLabel,
    IR::LabelInstr *const skipBailOutLabel) const
{
    Assert(instr);
    Assert(instr->m_opcode == Js::OpCode::Rem_I4);
    Assert(!instr->HasBailOutInfo());
    Assert(bailOutKind & IR::BailOutOnResultConditions);
    Assert(bailOutLabel);
    Assert(instr->m_next == bailOutLabel);
    Assert(skipBailOutLabel);

    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();

    Assert(dst->IsInt32());
    Assert(src1->IsInt32());
    Assert(instr->GetSrc2()->IsInt32());

    //Lower the instruction
    EmitInt4Instr(instr);

    //check for negative zero
    //We have, dst = src1 % src2
    //We need to bailout if dst == 0 and src1 < 0
    //     tst dst, dst
    //     bne $skipBailOutLabel
    //     tst src1,src1
    //     bpl $skipBailOutLabel
    //
    //$bailOutLabel
    //     GenerateBailout();
    //
    //$skipBailOutLabel
    if(bailOutKind & IR::BailOutOnNegativeZero)
    {
        IR::LabelInstr *checkForNegativeZeroLabel = IR::LabelInstr::New(Js::OpCode::Label, instr->m_func, true);
        bailOutLabel->InsertBefore(checkForNegativeZeroLabel);

        IR::Instr *insertInstr = IR::Instr::New(Js::OpCode::TST, instr->m_func);
        insertInstr->SetSrc1(dst);
        insertInstr->SetSrc2(dst);
        bailOutLabel->InsertBefore(insertInstr);
        LegalizeMD::LegalizeInstr(insertInstr);

        IR::Instr *branchInstr = IR::BranchInstr::New(Js::OpCode::BNE, skipBailOutLabel, instr->m_func);
        bailOutLabel->InsertBefore(branchInstr);

        insertInstr = IR::Instr::New(Js::OpCode::TST, instr->m_func);
        insertInstr->SetSrc1(src1);
        insertInstr->SetSrc2(src1);
        bailOutLabel->InsertBefore(insertInstr);
        LegalizeMD::LegalizeInstr(insertInstr);

        branchInstr = IR::BranchInstr::New(Js::OpCode::BPL, skipBailOutLabel, instr->m_func);
        bailOutLabel->InsertBefore(branchInstr);
    }
    // Fall through to bailOutLabel
}

void
LowererMD::EmitLoadVar(IR::Instr *instrLoad, bool isFromUint32, bool isHelper)
{
    //    MOV.32 e1, e_src1
    //    TBNZ e1, #31, $Helper [uint32]  -- overflows?
    //    ORR r1, 1<<VarTag_Shift
    //    MOV r_dst, r1
    //    JMP $done             [uint32]
    // $helper                  [uint32]
    //    EmitLoadVarNoCheck
    // $done                    [uint32]


    Assert(instrLoad->GetSrc1()->IsRegOpnd());
    Assert(instrLoad->GetDst()->GetType() == TyVar);

    bool isInt            = false;
    IR::Opnd *dst         = instrLoad->GetDst();
    IR::RegOpnd *src1     = instrLoad->GetSrc1()->AsRegOpnd();
    IR::LabelInstr *labelHelper = nullptr;

    // TODO: Fix bad lowering. We shouldn't get TyVars here.
    // Assert(instrLoad->GetSrc1()->GetType() == TyInt32);
    src1->SetType(isFromUint32 ? TyUint32 : TyInt32);

    if (src1->IsTaggedInt())
    {
        isInt = true;
    }
    else if (src1->IsNotInt())
    {
        // ToVar()
        this->EmitLoadVarNoCheck(dst->AsRegOpnd(), src1, instrLoad, isFromUint32, isHelper);
        return;
    }

    IR::RegOpnd *r1 = IR::RegOpnd::New(TyVar, m_func);

    // e1 = MOV_TRUNC src1
    // (Use 32-bit MOV_TRUNC here as we rely on the register copy to clear the upper 32 bits.)
    IR::RegOpnd *e1 = r1->Copy(m_func)->AsRegOpnd();
    e1->SetType(TyInt32);
    instrLoad->InsertBefore(IR::Instr::New(Js::OpCode::MOV_TRUNC,
        e1,
        src1,
        m_func));

    if (!isInt && isFromUint32)
    {
        Assert(!labelHelper);
        labelHelper = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);

        // TBNZ e1, #31, $helper
        IR::Instr* instr = IR::BranchInstr::New(Js::OpCode::TBNZ, labelHelper, m_func);
        instr->SetSrc1(e1);
        instr->SetSrc2(IR::IntConstOpnd::New(31, TyInt32, m_func));
        instrLoad->InsertBefore(instr);
    }

    // The previous operation clears the top 32 bits.
    // ORR r1, 1<<VarTag_Shift
    this->GenerateInt32ToVarConversion(r1, instrLoad);

    // REVIEW: We need r1 only if we could generate sn = Ld_A_I4 sn. i.e. the destination and
    // source are the same.
    // r_dst = MOV r1
    instrLoad->InsertBefore(IR::Instr::New(Js::OpCode::MOV,
        dst,
        r1,
        m_func));

    if (labelHelper)
    {
        Assert(isFromUint32);

        // B $done
        IR::LabelInstr * labelDone = IR::LabelInstr::New(Js::OpCode::Label, m_func, isHelper);
        instrLoad->InsertBefore(IR::BranchInstr::New(Js::OpCode::B, labelDone, m_func));

        // $helper
        instrLoad->InsertBefore(labelHelper);

        // ToVar()
        this->EmitLoadVarNoCheck(dst->AsRegOpnd(), src1, instrLoad, isFromUint32, true);

        // $done
        instrLoad->InsertBefore(labelDone);
    }
    instrLoad->Remove();
}

void
LowererMD::EmitLoadVarNoCheck(IR::RegOpnd * dst, IR::RegOpnd * src, IR::Instr *instrLoad, bool isFromUint32, bool isHelper)
{
    IR::RegOpnd * floatReg = IR::RegOpnd::New(TyFloat64, this->m_func);
    if (isFromUint32)
    {
        this->EmitUIntToFloat(floatReg, src, instrLoad);
    }
    else
    {
        this->EmitIntToFloat(floatReg, src, instrLoad);
    }
    this->SaveDoubleToVar(dst, floatReg, instrLoad, instrLoad, isHelper);
}

bool
LowererMD::EmitLoadInt32(IR::Instr *instrLoad, bool conversionFromObjectAllowed, bool bailOutOnHelper, IR::LabelInstr * labelBailOut)
{
    //
    //    r1 = MOV src1
    // rtest = UBFX src1, AtomTag_Shift, 64 - AtomTag_Shift
    //         EOR rtest, 1
    //         CBNZ $helper or $float
    // r_dst = MOV.32 e_src1
    //         B $done
    //  $float:
    //     dst = ConvertToFloat(r1, $helper)
    // $helper:
    // r_dst = ToInt32()
    //

    Assert(instrLoad->GetSrc1()->IsRegOpnd());
    Assert(instrLoad->GetSrc1()->GetType() == TyVar);

    // TODO: Fix bad lowering. We shouldn't see TyVars here.
    // Assert(instrLoad->GetDst()->GetType() == TyInt32);

    bool isInt             = false;
    bool isNotInt          = false;
    IR::Opnd *dst          = instrLoad->GetDst();
    IR::RegOpnd *src1      = instrLoad->GetSrc1()->AsRegOpnd();
    IR::LabelInstr *helper = nullptr;
    IR::LabelInstr *labelFloat = nullptr;
    IR::LabelInstr *done   = nullptr;

    if (src1->IsTaggedInt())
    {
        isInt = true;
    }
    else if (src1->IsNotInt())
    {
        isNotInt = true;
    }

    if (src1->IsEqual(instrLoad->GetDst()) == false)
    {
        // r1 = MOV src1
        IR::RegOpnd *r1 = IR::RegOpnd::New(TyVar, instrLoad->m_func);
        r1->SetValueType(src1->GetValueType());
        instrLoad->InsertBefore(IR::Instr::New(Js::OpCode::MOV, r1, src1, instrLoad->m_func));
        src1 = r1;
    }

    const ValueType src1ValueType(src1->GetValueType());
    const bool doFloatToIntFastPath =
        (src1ValueType.IsLikelyFloat() || src1ValueType.IsLikelyUntaggedInt()) &&
        !(instrLoad->HasBailOutInfo() && (instrLoad->GetBailOutKind() == IR::BailOutIntOnly || instrLoad->GetBailOutKind() == IR::BailOutExpectingInteger));

    if (isNotInt)
    {
        // Known to be non-integer. If we are required to bail out on helper call, just re-jit.
        if (!doFloatToIntFastPath && bailOutOnHelper)
        {
            if(!GlobOpt::DoEliminateArrayAccessHelperCall(this->m_func))
            {
                // Array access helper call removal is already off for some reason. Prevent trying to rejit again
                // because it won't help and the same thing will happen again. Just abort jitting this function.
                if(PHASE_TRACE(Js::BailOutPhase, this->m_func))
                {
                    Output::Print(_u("    Aborting JIT because EliminateArrayAccessHelperCall is already off\n"));
                    Output::Flush();
                }
                throw Js::OperationAbortedException();
            }

            throw Js::RejitException(RejitReason::ArrayAccessHelperCallEliminationDisabled);
        }
    }
    else
    {
        // It could be an integer in this case.
        if (!isInt)
        {
            if(doFloatToIntFastPath)
            {
                labelFloat = IR::LabelInstr::New(Js::OpCode::Label, instrLoad->m_func, false);
            }
            else
            {
                helper = IR::LabelInstr::New(Js::OpCode::Label, instrLoad->m_func, true);
            }

            this->GenerateSmIntTest(src1, instrLoad, labelFloat ? labelFloat : helper);
        }

        instrLoad->InsertBefore(IR::Instr::New(Js::OpCode::MOV_TRUNC,
            dst->UseWithNewType(TyInt32, instrLoad->m_func),
            src1->UseWithNewType(TyInt32, instrLoad->m_func),
            instrLoad->m_func));

        if (!isInt)
        {
            // JMP $done
            done = instrLoad->GetOrCreateContinueLabel();
            instrLoad->InsertBefore(IR::BranchInstr::New(Js::OpCode::B, done, m_func));
        }
    }

    if (!isInt)
    {
        if(doFloatToIntFastPath)
        {
            if(labelFloat)
            {
                instrLoad->InsertBefore(labelFloat);
            }
            if(!helper)
            {
                helper = IR::LabelInstr::New(Js::OpCode::Label, instrLoad->m_func, true);
            }

            if(!done)
            {
                done = instrLoad->GetOrCreateContinueLabel();
            }
            IR::RegOpnd* floatOpnd = this->CheckFloatAndUntag(src1, instrLoad, helper);
            this->ConvertFloatToInt32(instrLoad->GetDst(), floatOpnd, helper, done, instrLoad);
        }

        // $helper:
        if (helper)
        {
            instrLoad->InsertBefore(helper);
        }
        if(instrLoad->HasBailOutInfo() && (instrLoad->GetBailOutKind() == IR::BailOutIntOnly || instrLoad->GetBailOutKind() == IR::BailOutExpectingInteger))
        {
            // Avoid bailout if we have a JavascriptNumber whose value is a signed 32-bit integer
            m_lowerer->LoadInt32FromUntaggedVar(instrLoad);

            // Need to bail out instead of calling a helper
            return true;
        }

        if (bailOutOnHelper)
        {
            Assert(labelBailOut);
            m_lowerer->InsertBranch(Js::OpCode::Br, labelBailOut, instrLoad);
            instrLoad->Remove();
        }
        else if (conversionFromObjectAllowed)
        {
            m_lowerer->LowerUnaryHelperMem(instrLoad, IR::HelperConv_ToInt32);
        }
        else
        {
            m_lowerer->LowerUnaryHelperMemWithBoolReference(instrLoad, IR::HelperConv_ToInt32_NoObjects, true /*useBoolForBailout*/);
        }
    }
    else
    {
        instrLoad->Remove();
    }

    return false;
}

void
LowererMD::ImmedSrcToReg(IR::Instr * instr, IR::Opnd * newOpnd, int srcNum)
{
    if (srcNum == 2)
    {
        instr->SetSrc2(newOpnd);
    }
    else
    {
        Assert(srcNum == 1);
        instr->SetSrc1(newOpnd);
    }

    switch (instr->m_opcode)
    {
    case Js::OpCode::LDIMM:
        instr->m_opcode = Js::OpCode::MOV;
        break;
    default:
        // Nothing to do (unless we have immed/reg variations for other instructions).
        break;
    }
}

IR::LabelInstr *
LowererMD::GetBailOutStackRestoreLabel(BailOutInfo * bailOutInfo, IR::LabelInstr * exitTargetInstr)
{
    return exitTargetInstr;
}

StackSym *
LowererMD::GetImplicitParamSlotSym(Js::ArgSlot argSlot)
{
    return GetImplicitParamSlotSym(argSlot, this->m_func);
}

StackSym *
LowererMD::GetImplicitParamSlotSym(Js::ArgSlot argSlot, Func * func)
{
    // For ARM, offset for implicit params always start at 0
    // TODO: Consider not to use the argSlot number for the param slot sym, which can
    // be confused with arg slot number from javascript
    StackSym * stackSym = StackSym::NewParamSlotSym(argSlot, func);
    func->SetArgOffset(stackSym, argSlot * MachPtr);
    func->SetHasImplicitParamLoad();
    return stackSym;
}

IR::LabelInstr *
LowererMD::EnsureEHEpilogLabel()
{
    if (this->m_func->m_epilogLabel)
    {
        return this->m_func->m_epilogLabel;
    }

    IR::Instr *exitInstr = this->m_func->m_exitInstr;
    IR::Instr *prevInstr = exitInstr->GetPrevRealInstrOrLabel();
    if (prevInstr->IsLabelInstr())
    {
        this->m_func->m_epilogLabel = prevInstr->AsLabelInstr();
        return prevInstr->AsLabelInstr();
    }
    IR::LabelInstr *labelInstr = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    exitInstr->InsertBefore(labelInstr);
    this->m_func->m_epilogLabel = labelInstr;
    return labelInstr;
}

// Helper method: inserts legalized assign for given srcOpnd into RegD0 in front of given instr in the following way:
//   dstReg = InsertMove srcOpnd
// Used to put args of inline built-in call into RegD0 and RegD1 before we call actual CRT function.
void LowererMD::GenerateAssignForBuiltinArg(RegNum dstReg, IR::Opnd* srcOpnd, IR::Instr* instr)
{
    IR::RegOpnd* tempDst = IR::RegOpnd::New(nullptr, dstReg, TyMachDouble, this->m_func);
    tempDst->m_isCallArg = true; // This is to make sure that lifetime of opnd is virtually extended until next CALL instr.
    Lowerer::InsertMove(tempDst, srcOpnd, instr);
}

// For given InlineMathXXX instr, generate the call to actual CRT function/CPU instr.
void LowererMD::GenerateFastInlineBuiltInCall(IR::Instr* instr, IR::JnHelperMethod helperMethod)
{
    switch (instr->m_opcode)
    {
    case Js::OpCode::InlineMathSqrt:
        // Sqrt maps directly to the VFP instruction.
        // src and dst are already float, all we need is just change the opcode and legalize.
        // Before:
        //      dst = InlineMathSqrt src1
        // After:
        //       <potential FSTR by legalizer if src1 is not a register>
        //       dst = FSQRT src1
        Assert(helperMethod == (IR::JnHelperMethod)0);
        Assert(instr->GetSrc2() == nullptr);
        instr->m_opcode = Js::OpCode::FSQRT;
        LegalizeMD::LegalizeInstr(instr);
        break;

    case Js::OpCode::InlineMathAbs:
        Assert(helperMethod == (IR::JnHelperMethod)0);
        return GenerateFastInlineBuiltInMathAbs(instr);

    case Js::OpCode::InlineMathFloor:
    case Js::OpCode::InlineMathCeil:
        // W4 slices 1+2 (doc/wasm-arm64-plan.md): the wasm/asm.js floor and
        // ceil keep their float result (BuildDouble2/BuildFloat2 emit
        // TyFloat64/32 dst), but GenerateFastInlineBuiltInMathFloorCeil is
        // the JS integer fast path (Asserts Int32 dst). Route float dsts
        // through the CRT helpers, mirroring the x64 SSE4.1-less fallback
        // in Lower.cpp. Integer dsts stay on the fast path.
        if (!instr->GetDst()->IsInt32())
        {
            if (instr->m_opcode == Js::OpCode::InlineMathFloor)
            {
                HelperCallForAsmMathBuiltin(instr, IR::HelperDirectMath_FloorFlt, IR::HelperDirectMath_FloorDb);
            }
            else
            {
                HelperCallForAsmMathBuiltin(instr, IR::HelperDirectMath_CeilFlt, IR::HelperDirectMath_CeilDb);
            }
            break;
        }
        Assert(helperMethod == (IR::JnHelperMethod)0);
        return GenerateFastInlineBuiltInMathFloorCeil(instr);

    case Js::OpCode::InlineMathRound:
        Assert(helperMethod == (IR::JnHelperMethod)0);
        return GenerateFastInlineBuiltInMathRound(instr);

    case Js::OpCode::InlineMathMin:
    case Js::OpCode::InlineMathMax:
        Assert(helperMethod == (IR::JnHelperMethod)0);
        return GenerateFastInlineBuiltInMathMinMax(instr);

    default:
        // Before:
        //      dst = <Built-in call> src1, src2
        // After:
        //       d0 = InsertMove src1
        //       lr = MOV helperAddr
        //            BLX lr
        //      dst = InsertMove call->dst (d0)

        // Src1
        AssertMsg(instr->GetDst()->IsFloat(), "Currently accepting only float args for math helpers -- dst.");
        AssertMsg(instr->GetSrc1()->IsFloat(), "Currently accepting only float args for math helpers -- src1.");
        AssertMsg(!instr->GetSrc2() || instr->GetSrc2()->IsFloat(), "Currently accepting only float args for math helpers -- src2.");
        this->GenerateAssignForBuiltinArg((RegNum)FIRST_FLOAT_REG, instr->UnlinkSrc1(), instr);

        // Src2
        if (instr->GetSrc2() != nullptr)
        {
            this->GenerateAssignForBuiltinArg((RegNum)(FIRST_FLOAT_REG + 1), instr->UnlinkSrc2(), instr);
        }

        // Call CRT.
        IR::RegOpnd* floatCallDst = IR::RegOpnd::New(nullptr, (RegNum)(FIRST_FLOAT_REG), TyMachDouble, this->m_func);   // Dst in d0.
        IR::Instr* floatCall = IR::Instr::New(Js::OpCode::BLR, floatCallDst, this->m_func);
        instr->InsertBefore(floatCall);

        // lr = MOV helperAddr
        //      BLX lr
        IR::AddrOpnd* targetAddr = IR::AddrOpnd::New((Js::Var)IR::GetMethodOriginalAddress(m_func->GetThreadContextInfo(), helperMethod), IR::AddrOpndKind::AddrOpndKindDynamicMisc, this->m_func);
        IR::RegOpnd *targetOpnd = IR::RegOpnd::New(nullptr, RegLR, TyMachPtr, this->m_func);
        IR::Instr   *movInstr   = IR::Instr::New(Js::OpCode::LDIMM, targetOpnd, targetAddr, this->m_func);
        targetOpnd->m_isCallArg = true;
        floatCall->SetSrc1(targetOpnd);
        floatCall->InsertBefore(movInstr);

        // Save the result.
        Lowerer::InsertMove(instr->UnlinkDst(), floatCall->GetDst(), instr);
        instr->Remove();
        break;
    }
}

void
LowererMD::GenerateFastInlineBuiltInMathAbs(IR::Instr *inlineInstr)
{
    IR::Opnd* src = inlineInstr->GetSrc1()->Copy(this->m_func);
    IR::Opnd* dst = inlineInstr->UnlinkDst();
    Assert(src);
    IR::Instr* tmpInstr;
    IRType srcType = src->GetType();

    IR::Instr* nextInstr = IR::LabelInstr::New(Js::OpCode::Label, m_func);
    IR::Instr* continueInstr = m_lowerer->LowerBailOnIntMin(inlineInstr);
    continueInstr->InsertAfter(nextInstr);
    if (srcType == IRType::TyInt32)
    {
        // Note: if execution gets so far, we always get (untagged) int32 here.
        Assert(src->IsRegOpnd());

        // CMP src, #0
        tmpInstr = IR::Instr::New(Js::OpCode::CMP, this->m_func);
        tmpInstr->SetSrc1(src);
        tmpInstr->SetSrc2(IR::IntConstOpnd::New(0, IRType::TyInt32, this->m_func));
        nextInstr->InsertBefore(tmpInstr);
        Legalize(tmpInstr);

        // dst = CSNEGPL dst, src, src
        tmpInstr = IR::Instr::New(Js::OpCode::CSNEGPL, dst, src, src, this->m_func);
        nextInstr->InsertBefore(tmpInstr);
        Legalize(tmpInstr);
    }
    else if (srcType == IRType::TyFloat64)
    {
        // FABS dst, src
        tmpInstr = IR::Instr::New(Js::OpCode::FABS, dst, src, this->m_func);
        nextInstr->InsertBefore(tmpInstr);
    }
    else
    {
        AssertMsg(FALSE, "GenerateFastInlineBuiltInMathAbs: unexpected type of the src!");
    }
}


void
LowererMD::GenerateFastInlineMathFround(IR::Instr* instr)
{
    // Note that this is fround, not round; this operation is to
    // round a double to Float32 precision.
    IR::Opnd* src1 = instr->GetSrc1();
    IR::Opnd* dst = instr->GetDst();

    Assert(dst->IsFloat());
    Assert(src1->IsFloat());

    IRType srcType = src1->GetType();
    IRType dstType = dst->GetType();

    if (srcType == TyFloat32)
    {
        if (dstType == TyFloat32)
        {
            LowererMD::ChangeToAssign(instr);
        }
        else
        {
            Assert(dstType == TyFloat64);
            instr->m_opcode = Js::OpCode::FCVT;
            LegalizeMD::LegalizeInstr(instr);
        }
    }
    else
    {
        Assert(srcType == TyFloat64);
        if (dstType == TyFloat32)
        {
            instr->m_opcode = Js::OpCode::FCVT;
            LegalizeMD::LegalizeInstr(instr);
        }
        else
        {
            Assert(dstType == TyFloat64);
            IR::RegOpnd* tempOpnd = IR::RegOpnd::New(TyFloat32, instr->m_func);
            IR::Instr* shortener = IR::Instr::New(Js::OpCode::FCVT, tempOpnd, instr->UnlinkSrc1(), instr->m_func);
            instr->InsertBefore(shortener);
            instr->SetSrc1(tempOpnd);
            instr->m_opcode = Js::OpCode::FCVT;
            LegalizeMD::LegalizeInstr(instr);
        }
    }
}

void
LowererMD::GenerateFastInlineBuiltInMathRound(IR::Instr* instr)
{
    Assert(instr->GetDst()->IsInt32());

    IR::LabelInstr * doneLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    // Allocate an integer register for negative zero checks if needed
    IR::Opnd * negZeroReg = nullptr;
    if (instr->ShouldCheckForNegativeZero())
    {
        negZeroReg = IR::RegOpnd::New(TyInt64, this->m_func);
    }

    // FMOV floatOpnd, src
    IR::Opnd * src = instr->UnlinkSrc1();
    IR::RegOpnd* floatOpnd = IR::RegOpnd::New(TyFloat64, this->m_func);
    this->m_lowerer->InsertMove(floatOpnd, src, instr);

    IR::LabelInstr * bailoutLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);;
    bool sharedBailout = (instr->GetBailOutInfo()->bailOutInstr != instr) ? true : false;

    // FMOV_GEN negZeroReg, floatOpnd (note this is done before the 0.5 add below)
    if (negZeroReg)
    {
        instr->InsertBefore(IR::Instr::New(Js::OpCode::FMOV_GEN, negZeroReg, floatOpnd, instr->m_func));
    }

    // Add 0.5
    IR::Opnd * pointFive = IR::MemRefOpnd::New(m_func->GetThreadContextInfo()->GetDoublePointFiveAddr(), IRType::TyFloat64, this->m_func, IR::AddrOpndKindDynamicDoubleRef);
    this->m_lowerer->InsertAdd(false, floatOpnd, floatOpnd, pointFive, instr);

    // MSR FPSR, xzr
    IR::Instr* setFPSRInstr = IR::Instr::New(Js::OpCode::MSR_FPSR, instr->m_func);
    setFPSRInstr->SetSrc1(IR::RegOpnd::New(nullptr, RegZR, TyUint32, instr->m_func));
    instr->InsertBefore(setFPSRInstr);

    // FCVTM intOpnd, floatOpnd
    IR::Opnd * intOpnd = IR::RegOpnd::New(TyInt32, this->m_func);
    instr->InsertBefore(IR::Instr::New(Js::OpCode::FCVTM, intOpnd, floatOpnd, instr->m_func));

    // FCVTM would set FPSR.IOC (0th bit in FPSR) if the source cannot be represented within the destination register

    // MRS exceptReg, FPSR
    IR::Opnd * exceptReg = IR::RegOpnd::New(TyUint32, this->m_func);
    instr->InsertBefore(IR::Instr::New(Js::OpCode::MRS_FPSR, exceptReg, instr->m_func));

    IR::LabelInstr* checkOverflowLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    // CBNZ intOpnd, done/checkOverflow
    IR::BranchInstr * cbnzInstr = cbnzInstr = IR::BranchInstr::New(Js::OpCode::CBNZ, checkOverflowLabel, instr->m_func);
    cbnzInstr->SetSrc1(intOpnd);
    instr->InsertBefore(cbnzInstr);

    if (negZeroReg)
    {
        // TBZ negZeroReg, 63. A non-negative intOpnd==0 still needs the
        // FPSR.IOC check below: FCVTM(+NaN) yields 0 with IOC set, and
        // jumping straight to done would wrongly return 0 for Math.round(NaN).
        // Route through checkOverflowLabel so NaN falls into the bailout.
        IR::BranchInstr * tbzInstr = IR::BranchInstr::New(Js::OpCode::TBZ, checkOverflowLabel, instr->m_func);
        tbzInstr->SetSrc1(negZeroReg);
        tbzInstr->SetSrc2(IR::IntConstOpnd::New(63, TyMachReg, instr->m_func));
        instr->InsertBefore(tbzInstr);

        Lowerer::InsertBranch(LowererMD::MDUncondBranchOpcode, bailoutLabel, instr);
    }

    
    instr->InsertBefore(checkOverflowLabel);

    // TBZ exceptReg, #0, done
    IR::BranchInstr * tbzInstr = IR::BranchInstr::New(Js::OpCode::TBZ, doneLabel, instr->m_func);
    tbzInstr->SetSrc1(exceptReg);
    tbzInstr->SetSrc2(IR::IntConstOpnd::New(0, TyMachReg, instr->m_func));
    instr->InsertBefore(tbzInstr);

    IR::Opnd * dst = instr->UnlinkDst();
    instr->InsertAfter(doneLabel);
    if (!sharedBailout)
    {
        instr->InsertBefore(bailoutLabel);
    }

    // In case of a shared bailout, we should jump to the code that sets some data on the bailout record which is specific
    // to this bailout. Pass the bailoutLabel to GenerateFunction so that it may use the label as the collectRuntimeStatsLabel.
    this->m_lowerer->GenerateBailOut(instr, nullptr, nullptr, sharedBailout ? bailoutLabel : nullptr);

    // MOV dst, intOpnd
    IR::Instr* movInstr = IR::Instr::New(Js::OpCode::MOV, dst, intOpnd, this->m_func);
    doneLabel->InsertAfter(movInstr);
}

void
LowererMD::GenerateFastInlineBuiltInMathFloorCeil(IR::Instr* instr)
{
    Assert(instr->GetDst()->IsInt32());

    IR::LabelInstr * doneLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);

    // Allocate an integer register for negative zero checks if needed
    IR::Opnd * negZeroReg = nullptr;
    if (instr->ShouldCheckForNegativeZero())
    {
        negZeroReg = IR::RegOpnd::New(TyInt64, this->m_func);
    }

    // FMOV floatOpnd, src
    IR::Opnd * src = instr->UnlinkSrc1();
    IR::RegOpnd* floatOpnd = IR::RegOpnd::New(TyFloat64, this->m_func);
    this->m_lowerer->InsertMove(floatOpnd, src, instr);

    IR::LabelInstr * bailoutLabel = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);;
    bool sharedBailout = (instr->GetBailOutInfo()->bailOutInstr != instr) ? true : false;

    // MSR FPSR, xzr
    IR::Instr* setFPSRInstr = IR::Instr::New(Js::OpCode::MSR_FPSR, instr->m_func);
    setFPSRInstr->SetSrc1(IR::RegOpnd::New(nullptr, RegZR, TyUint32, instr->m_func));
    instr->InsertBefore(setFPSRInstr);

    // FMOV_GEN negZeroReg, floatOpnd (note this is done before the 0.5 add below)
    if (negZeroReg)
    {
        instr->InsertBefore(IR::Instr::New(Js::OpCode::FMOV_GEN, negZeroReg, floatOpnd, instr->m_func));
    }

    // FCVTM/FCVTP intOpnd, floatOpnd
    IR::Opnd * intOpnd = IR::RegOpnd::New(TyInt32, this->m_func);
    instr->InsertBefore(IR::Instr::New((instr->m_opcode == Js::OpCode::InlineMathCeil) ? Js::OpCode::FCVTP : Js::OpCode::FCVTM, intOpnd, floatOpnd, instr->m_func));

    // EOR negZeroReg, #0x8000000000000000
    if (negZeroReg)
    {
        instr->InsertBefore(IR::Instr::New(Js::OpCode::EOR, negZeroReg, negZeroReg, IR::IntConstOpnd::New(0x8000000000000000ULL, IRType::TyInt64, this->m_func), instr->m_func));
    }

    // FCVTM would set FPSR.IOC (0th bit in FPSR) if the source cannot be represented within the destination register

    // MRS exceptReg, FPSR
    IR::Opnd * exceptReg = IR::RegOpnd::New(TyUint32, this->m_func);
    instr->InsertBefore(IR::Instr::New(Js::OpCode::MRS_FPSR, exceptReg, instr->m_func));

    // CBZ negZeroReg, bailout
    if (negZeroReg)
    {
        IR::BranchInstr * cbzInstr = IR::BranchInstr::New(Js::OpCode::CBZ, bailoutLabel, instr->m_func);
        cbzInstr->SetSrc1(negZeroReg);
        instr->InsertBefore(cbzInstr);
    }

    // TBZ exceptReg, #0, done
    IR::BranchInstr * tbzInstr = IR::BranchInstr::New(Js::OpCode::TBZ, doneLabel, instr->m_func);
    tbzInstr->SetSrc1(exceptReg);
    tbzInstr->SetSrc2(IR::IntConstOpnd::New(0, TyMachReg, instr->m_func));
    instr->InsertBefore(tbzInstr);

    IR::Opnd * dst = instr->UnlinkDst();
    instr->InsertAfter(doneLabel);
    if(!sharedBailout)
    {
        instr->InsertBefore(bailoutLabel);
    }

    // In case of a shared bailout, we should jump to the code that sets some data on the bailout record which is specific
    // to this bailout. Pass the bailoutLabel to GenerateFunction so that it may use the label as the collectRuntimeStatsLabel.
    this->m_lowerer->GenerateBailOut(instr, nullptr, nullptr, sharedBailout ? bailoutLabel : nullptr);

    // MOV dst, intOpnd
    IR::Instr* movInstr = IR::Instr::New(Js::OpCode::MOV, dst, intOpnd, this->m_func);
    doneLabel->InsertAfter(movInstr);
}

void
LowererMD::GenerateFastInlineBuiltInMathMinMax(IR::Instr* instr)
{
    IR::Opnd* dst = instr->GetDst();

    if (dst->IsInt32())
    {
        IR::Opnd* src1 = instr->GetSrc1();
        IR::Opnd* src2 = instr->GetSrc2();

        // CMP src1, src2
        IR::Instr* cmpInstr = IR::Instr::New(Js::OpCode::CMP, instr->m_func);
        cmpInstr->SetSrc1(src1);
        cmpInstr->SetSrc2(src2);
        instr->InsertBefore(cmpInstr);
        Legalize(cmpInstr);

        // (min) CSELLT dst, src1, src2
        // (max) CSELLT dst, src2, src1
        IR::Opnd* op1 = (instr->m_opcode == Js::OpCode::InlineMathMin) ? src1 : src2;
        IR::Opnd* op2 = (instr->m_opcode == Js::OpCode::InlineMathMin) ? src2 : src1;
        IR::Instr * csellinstr = IR::Instr::New(Js::OpCode::CSELLT, dst, op1, op2, instr->m_func);
        instr->InsertBefore(csellinstr);
        Legalize(csellinstr);

        instr->Remove();
    }

    else if (dst->IsFloat64() || dst->IsFloat32())
    {
        // WASM/JS min/max need NaN propagation (either operand NaN ->
        // canonical NaN) plus correct ordered comparison including +/-0
        // ties. ARM64 FMIN/FMAX implement minNum/maxNum for ordered inputs
        // but return the numeric operand when exactly one input is NaN, so
        // check each operand against itself first (unordered iff NaN) and
        // fall back to FMIN/FMAX otherwise:
        //   if (src1 != src1) goto nan; if (src2 != src2) goto nan;
        //   dst = FMIN/FMAX(src1, src2); goto done;
        // nan: dst = canonical NaN;
        // done:
        // (The previous code emitted bare FMIN/FMAX, miscompiling f32.min/
        // max entirely for f32 dsts and NaN inputs for f64: f32.min always
        // returned src1, f64.max(Inf,NaN) returned Inf.)
        const bool isMin = instr->m_opcode == Js::OpCode::InlineMathMin;
        IR::Opnd *src1 = instr->GetSrc1();
        IR::Opnd *src2 = instr->GetSrc2();
        IR::LabelInstr *labelNaN = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);
        IR::LabelInstr *labelDone = IR::LabelInstr::New(Js::OpCode::Label, m_func);
        this->m_lowerer->InsertCompareBranch(src1, src1, Js::OpCode::BrNeq_A, labelNaN, instr);
        this->m_lowerer->InsertCompareBranch(src2, src2, Js::OpCode::BrNeq_A, labelNaN, instr);
        instr->m_opcode = isMin ? Js::OpCode::FMIN : Js::OpCode::FMAX;
        IR::BranchInstr *brDone = IR::BranchInstr::New(Js::OpCode::B, labelDone, m_func);
        instr->InsertAfter(brDone);
        brDone->InsertAfter(labelNaN);
        labelNaN->InsertAfter(labelDone);
        // Canonical quiet NaN in the destination width, pooled by exact bits
        // (float<->double round-trips are exact, so the double carrier
        // preserves the f32 payload).
        double nanValue;
        if (dst->IsFloat32())
        {
            uint32 nanBits = 0x7FC00000u;
            float nanFloat;
            memcpy(&nanFloat, &nanBits, sizeof(nanFloat));
            nanValue = nanFloat;
        }
        else
        {
            uint64 nanBits = 0x7FF8000000000000ull;
            memcpy(&nanValue, &nanBits, sizeof(nanValue));
        }
        this->LoadFloatValue(dst, nanValue, labelDone);
    }
}

IR::Instr *
LowererMD::LowerToFloat(IR::Instr *instr)
{
    switch (instr->m_opcode)
    {
    case Js::OpCode::Add_A:
        instr->m_opcode = Js::OpCode::FADD;
        break;

    case Js::OpCode::Sub_A:
        instr->m_opcode = Js::OpCode::FSUB;
        break;

    case Js::OpCode::Mul_A:
        instr->m_opcode = Js::OpCode::FMUL;
        break;

    case Js::OpCode::Div_A:
        instr->m_opcode = Js::OpCode::FDIV;
        break;

    case Js::OpCode::Neg_A:
        instr->m_opcode = Js::OpCode::FNEG;
        break;

    case Js::OpCode::BrEq_A:
    case Js::OpCode::BrNeq_A:
    case Js::OpCode::BrSrEq_A:
    case Js::OpCode::BrSrNeq_A:
    case Js::OpCode::BrGt_A:
    case Js::OpCode::BrGe_A:
    case Js::OpCode::BrLt_A:
    case Js::OpCode::BrLe_A:
    case Js::OpCode::BrNotEq_A:
    case Js::OpCode::BrNotNeq_A:
    case Js::OpCode::BrSrNotEq_A:
    case Js::OpCode::BrSrNotNeq_A:
    case Js::OpCode::BrNotGt_A:
    case Js::OpCode::BrNotGe_A:
    case Js::OpCode::BrNotLt_A:
    case Js::OpCode::BrNotLe_A:
        return this->LowerFloatCondBranch(instr->AsBranchInstr());

    default:
        Assume(UNREACHED);
    }

    LegalizeMD::LegalizeInstr(instr);
    return instr;
}

IR::BranchInstr *
LowererMD::LowerFloatCondBranch(IR::BranchInstr *instrBranch, bool ignoreNaN)
{
    IR::Instr *instr;
    Js::OpCode brOpcode = Js::OpCode::InvalidOpCode;
    bool addNaNCheck = false;

    Func * func = instrBranch->m_func;
    IR::Opnd *src1 = instrBranch->UnlinkSrc1();
    IR::Opnd *src2 = instrBranch->UnlinkSrc2();

    IR::Instr *instrCmp = IR::Instr::New(Js::OpCode::FCMP, func);
    instrCmp->SetSrc1(src1);
    instrCmp->SetSrc2(src2);
    instrBranch->InsertBefore(instrCmp);
    LegalizeMD::LegalizeInstr(instrCmp);

    switch (instrBranch->m_opcode)
    {
        case Js::OpCode::BrSrEq_A:
        case Js::OpCode::BrEq_A:
        case Js::OpCode::BrNotNeq_A:
        case Js::OpCode::BrSrNotNeq_A:
                brOpcode = Js::OpCode::BEQ;
                break;

        case Js::OpCode::BrNeq_A:
        case Js::OpCode::BrSrNeq_A:
        case Js::OpCode::BrSrNotEq_A:
        case Js::OpCode::BrNotEq_A:
                brOpcode = Js::OpCode::BNE;
                addNaNCheck = !ignoreNaN;   //Special check for BNE as it is set when the operands are unordered (NaN).
                break;

        case Js::OpCode::BrLe_A:
                brOpcode = Js::OpCode::BLS; //Can't use BLE as it is set when the operands are unordered (NaN).
                break;

        case Js::OpCode::BrLt_A:
                brOpcode = Js::OpCode::BCC; //Can't use BLT  as is set when the operands are unordered (NaN).
                break;

        case Js::OpCode::BrGe_A:
                brOpcode = Js::OpCode::BGE;
                break;

        case Js::OpCode::BrGt_A:
                brOpcode = Js::OpCode::BGT;
                break;

        case Js::OpCode::BrNotLe_A:
                brOpcode = Js::OpCode::BHI;
                break;

        case Js::OpCode::BrNotLt_A:
                brOpcode = Js::OpCode::BPL;
                break;

        case Js::OpCode::BrNotGe_A:
                brOpcode = Js::OpCode::BLT;
                break;

        case Js::OpCode::BrNotGt_A:
                brOpcode = Js::OpCode::BLE;
                break;

        default:
            Assert(false);
            break;
    }

    if (addNaNCheck)
    {
        instr = IR::BranchInstr::New(Js::OpCode::BVS, instrBranch->GetTarget(), func);
        instrBranch->InsertBefore(instr);
    }

    instr = IR::BranchInstr::New(brOpcode, instrBranch->GetTarget(), func);
    instrBranch->InsertBefore(instr);

    instrBranch->Remove();

    return instr->AsBranchInstr();
}

void
LowererMD::EmitIntToFloat(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    IR::Instr *instr;

    Assert(dst->IsRegOpnd() && dst->IsFloat64());
    Assert(src->IsRegOpnd() && src->IsInt32());

    // Convert to Float
    instr = IR::Instr::New(Js::OpCode::FCVT, dst, src, this->m_func);
    instrInsert->InsertBefore(instr);
}

void
LowererMD::EmitUIntToFloat(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    IR::Instr *instr;

    Assert(dst->IsRegOpnd() && dst->IsFloat64());
    Assert(src->IsRegOpnd() && src->IsUInt32());

    // Convert to Float
    instr = IR::Instr::New(Js::OpCode::FCVT, dst, src, this->m_func);
    instrInsert->InsertBefore(instr);
}

void LowererMD::ConvertFloatToInt32(IR::Opnd* intOpnd, IR::Opnd* floatOpnd, IR::LabelInstr * labelHelper, IR::LabelInstr * labelDone, IR::Instr * instrInsert)
{
    Assert(floatOpnd->IsFloat64());
    Assert(intOpnd->IsInt32());

    // VCVTS32F64 dst.i32, src.f64
    // Convert to int
    // ARM64_WORKITEM: On ARM32 this used the current rounding mode; here we are explicitly rounding toward zero -- is that ok?
    IR::Instr * instr = IR::Instr::New(Js::OpCode::FCVTZ, intOpnd, floatOpnd, this->m_func);
    instrInsert->InsertBefore(instr);
    Legalize(instr);

    this->CheckOverflowOnFloatToInt32(instrInsert, intOpnd, labelHelper, labelDone);
}

void
LowererMD::EmitIntToLong(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    Assert(dst->IsRegOpnd() && dst->IsInt64());
    Assert(src->IsInt32());
    // Signed i32 -> i64 sign-extend; ChangeToAssign via InsertMove will
    // use SBFX (0,32) for signed. Keep InsertMove so LEGAL handling matches
    // other moves. Fallback to explicit SBFX if dst/src sizes differ and
    // InsertMove does not emit sign extension (defensive).
    Lowerer::InsertMove(dst, src, instrInsert);
}

void
LowererMD::EmitUIntToLong(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    Assert(dst->IsRegOpnd() && dst->IsInt64());
    Assert(src->IsUInt32());
    // Unsigned i32 -> i64 zero-extend: InsertMove uses UBFX.
    Lowerer::InsertMove(dst, src, instrInsert);
}

void
LowererMD::EmitLongToInt(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    Assert(dst->IsRegOpnd() && dst->IsInt32());
    Assert(src->IsInt64());
    // Truncate low 32 bits; MOV_TRUNC is the ARM64 truncating move.
    IR::Instr *instr = IR::Instr::New(Js::OpCode::MOV_TRUNC, dst, src, instrInsert->m_func);
    instrInsert->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::EmitFloat32ToFloat64(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    Assert(dst->IsRegOpnd() && dst->IsFloat64());
    Assert(src->IsRegOpnd() && src->GetType() == TyFloat32);
    IR::Instr *instr = IR::Instr::New(Js::OpCode::FCVT, dst, src, this->m_func);
    instrInsert->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::EmitInt64toFloat(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert)
{
    Assert(dst->IsFloat());
    Assert(src->IsInt64() || src->IsUInt64());
    // ARM64 FCVT supports i64/u64 -> f32/f64 directly via SCVTF/UFCVTF.
    // No need for x86's unsigned 2-step (SHR/AND/OR + ADDSD).
    IR::Instr *instr = IR::Instr::New(Js::OpCode::FCVT, dst, src, this->m_func);
    instrInsert->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::EmitSignExtend(IR::Instr * instr)
{
    IR::Opnd* dst = instr->GetDst();
    IR::Opnd* src1 = instr->GetSrc1();
    IR::Opnd* src2 = instr->GetSrc2();
    Assert(dst && src1 && src2);
    Assert(src2->GetSize() < dst->GetSize());
    IRType fromType = src2->GetType();
    int32 fromBits = (int32)(src2->GetSize() * MachBits); // 8,16,32
    // ARM64: use SBFX 0, fromBits for sign extension
    IR::Instr *sbfx = IR::Instr::New(Js::OpCode::SBFX, dst, src1->UseWithNewType(fromType, m_func),
        IR::IntConstOpnd::New(BITFIELD(0, fromBits), TyMachReg, m_func, true), m_func);
    instr->InsertBefore(sbfx);
    LegalizeMD::LegalizeInstr(sbfx);
}

void
LowererMD::EmitReinterpretPrimitive(IR::Opnd* dst, IR::Opnd* src, IR::Instr* insertBeforeInstr)
{
    Assert(dst && src);
    Assert(dst->GetSize() == src->GetSize());
    Assert(dst->GetType() != src->GetType());
    // Bitcast: float <-> int of same size via FMOV_GEN (scalar FMOV/UMOV).
    // FMOV_GEN is LEGAL_REG2, so a non-register source (e.g. an i64 const)
    // is hoisted to a register by the legalizer (LegalizeSrc handles
    // Int64ConstOpnd via LDIMM).
    IR::Instr *instr = IR::Instr::New(Js::OpCode::FMOV_GEN, dst, src, m_func);
    insertBeforeInstr->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
}

void
LowererMD::GenerateCopysign(IR::Instr *instr)
{
    // dst = copysign(src1, src2): magnitude of src1, sign of src2.
    // The shared lowering emits x86 SSE (ANDPS/ORPS), which ARM64 does not
    // implement, so bit-twiddle in the integer domain with proven
    // primitives (FMOV_GEN, LDIMM-hoisted masks, AND/ORR):
    //   i1 = reinterpret(src1); i2 = reinterpret(src2)
    //   i1 &= absMask; i2 &= signMask; i = i1 | i2; dst = reinterpret(i)
    IR::Opnd *dst = instr->GetDst();
    IR::Opnd *src1 = instr->GetSrc1();
    IR::Opnd *src2 = instr->GetSrc2();
    const bool is32 = (dst->GetSize() == 4);
    IRType intType = is32 ? TyInt32 : TyInt64;
    const uint64 absBits = is32 ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull;
    const uint64 signBits = is32 ? 0x80000000ull : 0x8000000000000000ull;

    IR::RegOpnd *i1 = IR::RegOpnd::New(intType, m_func);
    IR::RegOpnd *i2 = IR::RegOpnd::New(intType, m_func);
    this->EmitReinterpretPrimitive(i1, src1, instr);
    this->EmitReinterpretPrimitive(i2, src2, instr);

    IR::RegOpnd *absMask = IR::RegOpnd::New(intType, m_func);
    IR::RegOpnd *signMask = IR::RegOpnd::New(intType, m_func);
    if (is32)
    {
        Lowerer::InsertMove(absMask, IR::IntConstOpnd::New((int32)absBits, intType, m_func), instr);
        Lowerer::InsertMove(signMask, IR::IntConstOpnd::New((int32)signBits, intType, m_func), instr);
    }
    else
    {
        Lowerer::InsertMove(absMask, IR::Int64ConstOpnd::New((int64)absBits, intType, m_func), instr);
        Lowerer::InsertMove(signMask, IR::Int64ConstOpnd::New((int64)signBits, intType, m_func), instr);
    }

    IR::Instr *and1 = IR::Instr::New(Js::OpCode::AND, i1, i1, absMask, m_func);
    instr->InsertBefore(and1);
    LegalizeMD::LegalizeInstr(and1);
    IR::Instr *and2 = IR::Instr::New(Js::OpCode::AND, i2, i2, signMask, m_func);
    instr->InsertBefore(and2);
    LegalizeMD::LegalizeInstr(and2);
    IR::Instr *orr = IR::Instr::New(Js::OpCode::ORR, i1, i1, i2, m_func);
    instr->InsertBefore(orr);
    LegalizeMD::LegalizeInstr(orr);

    this->EmitReinterpretPrimitive(dst, i1, instr);
    instr->Remove();
}

void
LowererMD::CheckOverflowOnFloatToInt32(IR::Instr* instrInsert, IR::Opnd* intOpnd, IR::LabelInstr * labelHelper, IR::LabelInstr * labelDone)
{
    // Test for 0x80000000 or 0x7FFFFFFF

    // tmp = EOR src, 0x80000000;   gives 0 or -1 for overflow values
    // tmp = EOR_ASR31 tmp, tmp;    tmp = tmp ^ ((int32)tmp >> 31) -- converts -1 or 0 to 0
    // CBZ tmp, helper;             branch if tmp was -1 or 0
    // B done;

    IR::RegOpnd* tmp = IR::RegOpnd::New(TyInt32, this->m_func);

    IR::Instr* instr = IR::Instr::New(Js::OpCode::EOR, tmp, intOpnd, IR::IntConstOpnd::New(0x80000000, TyUint32, this->m_func, true), this->m_func);
    instrInsert->InsertBefore(instr);

    instr = IR::Instr::New(Js::OpCode::EOR_ASR31, tmp, tmp, tmp, this->m_func);
    instrInsert->InsertBefore(instr);

    // CBZ $helper
    instr = IR::BranchInstr::New(Js::OpCode::CBZ, labelHelper, this->m_func);
    instr->SetSrc1(tmp);
    instrInsert->InsertBefore(instr);

    // B $done
    instr = IR::BranchInstr::New(Js::OpCode::B, labelDone, this->m_func);
    instrInsert->InsertBefore(instr);
}

void
LowererMD::EmitFloatToInt(IR::Opnd *dst, IR::Opnd *src, IR::Instr *instrInsert, IR::Instr * instrBailOut, IR::LabelInstr * labelBailOut)
{    
    IR::BailOutKind bailOutKind = IR::BailOutInvalid;
    if (instrBailOut && instrBailOut->HasBailOutInfo())
    {
        bailOutKind = instrBailOut->GetBailOutKind(); 
        if (bailOutKind & IR::BailOutOnArrayAccessHelperCall)
        {
            // Bail out instead of calling helper. If this is happening unconditionally, the caller should instead throw a rejit exception.
            Assert(labelBailOut);
            m_lowerer->InsertBranch(Js::OpCode::Br, labelBailOut, instrInsert);
            return;
        }
    }

    IR::LabelInstr *labelDone = IR::LabelInstr::New(Js::OpCode::Label, this->m_func);
    IR::LabelInstr *labelHelper = IR::LabelInstr::New(Js::OpCode::Label, this->m_func, true);
    IR::Instr *instr;

    ConvertFloatToInt32(dst, src, labelHelper, labelDone, instrInsert);

    // $Helper
    instrInsert->InsertBefore(labelHelper);

    instr = IR::Instr::New(Js::OpCode::Call, dst, this->m_func);
    instrInsert->InsertBefore(instr);

    if (BailOutInfo::IsBailOutOnImplicitCalls(bailOutKind))
    {
        _Analysis_assume_(instrBailOut != nullptr);
        instr = instr->ConvertToBailOutInstr(instrBailOut->GetBailOutInfo(), bailOutKind);
        if (instrBailOut->GetBailOutInfo()->bailOutInstr == instrBailOut)
        {
            IR::Instr * instrShare = instrBailOut->ShareBailOut();
            m_lowerer->LowerBailTarget(instrShare);
        }
    }

    // dst = ToInt32Core(src);
    LoadDoubleHelperArgument(instr, src);

    this->ChangeToHelperCall(instr, IR::HelperConv_ToInt32Core);

    // $Done
    instrInsert->InsertBefore(labelDone);
}

IR::Instr *
LowererMD::InsertConvertFloat64ToInt32(const RoundMode roundMode, IR::Opnd *const dst, IR::Opnd *const src, IR::Instr *const insertBeforeInstr)
{
    Assert(dst);
    Assert(dst->IsInt32());
    Assert(src);
    Assert(src->IsFloat64());
    Assert(insertBeforeInstr);

    // The caller is expected to check for overflow. To have that work be done automatically, use LowererMD::EmitFloatToInt.

    Func *const func = insertBeforeInstr->m_func;
    IR::AutoReuseOpnd autoReuseSrcPlusHalf;
    IR::Instr *instr = nullptr;

    switch (roundMode)
    {
        case RoundModeTowardInteger:
        case RoundModeHalfToEven:
        {
            // Conversion with rounding towards nearest integer is not supported by the architecture. Add 0.5 and do a
            // round-toward-zero conversion instead.
            IR::RegOpnd *const srcPlusHalf = IR::RegOpnd::New(TyFloat64, func);
            autoReuseSrcPlusHalf.Initialize(srcPlusHalf, func);
            Lowerer::InsertAdd(
                false /* needFlags */,
                srcPlusHalf,
                src,
                IR::MemRefOpnd::New(insertBeforeInstr->m_func->GetThreadContextInfo()->GetDoublePointFiveAddr(), TyFloat64, func,
                    IR::AddrOpndKindDynamicDoubleRef),
                insertBeforeInstr);

            instr = IR::Instr::New(LowererMD::MDConvertFloat64ToInt32Opcode(RoundModeTowardZero), dst, srcPlusHalf, func);

            insertBeforeInstr->InsertBefore(instr);
            LowererMD::Legalize(instr);
            return instr;
        }
        default:
            AssertMsg(0, "RoundMode not supported.");
            return nullptr;
    }
}

IR::Instr *
LowererMD::LoadFloatZero(IR::Opnd * opndDst, IR::Instr * instrInsert)
{
    Assert(opndDst->GetType() == TyFloat64);
    IR::Opnd * zero = IR::MemRefOpnd::New(instrInsert->m_func->GetThreadContextInfo()->GetDoubleZeroAddr(), TyFloat64, instrInsert->m_func, IR::AddrOpndKindDynamicDoubleRef);

    // Todo(magardn): Make sure the correct opcode is used for moving between float and non-float regs (FMOV_GEN)
    return Lowerer::InsertMove(opndDst, zero, instrInsert);
}

IR::Instr *
LowererMD::LoadFloatValue(IR::Opnd * opndDst, float value, IR::Instr * instrInsert)
{
    // Float overload: pool the 4 float bytes directly. Without this overload,
    // a float argument binds to the double overload below (name hiding) and
    // round-trips float->double->float in C++, which quiets signaling NaNs
    // (f32.const nan:0x200000 pooled as 0x7FE00000 instead of 0x7FA00000).
    if (opndDst->IsFloat64())
    {
        // Widening move (float const to double dst): keep the historical
        // convert-then-pool behavior.
        return LowererMD::LoadFloatValue(opndDst, (double)value,
                                         instrInsert);
    }
    Assert(opndDst->IsFloat32());
    void * pValue = NativeCodeDataNewNoFixup(instrInsert->m_func->GetNativeCodeDataAllocator(), FloatType<DataDesc_LowererMD_LoadFloatValue_Float>, value);
    IR::Opnd * opnd;
    if (instrInsert->m_func->IsOOPJIT())
    {
        int offset = NativeCodeData::GetDataTotalOffset(pValue);
        auto addressRegOpnd = IR::RegOpnd::New(TyMachPtr, instrInsert->m_func);

        Lowerer::InsertMove(
            addressRegOpnd,
            IR::MemRefOpnd::New(instrInsert->m_func->GetWorkItem()->GetWorkItemData()->nativeDataAddr, TyMachPtr, instrInsert->m_func, IR::AddrOpndKindDynamicNativeCodeDataRef),
            instrInsert);

        opnd = IR::IndirOpnd::New(addressRegOpnd, offset, TyFloat32,
#if DBG
            NativeCodeData::GetDataDescription(pValue, instrInsert->m_func->m_alloc),
#endif
            instrInsert->m_func, true);
    }
    else
    {
        opnd = IR::MemRefOpnd::New((void*)pValue, TyFloat32, instrInsert->m_func);
    }
    IR::Instr * instr = IR::Instr::New(Js::OpCode::FLDR, opndDst, opnd, instrInsert->m_func);
    instrInsert->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
    return instr;
}

IR::Instr *
LowererMD::LoadFloatValue(IR::Opnd * opndDst, double value, IR::Instr * instrInsert)
{
    // Pool and load the constant in the destination's width (mirrors the
    // shared LowererMD::LoadFloatValue<T> template, which is not compiled
    // on ARM64). A float32 destination must load 4 float bytes: pooling the
    // double bits and issuing a 64-bit load leaves the low word (which the
    // s-register consumer reads) zero, silently producing 0.0f.
    const bool isFloat64 = opndDst->IsFloat64();
    Assert(isFloat64 || opndDst->IsFloat32());
    // Floating point zero is a common value to load.  Let's use a single memory location instead of allocating new memory for each.
    const bool isFloatZero = value == 0.0 && !Js::JavascriptNumber::IsNegZero(value); // (-0.0 == 0.0) yields true
    if (isFloatZero && isFloat64)
    {
        return LowererMD::LoadFloatZero(opndDst, instrInsert);
    }
    void * pValue;
    IR::Opnd * opnd;
    if (isFloat64)
    {
        pValue = NativeCodeDataNewNoFixup(instrInsert->m_func->GetNativeCodeDataAllocator(), DoubleType<DataDesc_LowererMD_LoadFloatValue_Double>, value);
    }
    else
    {
        pValue = NativeCodeDataNewNoFixup(instrInsert->m_func->GetNativeCodeDataAllocator(), FloatType<DataDesc_LowererMD_LoadFloatValue_Float>, (float)value);
    }
    if (instrInsert->m_func->IsOOPJIT())
    {
        int offset = NativeCodeData::GetDataTotalOffset(pValue);
        auto addressRegOpnd = IR::RegOpnd::New(TyMachPtr, instrInsert->m_func);

        Lowerer::InsertMove(
            addressRegOpnd,
            IR::MemRefOpnd::New(instrInsert->m_func->GetWorkItem()->GetWorkItemData()->nativeDataAddr, TyMachPtr, instrInsert->m_func, IR::AddrOpndKindDynamicNativeCodeDataRef),
            instrInsert);

        opnd = IR::IndirOpnd::New(addressRegOpnd, offset, isFloat64 ? TyMachDouble : TyFloat32,
#if DBG
            NativeCodeData::GetDataDescription(pValue, instrInsert->m_func->m_alloc),
#endif
            instrInsert->m_func, true);
    }
    else
    {
        opnd = IR::MemRefOpnd::New((void*)pValue, isFloat64 ? TyMachDouble : TyFloat32, instrInsert->m_func);
    }
    IR::Instr * instr = IR::Instr::New(Js::OpCode::FLDR, opndDst, opnd, instrInsert->m_func);
    instrInsert->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);
    return instr;
}

void LowererMD::GenerateFloatTest(IR::RegOpnd * opndSrc, IR::Instr * insertInstr, IR::LabelInstr* labelHelper, const bool checkForNullInLoopBody)
{
    if (opndSrc->GetValueType().IsFloat())
    {
        return;
    }

    // TST s1, floatTagReg
    IR::Opnd* floatTag = IR::IntConstOpnd::New(Js::FloatTag_Value, TyMachReg, this->m_func, /* dontEncode = */ true);
    IR::Instr* instr = IR::Instr::New(Js::OpCode::TST, this->m_func);
    instr->SetSrc1(opndSrc);
    instr->SetSrc2(floatTag);
    insertInstr->InsertBefore(instr);
    LegalizeMD::LegalizeInstr(instr);

    // BZ $helper
    instr = IR::BranchInstr::New(Js::OpCode::BEQ /* BZ */, labelHelper, this->m_func);
    insertInstr->InsertBefore(instr);
}

IR::RegOpnd* LowererMD::CheckFloatAndUntag(IR::RegOpnd * opndSrc, IR::Instr * insertInstr, IR::LabelInstr* labelHelper)
{
    IR::Opnd* floatTag = IR::IntConstOpnd::New(Js::FloatTag_Value, TyMachReg, this->m_func, /* dontEncode = */ true);

    // MOV floatTagReg, FloatTag_Value
    if (!opndSrc->GetValueType().IsFloat())
    {
        // TST s1, floatTagReg
        IR::Instr* instr = IR::Instr::New(Js::OpCode::TST, this->m_func);
        instr->SetSrc1(opndSrc);
        instr->SetSrc2(floatTag);
        insertInstr->InsertBefore(instr);
        LegalizeMD::LegalizeInstr(instr);

        // BZ $helper
        instr = IR::BranchInstr::New(Js::OpCode::BEQ /* BZ */, labelHelper, this->m_func);
        insertInstr->InsertBefore(instr);
    }

    IR::RegOpnd* untaggedFloat = IR::RegOpnd::New(TyMachReg, this->m_func);
    IR::Instr* instr = IR::Instr::New(Js::OpCode::EOR, untaggedFloat, opndSrc, floatTag, this->m_func);
    insertInstr->InsertBefore(instr);

    IR::RegOpnd *floatReg = IR::RegOpnd::New(TyMachDouble, this->m_func);
    instr = IR::Instr::New(Js::OpCode::FMOV_GEN, floatReg, untaggedFloat, this->m_func);
    insertInstr->InsertBefore(instr);
    return floatReg;
}

template <bool verify>
void
LowererMD::Legalize(IR::Instr *const instr, bool fPostRegAlloc)
{
    if (verify)
    {
        // NYI for the rest of legalization
        return;
    }
    LegalizeMD::LegalizeInstr(instr);
}

template void LowererMD::Legalize<false>(IR::Instr *const instr, bool fPostRegalloc);
#if DBG
template void LowererMD::Legalize<true>(IR::Instr *const instr, bool fPostRegalloc);
#endif

void
LowererMD::FinalLower()
{
    NoRecoverMemoryArenaAllocator tempAlloc(_u("BE-ARMFinalLower"), m_func->m_alloc->GetPageAllocator(), Js::Throw::OutOfMemory);
    EncodeReloc *pRelocList = nullptr;

    size_t totalJmpTableSizeInBytes = 0;

    uintptr_t instrOffset = 0;
    FOREACH_INSTR_BACKWARD_EDITING_IN_RANGE(instr, instrPrev, this->m_func->m_tailInstr, this->m_func->m_headInstr)
    {
        if (instr->IsLowered() == false)
        {
            if (instr->IsLabelInstr())
            {
                //This is not the real set, Real offset gets set in encoder.
                IR::LabelInstr *labelInstr = instr->AsLabelInstr();
                labelInstr->SetOffset(instrOffset);
            }

            switch (instr->m_opcode)
            {
            case Js::OpCode::Ret:
                instr->Remove();
                break;
            case Js::OpCode::Leave:
                Assert(this->m_func->DoOptimizeTry() && !this->m_func->IsLoopBodyInTry());
                instrPrev = m_lowerer->LowerLeave(instr, instr->AsBranchInstr()->GetTarget(), true /*fromFinalLower*/);
                break;
            }
        }
        else
        {
            instrOffset = instrOffset + MachMaxInstrSize;

            if (instr->IsBranchInstr())
            {
                IR::BranchInstr *branchInstr = instr->AsBranchInstr();

                if (branchInstr->IsMultiBranch())
                {
                    Assert(instr->GetSrc1() && instr->GetSrc1()->IsRegOpnd());
                    IR::MultiBranchInstr * multiBranchInstr = instr->AsBranchInstr()->AsMultiBrInstr();

                    if (multiBranchInstr->m_isSwitchBr &&
                        (multiBranchInstr->m_kind == IR::MultiBranchInstr::IntJumpTable || multiBranchInstr->m_kind == IR::MultiBranchInstr::SingleCharStrJumpTable))
                    {
                        BranchJumpTableWrapper * branchJumpTableWrapper = multiBranchInstr->GetBranchJumpTable();
                        totalJmpTableSizeInBytes += (branchJumpTableWrapper->tableSize * sizeof(void*));

                        // instrOffset is relative to the end of the function. Jump tables come after the function and so would result in negative offsets. label offsets
                        // are unsigned so instead give jump table lables offsets relative to the end of the jump table section.
                        branchJumpTableWrapper->labelInstr->SetOffset(totalJmpTableSizeInBytes);
                    }
                }
                else if (!LowererMD::IsUnconditionalBranch(branchInstr)) //Ignore other direct branches
                {
                    uintptr_t targetOffset = branchInstr->GetTarget()->GetOffset();

                    if (targetOffset != 0)
                    {
                        // this is forward reference
                        if (LegalizeMD::LegalizeDirectBranch(branchInstr, instrOffset))
                        {
                            //There might be an instruction inserted for legalizing conditional branch
                            instrOffset = instrOffset + MachMaxInstrSize;
                        }
                    }
                    else
                    {
                        EncodeReloc::New(&pRelocList, RelocTypeBranch19, (BYTE*)instrOffset, branchInstr, &tempAlloc);
                        //Assume this is a backward long branch, we shall fix up after complete pass, be conservative here
                        instrOffset = instrOffset + MachMaxInstrSize;
                    }
                }
            }
            else if (LowererMD::IsAssign(instr) || instr->m_opcode == Js::OpCode::LEA || instr->m_opcode == Js::OpCode::LDARGOUTSZ || instr->m_opcode == Js::OpCode::REM || instr->m_opcode == Js::OpCode::UREM)
            {
                // Cleanup spill code
                // INSTR_BACKWARD_EDITING_IN_RANGE implies that next loop iteration will use instrPrev (instr->m_prev computed before entering current loop iteration).
                IR::Instr* instrNext = instr->m_next;
                bool canExpand = this->FinalLowerAssign(instr);

                if (canExpand)
                {
                    uint32 expandedInstrCount = 0;   // The number of instrs the LDIMM expands into.
                    FOREACH_INSTR_IN_RANGE(instrCount, instrPrev->m_next, instrNext)
                    {
                        ++expandedInstrCount;
                    }
                    NEXT_INSTR_IN_RANGE;
                    Assert(expandedInstrCount > 0);

                    // Adjust the offset for expanded instrs.
                    instrOffset += (expandedInstrCount - 1) * MachMaxInstrSize;    // We already accounted for one MachMaxInstrSize.
                }
            }

            if (instr->m_opcode == Js::OpCode::ADR)
            {
                IR::LabelInstr* label = instr->GetSrc1()->AsLabelOpnd()->GetLabel();
                if (label->GetOffset() != 0 && !label->m_isDataLabel)
                {
                    // this is forward reference
                    if (LegalizeMD::LegalizeAdrOffset(instr, instrOffset))
                    {
                        //Additional instructions were inserted.
                        instrOffset = instrOffset + MachMaxInstrSize * 2;
                    }
                }
                else
                {
                    EncodeReloc::New(&pRelocList, RelocTypeLabelAdr, (BYTE*)instrOffset, instr, &tempAlloc);
                    //Assume this is a backward long branch, we shall fix up after complete pass, be conservative here
                    instrOffset = instrOffset + MachMaxInstrSize * 2;
                }
            }
        }
    } NEXT_INSTR_BACKWARD_EDITING_IN_RANGE;

    //Fixup all the backward branches
    for (EncodeReloc *reloc = pRelocList; reloc; reloc = reloc->m_next)
    {
        uintptr_t relocAddress = (uintptr_t)reloc->m_consumerOffset;

        switch (reloc->m_relocType)
        {
        case RelocTypeBranch19:
            AssertMsg(relocAddress < reloc->m_relocInstr->AsBranchInstr()->GetTarget()->GetOffset(), "Only backward branches require fixup");
            LegalizeMD::LegalizeDirectBranch(reloc->m_relocInstr->AsBranchInstr(), relocAddress);
            break;

        case RelocTypeLabelAdr:
        {
            IR::LabelInstr* label = reloc->m_relocInstr->GetSrc1()->AsLabelOpnd()->GetLabel();
            if (label->m_isDataLabel)
            {
                uintptr_t dataOffset;
                if (label == m_func->GetFuncStartLabel())
                {
                    dataOffset = instrOffset - relocAddress;
                }
                else if (label == m_func->GetFuncEndLabel())
                {
                    dataOffset = relocAddress;
                }
                else
                {
                    Assert(label->GetOffset() != 0);

                    // jump table label offsets are relative to the end of the jump table area.
                    dataOffset = relocAddress + totalJmpTableSizeInBytes - label->GetOffset();

                    // PC is a union with offset. Encoder expects this to be nullptr for jump table labels.
                    label->SetPC(nullptr);
                }

                LegalizeMD::LegalizeDataAdr(reloc->m_relocInstr, dataOffset);
                break;
            }

            AssertMsg(relocAddress < label->GetOffset(), "Only backward branches require fixup");
            LegalizeMD::LegalizeAdrOffset(reloc->m_relocInstr, relocAddress);
            break;
        }
        default:
            Assert(false);
        }
    }
}

// Returns true, if and only if the assign may expand into multiple instrs.
bool
LowererMD::FinalLowerAssign(IR::Instr * instr)
{
    if (instr->m_opcode == Js::OpCode::LDIMM)
    {
        LegalizeMD::LegalizeInstr(instr);

        // LDIMM can expand into up to 4 instructions when the immediate is more than 16 bytes,
        // it can also expand into multiple different no-op (normally MOV) instrs when we obfuscate it, which is randomly.
        return true;
    }
    else if (EncoderMD::IsLoad(instr) || instr->m_opcode == Js::OpCode::LEA)
    {
        Assert(instr->GetDst()->IsRegOpnd());
        if (!instr->GetSrc1()->IsRegOpnd())
        {
            LegalizeMD::LegalizeSrc(instr, instr->GetSrc1(), 1);
            return true;
        }
        instr->m_opcode = instr->GetSrc1()->IsFloat() ? Js::OpCode::FMOV : Js::OpCode::MOV;
    }
    else if (EncoderMD::IsStore(instr))
    {
        Assert(instr->GetSrc1()->IsRegOpnd());
        if (!instr->GetDst()->IsRegOpnd())
        {
            LegalizeMD::LegalizeDst(instr);
            return true;
        }
        instr->m_opcode = instr->GetDst()->IsFloat() ? Js::OpCode::FMOV : Js::OpCode::MOV;
    }
    else if (instr->m_opcode == Js::OpCode::LDARGOUTSZ)
    {
        Assert(instr->GetDst()->IsRegOpnd());
        Assert((instr->GetSrc1() == nullptr) && (instr->GetSrc2() == nullptr));
        // dst = LDARGOUTSZ
        // This loads the function's arg out area size into the dst operand. We need a pseudo-op,
        // because we generate the instruction during Lower but don't yet know the value of the constant it needs
        // to load. Change it to the appropriate LDIMM here.
        uint32 argOutSize = UInt32Math::Mul(this->m_func->m_argSlotsForFunctionsCalled, MachRegInt, Js::Throw::OutOfMemory);
        instr->SetSrc1(IR::IntConstOpnd::New(argOutSize, TyMachReg, this->m_func));
        instr->m_opcode = Js::OpCode::LDIMM;
        LegalizeMD::LegalizeInstr(instr);
        return true;
    }
    else if (instr->m_opcode == Js::OpCode::REM)
    {
        IR::Opnd* dst = instr->GetDst();
        IR::Opnd* src1 = instr->GetSrc1();
        IR::Opnd* src2 = instr->GetSrc2();

        Assert(src1->IsRegOpnd());
        Assert(src2->IsRegOpnd());

        RegNum dstReg = dst->AsRegOpnd()->GetReg();

        if (dstReg == src1->AsRegOpnd()->GetReg() || dstReg == src2->AsRegOpnd()->GetReg())
        {
            Assert(src1->AsRegOpnd()->GetReg() != SCRATCH_REG);
            Assert(src2->AsRegOpnd()->GetReg() != SCRATCH_REG);
            Assert(src1->GetType() == src2->GetType());

            // r17 = SDIV src1, src2
            IR::RegOpnd *regScratch = IR::RegOpnd::New(nullptr, SCRATCH_REG, src1->GetType(), instr->m_func);
            IR::Instr *insertInstr = IR::Instr::New(Js::OpCode::SDIV, regScratch, src1, src2, instr->m_func);
            instr->InsertBefore(insertInstr);

            // r17 = MSUB src1, src2, r17 (r17 = src1 - src2 * r17)
            insertInstr = IR::Instr::New(Js::OpCode::MSUB, regScratch, src1, src2, instr->m_func);
            instr->InsertBefore(insertInstr);

            // mov dst, r17
            insertInstr = IR::Instr::New(dst->IsFloat() ? Js::OpCode::FMOV : Js::OpCode::MOV, dst, regScratch, instr->m_func);
            instr->InsertBefore(insertInstr);
            instr->Remove();
        }
        else
        {
            // dst = SDIV src1, src2
            IR::Instr *divInstr = IR::Instr::New(Js::OpCode::SDIV, dst, src1, src2, instr->m_func);
            instr->InsertBefore(divInstr);

            // dst = MSUB src1, src2, dst (dst = src1 - src2 * dst)
            instr->m_opcode = Js::OpCode::MSUB;
        }
        return true;
    }
    else if (instr->m_opcode == Js::OpCode::UREM)
    {
        IR::Opnd* dst = instr->GetDst();
        IR::Opnd* src1 = instr->GetSrc1();
        IR::Opnd* src2 = instr->GetSrc2();

        Assert(src1->IsRegOpnd());
        Assert(src2->IsRegOpnd());

        RegNum dstReg = dst->AsRegOpnd()->GetReg();

        if (dstReg == src1->AsRegOpnd()->GetReg() || dstReg == src2->AsRegOpnd()->GetReg())
        {
            Assert(src1->AsRegOpnd()->GetReg() != SCRATCH_REG);
            Assert(src2->AsRegOpnd()->GetReg() != SCRATCH_REG);
            Assert(src1->GetType() == src2->GetType());

            // r17 = UDIV src1, src2
            IR::RegOpnd *regScratch = IR::RegOpnd::New(nullptr, SCRATCH_REG, src1->GetType(), instr->m_func);
            IR::Instr *insertInstr = IR::Instr::New(Js::OpCode::UDIV, regScratch, src1, src2, instr->m_func);
            instr->InsertBefore(insertInstr);

            // r17 = MSUB src1, src2, r17 (r17 = src1 - src2 * r17)
            insertInstr = IR::Instr::New(Js::OpCode::MSUB, regScratch, src1, src2, instr->m_func);
            instr->InsertBefore(insertInstr);

            // mov dst, r17
            insertInstr = IR::Instr::New(dst->IsFloat() ? Js::OpCode::FMOV : Js::OpCode::MOV, dst, regScratch, instr->m_func);
            instr->InsertBefore(insertInstr);
            instr->Remove();
        }
        else
        {
            // dst = UDIV src1, src2
            IR::Instr *divInstr = IR::Instr::New(Js::OpCode::UDIV, dst, src1, src2, instr->m_func);
            instr->InsertBefore(divInstr);

            // dst = MSUB src1, src2, dst (dst = src1 - src2 * dst)
            instr->m_opcode = Js::OpCode::MSUB;
        }
        return true;
    }

    return false;
}
IR::Opnd *
LowererMD::GenerateArgOutForStackArgs(IR::Instr* callInstr, IR::Instr* stackArgsInstr)
{
    return this->m_lowerer->GenerateArgOutForStackArgs(callInstr, stackArgsInstr);
}

IR::Instr *
LowererMD::LowerDivI4AndBailOnReminder(IR::Instr * instr, IR::LabelInstr * bailOutLabel)
{
    //      result = SDIV numerator, denominator
    //   mulResult = MUL result, denominator
    //               CMP mulResult, numerator
    //               BNE bailout
    //               <Caller insert more checks here>
    //         dst = MOV result                             <-- insertBeforeInstr


    instr->m_opcode = Js::OpCode::SDIV;

    // delay assigning to the final dst.
    IR::Instr * sinkedInstr = instr->SinkDst(Js::OpCode::MOV);
    LegalizeMD::LegalizeInstr(instr);
    LegalizeMD::LegalizeInstr(sinkedInstr);

    IR::Opnd * resultOpnd = instr->GetDst();
    IR::Opnd * numerator = instr->GetSrc1();
    IR::Opnd * denominatorOpnd = instr->GetSrc2();

    // Insert all check before the assignment to the actual
    IR::Instr * insertBeforeInstr = instr->m_next;

    // Jump to bailout if the reminder is not 0 (or the divResult * denominator is not same as the numerator)
    IR::RegOpnd * mulResult = IR::RegOpnd::New(TyInt32, m_func);
    IR::Instr * mulInstr = IR::Instr::New(Js::OpCode::MUL, mulResult, resultOpnd, denominatorOpnd, m_func);
    insertBeforeInstr->InsertBefore(mulInstr);
    LegalizeMD::LegalizeInstr(mulInstr);

    this->m_lowerer->InsertCompareBranch(mulResult, numerator, Js::OpCode::BrNeq_A, bailOutLabel, insertBeforeInstr);
    return insertBeforeInstr;
}

void
LowererMD::LowerInlineSpreadArgOutLoop(IR::Instr *callInstr, IR::RegOpnd *indexOpnd, IR::RegOpnd *arrayElementsStartOpnd)
{
    this->m_lowerer->LowerInlineSpreadArgOutLoopUsingRegisters(callInstr, indexOpnd, arrayElementsStartOpnd);
}

void
LowererMD::LowerTypeof(IR::Instr* typeOfInstr)
{
    Func * func = typeOfInstr->m_func;
    IR::Opnd * src1 = typeOfInstr->GetSrc1();
    IR::Opnd * dst = typeOfInstr->GetDst();
    Assert(src1->IsRegOpnd() && dst->IsRegOpnd());
    IR::LabelInstr * helperLabel = IR::LabelInstr::New(Js::OpCode::Label, func, true);
    IR::LabelInstr * taggedIntLabel = IR::LabelInstr::New(Js::OpCode::Label, func);
    IR::LabelInstr * doneLabel = IR::LabelInstr::New(Js::OpCode::Label, func);

    // MOV typeDisplayStringsArray, &javascriptLibrary->typeDisplayStrings
    IR::RegOpnd * typeDisplayStringsArrayOpnd = IR::RegOpnd::New(TyMachPtr, func);
    m_lowerer->InsertMove(typeDisplayStringsArrayOpnd, IR::AddrOpnd::New((BYTE*)m_func->GetScriptContextInfo()->GetLibraryAddr() + Js::JavascriptLibrary::GetTypeDisplayStringsOffset(), IR::AddrOpndKindConstantAddress, this->m_func), typeOfInstr);

    GenerateObjectTest(src1, typeOfInstr, taggedIntLabel);

    // MOV typeRegOpnd, [src1 + offset(Type)]
    IR::RegOpnd * typeRegOpnd = IR::RegOpnd::New(TyMachReg, func);
    m_lowerer->InsertMove(typeRegOpnd,
        IR::IndirOpnd::New(src1->AsRegOpnd(), Js::RecyclableObject::GetOffsetOfType(), TyMachReg, func),
        typeOfInstr);

    IR::LabelInstr * falsyLabel = IR::LabelInstr::New(Js::OpCode::Label, func);
    m_lowerer->GenerateFalsyObjectTest(typeOfInstr, typeRegOpnd, falsyLabel);

    // <$not falsy>
    //   MOV typeId, TypeIds_Object
    //   MOV objTypeId, [typeRegOpnd + offsetof(typeId)]
    //   CMP objTypeId, TypeIds_Limit                                      /*external object test*/
    //   BCS $externalObjectLabel
    //   MOV typeId, objTypeId
    // $loadTypeDisplayStringLabel:
    //   MOV dst, typeDisplayStrings[typeId]
    //   TEST dst, dst
    //   BEQ $helper
    //   B $done
    IR::RegOpnd * typeIdOpnd = IR::RegOpnd::New(TyUint32, func);
    m_lowerer->InsertMove(typeIdOpnd, IR::IntConstOpnd::New(Js::TypeIds_Object, TyUint32, func), typeOfInstr);

    IR::RegOpnd * objTypeIdOpnd = IR::RegOpnd::New(TyUint32, func);
    m_lowerer->InsertMove(objTypeIdOpnd, IR::IndirOpnd::New(typeRegOpnd, Js::Type::GetOffsetOfTypeId(), TyInt32, func), typeOfInstr);

    IR::LabelInstr * loadTypeDisplayStringLabel = IR::LabelInstr::New(Js::OpCode::Label, func);
    m_lowerer->InsertCompareBranch(objTypeIdOpnd, IR::IntConstOpnd::New(Js::TypeIds_Limit, TyUint32, func), Js::OpCode::BrGe_A, true /*unsigned*/, loadTypeDisplayStringLabel, typeOfInstr);
    
    m_lowerer->InsertMove(typeIdOpnd, objTypeIdOpnd, typeOfInstr);
    typeOfInstr->InsertBefore(loadTypeDisplayStringLabel);

    if (dst->IsEqual(src1))
    {
        ChangeToAssign(typeOfInstr->HoistSrc1(Js::OpCode::Ld_A));
    }
    m_lowerer->InsertMove(dst, IR::IndirOpnd::New(typeDisplayStringsArrayOpnd, typeIdOpnd, this->GetDefaultIndirScale(), TyMachPtr, func), typeOfInstr);
    m_lowerer->InsertTestBranch(dst, dst, Js::OpCode::BrEq_A, helperLabel, typeOfInstr);

    m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, typeOfInstr);

    // $taggedInt:
    //   MOV dst, typeDisplayStrings[TypeIds_Number]
    //   B $done
    typeOfInstr->InsertBefore(taggedIntLabel);
    m_lowerer->InsertMove(dst, IR::IndirOpnd::New(typeDisplayStringsArrayOpnd, Js::TypeIds_Number * sizeof(Js::Var), TyMachPtr, func), typeOfInstr);
    m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, typeOfInstr);

    // $falsy:
    //   MOV dst, "undefined"
    //   B $done
    typeOfInstr->InsertBefore(falsyLabel);
    IR::Opnd * undefinedDisplayStringOpnd = IR::IndirOpnd::New(typeDisplayStringsArrayOpnd, Js::TypeIds_Undefined, TyMachPtr, func);
    m_lowerer->InsertMove(dst, undefinedDisplayStringOpnd, typeOfInstr);
    m_lowerer->InsertBranch(Js::OpCode::Br, doneLabel, typeOfInstr);

    // $helper
    //   CALL OP_TypeOf
    // $done
    typeOfInstr->InsertBefore(helperLabel);
    typeOfInstr->InsertAfter(doneLabel);
    m_lowerer->LowerUnaryHelperMem(typeOfInstr, IR::HelperOp_Typeof);
}

void
LowererMD::InsertObjectPoison(IR::Opnd* poisonedOpnd, IR::BranchInstr* branchInstr, IR::Instr* insertInstr, bool isForStore)
{
    if ((isForStore && CONFIG_FLAG_RELEASE(PoisonObjectsForStores)) || (!isForStore && CONFIG_FLAG_RELEASE(PoisonObjectsForLoads)))
    {
        Js::OpCode opcode;
        if (branchInstr->m_opcode == Js::OpCode::BNE)
        {
            opcode = Js::OpCode::CSELEQ;
        }
        else
        {
            AssertOrFailFastMsg(branchInstr->m_opcode == Js::OpCode::BEQ, "Unexpected branch type in InsertObjectPoison preceeding instruction");
            opcode = Js::OpCode::CSELNE;
        }
        AssertOrFailFast(branchInstr->m_prev->m_opcode == Js::OpCode::SUBS || branchInstr->m_prev->m_opcode == Js::OpCode::ANDS);

        IR::RegOpnd* regZero = IR::RegOpnd::New(nullptr, RegZR, TyMachPtr, insertInstr->m_func);
        IR::Instr* csel = IR::Instr::New(opcode, poisonedOpnd, poisonedOpnd, regZero, insertInstr->m_func);
        insertInstr->InsertBefore(csel);
    }
}

IR::BranchInstr*
LowererMD::InsertMissingItemCompareBranch(IR::Opnd* compareSrc, IR::Opnd* missingItemOpnd, Js::OpCode opcode, IR::LabelInstr* target, IR::Instr* insertBeforeInstr)
{
    Assert(compareSrc->IsFloat64() && missingItemOpnd->IsUint64());

    IR::Opnd * compareSrcUint64Opnd = IR::RegOpnd::New(TyUint64, m_func);
    if (compareSrc->IsRegOpnd())
    {
        IR::Instr * movDoubleToUint64Instr = IR::Instr::New(Js::OpCode::FMOV_GEN, compareSrcUint64Opnd, compareSrc, insertBeforeInstr->m_func);
        insertBeforeInstr->InsertBefore(movDoubleToUint64Instr);
    }
    else if (compareSrc->IsIndirOpnd())
    {
        compareSrcUint64Opnd = compareSrc->UseWithNewType(TyUint64, m_func);
    }

    return m_lowerer->InsertCompareBranch(compareSrcUint64Opnd, missingItemOpnd, opcode, target, insertBeforeInstr);
}

#if DBG
//
// Helps in debugging of fast paths.
//
void LowererMD::GenerateDebugBreak( IR::Instr * insertInstr )
{
    IR::Instr *int3 = IR::Instr::New(Js::OpCode::DEBUGBREAK, insertInstr->m_func);
    insertInstr->InsertBefore(int3);
}
#endif

#ifdef _CONTROL_FLOW_GUARD
void
LowererMD::GenerateCFGCheck(IR::Opnd * entryPointOpnd, IR::Instr * insertBeforeInstr)
{
    bool useJITTrampoline = CONFIG_FLAG(UseJITTrampoline);
    IR::LabelInstr * callLabelInstr = nullptr;
    uintptr_t jitThunkStartAddress = NULL;

    if (useJITTrampoline)
    {
#if ENABLE_OOP_NATIVE_CODEGEN
        if (m_func->IsOOPJIT())
        {
            OOPJITThunkEmitter * jitThunkEmitter = m_func->GetOOPThreadContext()->GetJITThunkEmitter();
            jitThunkStartAddress = jitThunkEmitter->EnsureInitialized();
        }
        else
#endif
        {
            InProcJITThunkEmitter * jitThunkEmitter = m_func->GetInProcThreadContext()->GetJITThunkEmitter();
            jitThunkStartAddress = jitThunkEmitter->EnsureInitialized();
        }
        if (jitThunkStartAddress)
        {
            uintptr_t endAddressOfSegment = jitThunkStartAddress + InProcJITThunkEmitter::TotalThunkSize;
            Assert(endAddressOfSegment > jitThunkStartAddress);
            // Generate instructions for local Pre-Reserved Segment Range check

            IR::AddrOpnd * endAddressOfSegmentConstOpnd = IR::AddrOpnd::New(endAddressOfSegment, IR::AddrOpndKindDynamicMisc, m_func);
            IR::RegOpnd *resultOpnd = IR::RegOpnd::New(TyMachReg, this->m_func);

            callLabelInstr = IR::LabelInstr::New(Js::OpCode::Label, m_func);
            IR::LabelInstr * cfgLabelInstr = IR::LabelInstr::New(Js::OpCode::Label, m_func, true);

            // resultOpnd = SUB endAddressOfSegmentConstOpnd, entryPointOpnd
            // CMP resultOpnd, TotalThunkSize
            // BHS $cfgLabel
            // AND entryPointOpnd,  ~(ThunkSize-1)
            // JMP $callLabel
            m_lowerer->InsertSub(false, resultOpnd, endAddressOfSegmentConstOpnd, entryPointOpnd, insertBeforeInstr);
            m_lowerer->InsertCompareBranch(resultOpnd, IR::IntConstOpnd::New(InProcJITThunkEmitter::TotalThunkSize, TyMachReg, m_func, true), Js::OpCode::BrGe_A, true, cfgLabelInstr, insertBeforeInstr);
            m_lowerer->InsertAnd(entryPointOpnd, entryPointOpnd, IR::IntConstOpnd::New(InProcJITThunkEmitter::ThunkAlignmentMask, TyMachReg, m_func, true), insertBeforeInstr);
            m_lowerer->InsertBranch(Js::OpCode::Br, callLabelInstr, insertBeforeInstr);

            insertBeforeInstr->InsertBefore(cfgLabelInstr);
        }
    }
    //MOV  x15, entryPoint
    IR::RegOpnd * entryPointRegOpnd = IR::RegOpnd::New(nullptr, RegR15, TyMachReg, this->m_func);
    entryPointRegOpnd->m_isCallArg = true;
    IR::Instr *movInstrEntryPointToRegister = Lowerer::InsertMove(entryPointRegOpnd, entryPointOpnd, insertBeforeInstr);

    //Generate CheckCFG CALL here
    IR::HelperCallOpnd *cfgCallOpnd = IR::HelperCallOpnd::New(IR::HelperGuardCheckCall, this->m_func);
    IR::Instr* cfgCallInstr = IR::Instr::New(Js::OpCode::BLR, this->m_func);
    this->m_func->SetHasCallsOnSelfAndParents();

    //mov x16, __guard_check_icall_fptr
    IR::RegOpnd *targetOpnd = IR::RegOpnd::New(nullptr, RegR16, TyMachPtr, this->m_func);
    IR::Instr   *movInstr = Lowerer::InsertMove(targetOpnd, cfgCallOpnd, insertBeforeInstr);
    Legalize(movInstr);

    //call x16
    cfgCallInstr->SetSrc1(targetOpnd);

    //CALL cfg(x15)
    insertBeforeInstr->InsertBefore(cfgCallInstr);

    if (jitThunkStartAddress)
    {
        Assert(callLabelInstr);
        if (CONFIG_FLAG(ForceJITCFGCheck))
        {
            // Always generate CFG check to make sure that the address is still valid
            movInstrEntryPointToRegister->InsertBefore(callLabelInstr);
        }
        else
        {
            insertBeforeInstr->InsertBefore(callLabelInstr);
        }
    }
}
#endif
