;-------------------------------------------------------------------------------------------------------
; Copyright (C) Microsoft. All rights reserved.
; Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
;-------------------------------------------------------------------------------------------------------

    OPT 2 ; disable listing
#include "ksarm64.h"
    OPT 1 ; re-enable listing

    TTL Lib\Backend\arm64\LinearScanMdA.asm

    EXPORT |?SaveAllRegistersAndBailOut@LinearScanMD@@SAXQEAVBailOutRecord@@@Z|
    EXPORT |?SaveAllRegistersAndBranchBailOut@LinearScanMD@@SAXQEAVBranchBailOutRecord@@H@Z|

    ; BailOutRecord::BailOut(BailOutRecord const * bailOutRecord)
    IMPORT |?BailOut@BailOutRecord@@SAPEAXPEBV1@@Z|

    ; BranchBailOutRecord::BailOut(BranchBailOutRecord const * bailOutRecord, BOOL cond)
    IMPORT |?BailOut@BranchBailOutRecord@@SAPEAXPEBV1@H@Z|

    TEXTAREA

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; LinearScanMD::SaveAllRegistersAndBailOut(BailOutRecord *const bailOutRecord)

    NESTED_ENTRY ?SaveAllRegistersAndBailOut@LinearScanMD@@SAXQEAVBailOutRecord@@@Z

    ; x0 == bailOutRecord
    ; lr == return address

    ; Save all registers except the above, which would have already been saved by jitted code if necessary
    ldr x17, [x0] ; bailOutRecord->globalBailOutRecordDataTable
    ldr x17, [x17] ; bailOutRecord->globalBailOutRecordDataTable->registerSaveSpace
    str x1, [x17, #1*8]
    stp x2, x3, [x17, #2*8]
    stp x4, x5, [x17, #4*8]
    stp x6, x7, [x17, #6*8]
    stp x8, x9, [x17, #8*8]
    stp x10, x11, [x17, #10*8]
    stp x12, x13, [x17, #12*8]
    stp x14, x15, [x17, #14*8]
    str x16, [x17, #16*8]
    ; skip x17/x18
    stp x19, x20, [x17, #19*8]
    stp x21, x22, [x17, #21*8]
    stp x23, x24, [x17, #23*8]
    stp x25, x26, [x17, #25*8]
    stp x27, x28, [x17, #27*8]
    str fp, [x17, #29*8]
    ; skip lr, sp, zr
    ; W8-NEON slice 1: Q regs are 16 bytes (stride 2 vars), matching
    ; LinearScanMD::GetRegisterSaveIndex. Qn == Dn hardware, wider store.
    add x17, x17, #33*8
    stp q0, q1, [x17, #0*16]
    stp q2, q3, [x17, #2*16]
    stp q4, q5, [x17, #4*16]
    stp q6, q7, [x17, #6*16]
    stp q8, q9, [x17, #8*16]
    stp q10, q11, [x17, #10*16]
    stp q12, q13, [x17, #12*16]
    stp q14, q15, [x17, #14*16]
    stp q16, q17, [x17, #16*16]
    stp q18, q19, [x17, #18*16]
    stp q20, q21, [x17, #20*16]
    stp q22, q23, [x17, #22*16]
    stp q24, q25, [x17, #24*16]
    stp q26, q27, [x17, #26*16]
    stp q28, q29, [x17, #28*16]
    ;stp q30, q31, [x17, #30*16]

    b |?BailOut@BailOutRecord@@SAPEAXPEBV1@@Z|

    NESTED_END

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; LinearScanMD::SaveAllRegistersAndBranchBailOut(BranchBailOutRecord *const bailOutRecord, const BOOL condition)

    NESTED_ENTRY ?SaveAllRegistersAndBranchBailOut@LinearScanMD@@SAXQEAVBranchBailOutRecord@@H@Z

    ; x0 == bailOutRecord
    ; x1 == condition
    ; lr == return address

    ; Save all registers except the above, which would have already been saved by jitted code if necessary
    ldr x17, [x0] ; bailOutRecord->globalBailOutRecordDataTable
    ldr x17, [x17] ; bailOutRecord->globalBailOutRecordDataTable->registerSaveSpace
    stp x2, x3, [x17, #2*8]
    stp x4, x5, [x17, #4*8]
    stp x6, x7, [x17, #6*8]
    stp x8, x9, [x17, #8*8]
    stp x10, x11, [x17, #10*8]
    stp x12, x13, [x17, #12*8]
    stp x14, x15, [x17, #14*8]
    str x16, [x17, #16*8]
    ; skip x17/x18
    stp x19, x20, [x17, #19*8]
    stp x21, x22, [x17, #21*8]
    stp x23, x24, [x17, #23*8]
    stp x25, x26, [x17, #25*8]
    stp x27, x28, [x17, #27*8]
    str fp, [x17, #29*8]
    ; skip lr, sp, zr
    ; W8-NEON slice 1: Q regs are 16 bytes (stride 2 vars), matching
    ; LinearScanMD::GetRegisterSaveIndex. Qn == Dn hardware, wider store.
    add x17, x17, #33*8
    stp q0, q1, [x17, #0*16]
    stp q2, q3, [x17, #2*16]
    stp q4, q5, [x17, #4*16]
    stp q6, q7, [x17, #6*16]
    stp q8, q9, [x17, #8*16]
    stp q10, q11, [x17, #10*16]
    stp q12, q13, [x17, #12*16]
    stp q14, q15, [x17, #14*16]
    stp q16, q17, [x17, #16*16]
    stp q18, q19, [x17, #18*16]
    stp q20, q21, [x17, #20*16]
    stp q22, q23, [x17, #22*16]
    stp q24, q25, [x17, #24*16]
    stp q26, q27, [x17, #26*16]
    stp q28, q29, [x17, #28*16]
    ;stp q30, q31, [x17, #30*16]

    b |?BailOut@BranchBailOutRecord@@SAPEAXPEBV1@H@Z|

    NESTED_END

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    END
