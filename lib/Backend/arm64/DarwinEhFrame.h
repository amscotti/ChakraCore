//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

// One CIE and one FDE, absolute PC addresses, no augmentation.
// Apple __register_frame accepts this encoding when each FDE is registered
// on its own (see mac_fde_wrapper). The caller owns `buffer`. This writer
// does not allocate.
//
// CFA is SP + cfaSpOffset after `advance` bytes. Each save is the byte
// offset of that register from the final SP. DWARF data alignment is -8.

struct DarwinArm64SavedReg
{
    uint16 dwarfReg;
    uint32 offsetFromSp;
};

size_t EmitDarwinArm64EhFrame(
    BYTE* buffer,
    size_t capacity,
    const void* pcBegin,
    uint32 pcRange,
    uint32 advance,
    uint32 cfaSpOffset,
    const DarwinArm64SavedReg* saved,
    uint32 savedCount);
