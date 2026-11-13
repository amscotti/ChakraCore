//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#define MD_GROW_LOCALS_AREA_UP

// Don't encode large user constants, because they won't emit them as 32-bit numbers
// in the instruction stream.
#undef MD_ENCODE_LG_CONSTS
#define MD_ENCODE_LG_CONSTS false

const int PAGESIZE = 0x1000;

const IRType TyMachReg = TyInt64;
const IRType TyMachPtr = TyUint64;
const IRType TyMachDouble = TyFloat64;

// W4: IRBuilderAsmJs compiles under ENABLE_ASMJS on ARM64 but wasm SIMD stays
// x86-only (W8), so no SIMD bytecode is produced and these aliases are dead.
// Defined so the SIMD builder code type-checks (mirrors amd64/i386 md.h).
const IRType TyMachSimd128F4 = TySimd128F4;
const IRType TyMachSimd128I4 = TySimd128I4;
const IRType TyMachSimd128I8 = TySimd128I8;
const IRType TyMachSimd128I16 = TySimd128I16;
const IRType TyMachSimd128U4 = TySimd128U4;
const IRType TyMachSimd128U8 = TySimd128U8;
const IRType TyMachSimd128U16 = TySimd128U16;
const IRType TyMachSimd128B4 = TySimd128B4;
const IRType TyMachSimd128B8 = TySimd128B8;
const IRType TyMachSimd128B16 = TySimd128B16;
const IRType TyMachSimd128D2 = TySimd128D2;
const IRType TyMachSimd128I2 = TySimd128I2;

const DWORD EMIT_BUFFER_ALIGNMENT = 16;
const DWORD INSTR_ALIGNMENT = 2;

#ifdef INSERT_NOPS
const int CountNops = 10;
const int MachMaxInstrSize = (2 * CountNops + 1)*4;
#else
const int MachMaxInstrSize = 4;
#endif

#define SOFTWARE_FIXFOR_HARDWARE_BUGWIN8_502326
