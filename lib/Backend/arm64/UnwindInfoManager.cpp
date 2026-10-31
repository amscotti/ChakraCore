//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "Backend.h"
#include "ARM64UnwindEncoder.h"
#if defined(__APPLE__) && defined(_M_ARM64)
#include "DarwinEhFrame.h"
#endif

UnwindInfoManager::UnwindInfoManager()
{
    for (int which = UnwindInvalid; which < UnwindFunctionOffsetCount; which++)
    {
        this->m_functionOffsetLabelId[which] = 0;
        this->m_functionOffset[which] = 0xffffffff;
    }
#if defined(__APPLE__) && defined(_M_ARM64)
    this->m_ehFrameBytes = 0;
    this->m_cfaSpOffset = 0;
    this->m_savedCount = 0;
    memset(this->m_ehFrame, 0, sizeof(this->m_ehFrame));
#endif
}

void UnwindInfoManager::Init(Func * func)
{
    // necessary? Maybe not, but keeping around for now
}

DWORD UnwindInfoManager::GetPDataCount(DWORD length)
{
    // TODO: Support splitting huge functions if needed?
    return 1;
}

DWORD UnwindInfoManager::GetFunctionOffset(UnwindFunctionOffsets which)
{
    Assert(which < UnwindFunctionOffsetCount);
    DWORD result = this->m_functionOffset[which];
    Assert(result != 0xffffffff);
    Assert(result % 4 == 0);
    return result;
}

void UnwindInfoManager::FinalizeUnwindInfo(BYTE *functionStart, DWORD codeSize)
{
#if defined(__APPLE__) && defined(_M_ARM64)
    // Windows .xdata is not an FDE. libunwind would read past the allocation.
    DarwinArm64SavedReg saved[kMaxDarwinSaves];
    uint32 count = this->m_savedCount;
    if (count > kMaxDarwinSaves)
    {
        count = kMaxDarwinSaves;
    }
    for (uint32 i = 0; i < count; i++)
    {
        saved[i].dwarfReg = this->m_savedDwarf[i];
        saved[i].offsetFromSp = this->m_savedOffset[i];
    }
    size_t bytes = EmitDarwinArm64EhFrame(
        this->m_ehFrame,
        sizeof(this->m_ehFrame),
        functionStart,
        codeSize,
        this->GetFunctionOffset(UnwindPrologEnd),
        this->m_cfaSpOffset,
        saved,
        count);
    this->m_ehFrameBytes = (bytes == 0) ? 0 : (DWORD)bytes;
#else
    // fetch the appropriate offsets and hand off to the generic code
    m_xdata.Generate(PULONG(functionStart + this->GetFunctionOffset(UnwindPrologStart)), 
                     PULONG(functionStart + this->GetFunctionOffset(UnwindPrologEnd)), 
                     PULONG(functionStart + this->GetFunctionOffset(UnwindEpilogStart)), 
                     PULONG(functionStart + this->GetFunctionOffset(UnwindEpilogEnd)),
                     PULONG(functionStart + codeSize));
#endif
}

#if defined(__APPLE__) && defined(_M_ARM64)
void UnwindInfoManager::ResetDarwinFrame(uint32 cfaSpOffset)
{
    this->m_cfaSpOffset = cfaSpOffset;
    this->m_savedCount = 0;
}

void UnwindInfoManager::AddDarwinSave(uint16 dwarfReg, uint32 offsetFromSp)
{
    if (this->m_savedCount >= kMaxDarwinSaves)
    {
        return;
    }
    uint32 index = this->m_savedCount++;
    this->m_savedDwarf[index] = dwarfReg;
    this->m_savedOffset[index] = offsetFromSp;
}
#endif

void UnwindInfoManager::SetFunctionOffsetLabel(UnwindFunctionOffsets which, IR::LabelInstr *label)
{
    Assert(which < UnwindFunctionOffsetCount);
    Assert(this->m_functionOffsetLabelId[which] == 0);
    this->m_functionOffsetLabelId[which] = label->m_id;
}

#if defined(__APPLE__) && defined(_M_ARM64)
namespace
{
class EhWriter
{
public:
    EhWriter(BYTE* buffer, size_t capacity)
        : p(buffer), end(buffer + capacity), ok(buffer != nullptr && capacity > 0)
    {
    }

    bool Ok() const { return this->ok; }
    BYTE* Pos() const { return this->p; }

