;-------------------------------------------------------------------------------------------------------
; Copyright (C) Microsoft. All rights reserved.
; Copyright (c) ChakraCore Project Contributors. All rights reserved.
; Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
;-------------------------------------------------------------------------------------------------------

;Var arm64_CallFunction(JavascriptFunction* function, CallInfo info, uint argCount, Var* values, JavascriptMethod entryPoint)
;
;   This method should be called as follows
;       varResult = arm64_CallFunction((JavascriptFunction*)function, args.Info, argCount, args.Values, entryPoint);
;
;   and makes the following call
;           return entryPoint(function, info, values[0], values[1], ..., values[n-1]);
;   where n = info.Count
;
;   macOS passes variadic arguments on the stack. JIT callees also read the first
;   values from x2-x7 and home them. Function and callInfo stay in x0 and x1 and
;   at [sp], [sp+8] for the stack walker and for JavascriptMethod.
;

#include "unixasmmacros.inc"

NESTED_ENTRY arm64_CallFunction, _TEXT, NoHandler

    PROLOG_SAVE_REG_PAIR_INDEXED fp, lr, -16    ; save FP/LR registers, implicitly stores FP in SP

    mov     x16, x4                             ; entry point; x4 is reused as an argument
    mov     x15, x2                             ; argCount, kept across the copy

    add     x5, x2, #3                          ; add 3 to param count
    lsr     x5, x5, #1                          ; divide by 2 
    sub     sp, sp, x5, lsl #4                  ; then allocate the space
    mov     x6, sp                              ; use x6 = dest
    str     x0, [x6], 8                         ; store function pointer
    str     x1, [x6], 8                         ; store info pointer

    cmp     x2, #0                              ; check for 0 params
    beq     LOCAL_LABEL(CallEntryPoint)

    LOCAL_LABEL(CopyLoop):
    subs    x2, x2, #1                          ; decrement param count by 1
    ldr     x7, [x3], #8                        ; read param from source
    str     x7, [x6], #8                        ; store param to dest
    bne     LOCAL_LABEL(CopyLoop)               ; loop until all copied

    ; values[0] is slot 2. Load only the slots the copy wrote.
    cmp     x15, #1
    blo     LOCAL_LABEL(CallEntryPoint)
    ldr     x2, [sp, #16]
    beq     LOCAL_LABEL(CallEntryPoint)
    ldr     x3, [sp, #24]
    cmp     x15, #2
    beq     LOCAL_LABEL(CallEntryPoint)
    ldr     x4, [sp, #32]
    cmp     x15, #3
    beq     LOCAL_LABEL(CallEntryPoint)
    ldr     x5, [sp, #40]
    cmp     x15, #4
    beq     LOCAL_LABEL(CallEntryPoint)
    ldr     x6, [sp, #48]
    cmp     x15, #5
    beq     LOCAL_LABEL(CallEntryPoint)
    ldr     x7, [sp, #56]

    LOCAL_LABEL(CallEntryPoint):
    blr     x16                                 ; call entry point

    mov     sp, fp                              ; explicitly restore sp
    EPILOG_RESTORE_REG_PAIR_INDEXED fp, lr, 16  ; restore FP/LR
    ret                                         ; return

NESTED_END arm64_CallFunction

NESTED_ENTRY BreakSpeculation, _TEXT, NoHandler
    cmp x0, x0
    csel x0, x0, x0, eq
    ret
NESTED_END BreakSpeculation
