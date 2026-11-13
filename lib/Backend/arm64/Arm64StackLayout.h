//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

// Canonical ARM64 JIT frame layout shared by the prolog/epilog emitter
// (LowerMD::LowerEntryInstr) and the encoder's incoming-stack-arg path.
// Moved here from LowerMD.cpp so the encoder can reconstruct the exact
// frame (notably the homed-SIMD area) instead of assuming a fixed shape.
//
// Requires Backend.h (Func, BitVector, MachRegInt/MachRegDouble/MachRegSimd128,
// ULONG) to be included first.

class Func;

class ARM64StackLayout
{
    //
    // Canonical ARM64 prolog/epilog stack layout (stack grows downward):
    //
    //    +-------------------------------------+
    //    |     caller-allocated parameters     |
    //    +=====================================+-----> SP at time of call
    //    |   callee-saved parameters (x0-x7)   |
    //    +-------------------------------------+
    //    |    frame pointer + link register    |
    //    +-------------------------------------+-----> updated FP points here
    //    |  arguments slot + StackFunctionList |
    //    +-------------------------------------+
    //    |   callee-saved registers (x19-x28)  |
    //    +-------------------------------------+
    //    |    callee-saved FP regs (d8-d15)    |
    //    +-------------------------------------+-----> == regOffset
    //    |            locals area              |
    //    +-------------------------------------+-----> locals pointer if not SP
    //    |     caller-allocated parameters     |
    //    +=====================================+-----> SP points here when done
    //

public:
    ARM64StackLayout(Func* func);

    // Getters
    bool HasCalls() const { return m_hasCalls; }
    bool HasTry() const { return m_hasTry; }
    ULONG ArgSlotCount() const { return m_argSlotCount; }
    BitVector HomedParams() const { return m_homedParams; }
    BitVector HomedSimd() const { return m_homedSimd; }
    BitVector SavedRegisters() const { return m_savedRegisters; }
    BitVector SavedDoubles() const { return m_savedDoubles; }
    BitVector SavedSimd() const { return m_savedSimd; }

    // Locals area sits right after space allocated for arguments
    ULONG LocalsOffset() const { return this->m_argSlotCount * MachRegInt; }
    ULONG LocalsSize() const { return this->m_localsArea; }

    // Saved non-volatile double registers sit past the locals area
    ULONG SavedDoublesOffset() const { return this->LocalsOffset() + this->LocalsSize(); }
    ULONG SavedDoublesSize() const { return this->m_savedDoubles.Count() * MachRegDouble; }

    // Saved 128-bit SIMD registers (Q8-15, count*16) sit after scalar doubles.
    // Q8-15 alias D8-15 (same hardware, Reg.h); a SIMD function moves its
    // callee-saved vector saves here so they are stored/restored at full
    // 16-byte width. Non-SIMD functions have an empty area (no layout change).
    ULONG SavedSimdOffset() const { return this->SavedDoublesOffset() + this->SavedDoublesSize(); }
    ULONG SavedSimdSize() const { return this->m_savedSimd.Count() * MachRegSimd128; }

    // Saved non-volatile integer registers sit after the saved doubles+simd
    ULONG SavedRegistersOffset() const { return this->SavedSimdOffset() + this->SavedSimdSize(); }
    ULONG SavedRegistersSize() const { return this->m_savedRegisters.Count() * MachRegInt; }

    // The argument slot and StackFunctionList entry come after the saved integer registers
    ULONG ArgSlotOffset() const { return this->SavedRegistersOffset() + this->SavedRegistersSize(); }
    ULONG ArgSlotSize() const { return this->m_hasCalls ? (2 * MachRegInt) : 0; }

    // Next comes the frame chain
    ULONG FpLrOffset() const { return this->ArgSlotOffset() + this->ArgSlotSize(); }
    ULONG FpLrSize() const { return this->m_hasCalls ? (2 * MachRegInt) : 0; }

    // Followed by any homed parameters
    ULONG HomedParamsOffset() const { return this->FpLrOffset() + this->FpLrSize(); }
    ULONG HomedParamsSize() const { return this->m_homedParams.Count() * MachRegInt; }

    // W8-NEON slice 1: homed 128-bit params (Q0-7, count*16) after int homes.
    ULONG HomedSimdOffset() const { return this->HomedParamsOffset() + this->HomedParamsSize(); }
    ULONG HomedSimdSize() const { return this->m_homedSimd.Count() * MachRegSimd128; }

    // And that's the total stack allocation
    ULONG TotalStackSize() const { return this->HomedSimdOffset() + this->HomedSimdSize(); }

    // The register area is the area at the far end that doesn't include locals or arg slots
    ULONG RegisterAreaOffset() const { return this->SavedDoublesOffset(); }
    ULONG RegisterAreaSize() const { return this->TotalStackSize() - this->RegisterAreaOffset(); }

private:
    bool m_hasCalls;
    bool m_hasTry;
    ULONG m_argSlotCount;
    ULONG m_localsArea;
    BitVector m_homedParams;
    BitVector m_homedSimd;
    BitVector m_savedRegisters;
    BitVector m_savedDoubles;
    BitVector m_savedSimd;
};