    void Byte(BYTE value)
    {
        if (!this->ok || this->p >= this->end)
        {
            this->ok = false;
            return;
        }
        *this->p++ = value;
    }

    void U32(uint32 value)
    {
        for (int i = 0; i < 4; i++)
        {
            this->Byte((BYTE)(value & 0xff));
            value >>= 8;
        }
    }

    void U64(uint64 value)
    {
        for (int i = 0; i < 8; i++)
        {
            this->Byte((BYTE)(value & 0xff));
            value >>= 8;
        }
    }

    void Uleb(uint32 value)
    {
        do
        {
            BYTE b = (BYTE)(value & 0x7f);
            value >>= 7;
            if (value)
            {
                b |= 0x80;
            }
            this->Byte(b);
        } while (value != 0 && this->ok);
    }

    void Pad(BYTE* recordStart)
    {
        while (this->ok && ((this->p - recordStart) & 7) != 0)
        {
            this->Byte(0);
        }
    }

private:
    BYTE* p;
    BYTE* end;
    bool ok;
};
}

size_t EmitDarwinArm64EhFrame(
    BYTE* buffer,
    size_t capacity,
    const void* pcBegin,
    uint32 pcRange,
    uint32 advance,
    uint32 cfaSpOffset,
    const DarwinArm64SavedReg* saved,
    uint32 savedCount)
{
    if (buffer == nullptr || capacity < 16)
    {
        return 0;
    }
    memset(buffer, 0, capacity);
    EhWriter w(buffer, capacity);

    BYTE* cie = w.Pos();
    w.U32(0);
    w.U32(0);
    w.Byte(1);
    w.Byte(0);
    w.Byte(1);
    w.Byte(0x78);
    w.Byte(30);
    w.Byte(0x0c);
    w.Uleb(31);
    w.Uleb(0);
    w.Pad(cie);
    if (!w.Ok())
    {
        return 0;
    }
    uint32 cieLength = (uint32)(w.Pos() - cie - 4);
    memcpy(cie, &cieLength, sizeof(cieLength));

    BYTE* fde = w.Pos();
    w.U32(0);
    BYTE* ciePointer = w.Pos();
    w.U32((uint32)(ciePointer - buffer));
    w.U64((uint64)pcBegin);
    w.U64(pcRange);
    if (advance != 0)
    {
        if (advance <= 0x3f)
        {
            w.Byte((BYTE)(0x40 | advance));
        }
        else if (advance <= 0xff)
        {
            w.Byte(0x02);
            w.Byte((BYTE)advance);
        }
        else if (advance <= 0xffff)
        {
            w.Byte(0x03);
            w.Byte((BYTE)(advance & 0xff));
            w.Byte((BYTE)((advance >> 8) & 0xff));
        }
        else
        {
            w.Byte(0x04);
            w.U32(advance);
        }
    }
    if (cfaSpOffset != 0)
    {
        w.Byte(0x0c);
        w.Uleb(31);
        w.Uleb(cfaSpOffset);
    }
    for (uint32 i = 0; i < savedCount; i++)
    {
        uint32 where = saved[i].offsetFromSp;
        if (where > cfaSpOffset || ((cfaSpOffset - where) & 7) != 0)
        {
            continue;
        }
        uint32 factored = (cfaSpOffset - where) / 8;
        uint16 reg = saved[i].dwarfReg;
        if (reg < 64)
        {
            w.Byte((BYTE)(0x80 | reg));
            w.Uleb(factored);
        }
        else
        {
            w.Byte(0x05);
            w.Uleb(reg);
            w.Uleb(factored);
        }
    }
    w.Pad(fde);
    if (!w.Ok())
    {
        return 0;
    }
    uint32 fdeLength = (uint32)(w.Pos() - fde - 4);
    memcpy(fde, &fdeLength, sizeof(fdeLength));
    w.U32(0);
    if (!w.Ok())
    {
        return 0;
    }
    return (size_t)(w.Pos() - buffer);
}
#endif

void UnwindInfoManager::SetLabelOffset(DWORD id, DWORD offset)
{
    Assert(offset % 4 == 0);
    for (int which = UnwindInvalid + 1; which < UnwindFunctionOffsetCount; which++)
    {
        if (this->m_functionOffsetLabelId[which] == id)
        {
            Assert(this->m_functionOffset[which] == 0xffffffff);
            this->m_functionOffset[which] = offset;
        }
    }
}
