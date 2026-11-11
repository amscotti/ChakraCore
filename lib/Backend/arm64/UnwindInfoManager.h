//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "ARM64UnwindEncoder.h"

enum UnwindFunctionOffsets
{
    UnwindInvalid,
    UnwindPrologStart,
    UnwindPrologEnd,
    UnwindEpilogStart,
    UnwindEpilogEnd,
    UnwindFunctionOffsetCount
};

class UnwindInfoManager
{
public:
    UnwindInfoManager();

    void Init(Func * func);
    DWORD GetPDataCount(DWORD length);
#if defined(__APPLE__) && defined(_M_ARM64)
    // Mach-O registration reads this buffer as .eh_frame. The slot is the
    // existing xdata allocation (XDATA_SIZE bytes).
    DWORD SizeOfUnwindInfo() const { return m_ehFrameBytes; }
    BYTE *GetUnwindInfo() { return m_ehFrame; }
    void ResetDarwinFrame(uint32 cfaSpOffset);
    void AddDarwinSave(uint16 dwarfReg, uint32 offsetFromSp);
#else
    DWORD SizeOfUnwindInfo() const { return m_xdata.GetXdataBytes(); }
    BYTE *GetUnwindInfo() { return reinterpret_cast<BYTE *>(const_cast<void *>(m_xdata.GetXdata())); }
#endif
    void FinalizeUnwindInfo(BYTE *functionStart, DWORD codeSize);

    // label management
    void SetFunctionOffsetLabel(UnwindFunctionOffsets which, IR::LabelInstr *label);
    void SetLabelOffset(DWORD id, DWORD offset);

private:
    DWORD GetFunctionOffset(UnwindFunctionOffsets which);

    DWORD m_functionOffsetLabelId[UnwindFunctionOffsetCount];
    DWORD m_functionOffset[UnwindFunctionOffsetCount];

    Arm64XdataGenerator m_xdata;
#if defined(__APPLE__) && defined(_M_ARM64)
    // CFA is SP + m_cfaSpOffset once the prolog has finished. Saves are byte
    // offsets from that SP. The try thunk builds this same shape and then
    // points FP at the parent frame, so the FDE is SP-relative.
    static const uint32 kMaxDarwinSaves = 24;
    BYTE m_ehFrame[0x100];
    DWORD m_ehFrameBytes;
    uint32 m_cfaSpOffset;
    uint32 m_savedCount;
    uint16 m_savedDwarf[kMaxDarwinSaves];
    uint32 m_savedOffset[kMaxDarwinSaves];
#endif
};
