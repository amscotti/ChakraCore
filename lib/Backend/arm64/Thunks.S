;-------------------------------------------------------------------------------------------------------
; Copyright (C) Microsoft. All rights reserved.
; Copyright (c) ChakraCore Project Contributors. All rights reserved.
; Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
;-------------------------------------------------------------------------------------------------------

#include "unixasmmacros.inc"

;;============================================================================================================
; Fake __chkstk
;;============================================================================================================
LEAF_ENTRY __chkstk, _TEXT
        ret
LEAF_END __chkstk, _TEXT

;;============================================================================================================
; NativeCodeGenerator::CheckCodeGenThunk
;;============================================================================================================
    ;Js::Var NativeCodeGenerator::CheckCodeGenThunk(Js::RecyclableObject* function, Js::CallInfo callInfo, ...)
NESTED_ENTRY _ZN19NativeCodeGenerator17CheckCodeGenThunkEPN2Js16RecyclableObjectENS0_8CallInfoEz, _TEXT, NoHandler

    PROLOG_SAVE_REG_PAIR_INDEXED  fp, lr, -(16+16*8)   ; establish stack frame
    stp     d0, d1, [sp, #16+0*8]
    stp     d2, d3, [sp, #16+2*8]
    stp     d4, d5, [sp, #16+4*8]
    stp     d6, d7, [sp, #16+6*8]
    stp     x0, x1, [sp, #16+8*8]
    stp     x2, x3, [sp, #16+10*8]
    stp     x4, x5, [sp, #16+12*8]
    stp     x6, x7, [sp, #16+14*8]

    bl      C_FUNC(_ZN19NativeCodeGenerator12CheckCodeGenEPN2Js14ScriptFunctionE)  ; call  NativeCodeGenerator::CheckCodeGen
    mov     x15, x0               ; move entry point to x15

    ldp     d0, d1, [sp, #16+0*8]
    ldp     d2, d3, [sp, #16+2*8]
    ldp     d4, d5, [sp, #16+4*8]
    ldp     d6, d7, [sp, #16+6*8]
    ldp     x0, x1, [sp, #16+8*8]
    ldp     x2, x3, [sp, #16+10*8]
    ldp     x4, x5, [sp, #16+12*8]
    ldp     x6, x7, [sp, #16+14*8]
    EPILOG_RESTORE_REG_PAIR_INDEXED  fp, lr, (16+16*8)
    br      x15             ; jump (tail call) to new entryPoint

NESTED_END _ZN19NativeCodeGenerator17CheckCodeGenThunkEPN2Js16RecyclableObjectENS0_8CallInfoEz, _TEXT

;;============================================================================================================
; NativeCodeGenerator::CheckAsmJsCodeGenThunk (W4, mirrors x64 CheckAsmJsCodeGenThunk)
;; Saves/restores the JS arg registers across the CheckAsmJsCodeGen call so the
;; incoming slot image (x0-x7 homed by the callee) is intact at the JIT target.
;;============================================================================================================
    ;Js::Var NativeCodeGenerator::CheckAsmJsCodeGenThunk(Js::RecyclableObject* function, Js::CallInfo callInfo, ...)
NESTED_ENTRY _ZN19NativeCodeGenerator22CheckAsmJsCodeGenThunkEPN2Js16RecyclableObjectENS0_8CallInfoEz, _TEXT, NoHandler

    PROLOG_SAVE_REG_PAIR_INDEXED  fp, lr, -(16+16*8)   ; establish stack frame
    stp     d0, d1, [sp, #16+0*8]
    stp     d2, d3, [sp, #16+2*8]
    stp     d4, d5, [sp, #16+4*8]
    stp     d6, d7, [sp, #16+6*8]
    stp     x0, x1, [sp, #16+8*8]
    stp     x2, x3, [sp, #16+10*8]
    stp     x4, x5, [sp, #16+12*8]
    stp     x6, x7, [sp, #16+14*8]

    bl      C_FUNC(_ZN19NativeCodeGenerator17CheckAsmJsCodeGenEPN2Js14ScriptFunctionE)  ; call NativeCodeGenerator::CheckAsmJsCodeGen
    mov     x15, x0               ; move entry point to x15

    ldp     d0, d1, [sp, #16+0*8]
    ldp     d2, d3, [sp, #16+2*8]
    ldp     d4, d5, [sp, #16+4*8]
    ldp     d6, d7, [sp, #16+6*8]
    ldp     x0, x1, [sp, #16+8*8]
    ldp     x2, x3, [sp, #16+10*8]
    ldp     x4, x5, [sp, #16+12*8]
    ldp     x6, x7, [sp, #16+14*8]
    EPILOG_RESTORE_REG_PAIR_INDEXED  fp, lr, (16+16*8)
    br      x15             ; jump (tail call) to new entryPoint

NESTED_END _ZN19NativeCodeGenerator22CheckAsmJsCodeGenThunkEPN2Js16RecyclableObjectENS0_8CallInfoEz, _TEXT

;;============================================================================================================
; AsmJsJitCall: bridge from C++ (AsmJsExternalEntryPoint / CallAsmJsFunction) to
; JIT-compiled wasm/asm.js code. The JIT body is called as if by JIT code
; (doc/wasm-arm64-abi.md section 3, AAPCS64): the args slot image at layout is
; [func][gap][gap][args...] (wasm param k at arg slot 3+k). x0-x7 are loaded
; from [layout + 8*slot]; overflow args (slots >= 8) are read from
; [sp + 8*slot] (the caller out-area), so the callee sp is layout itself.
; The JIT prologue (LowerEntryInstr) then sub sp,sp,#F for its frame, which
; descends BELOW layout; the caller reserves free space below the image
; so the frame never reaches the bridge frame saved below. Writes x0/d0 to outRet.
;   x0 = entrypoint (JIT code only; C-function entrypoints are excluded by
;        the callers), x1 = layout (16-aligned args slot image),
;        x2 = outRet (16 bytes: x0 at +0, d0 at +8)
;;============================================================================================================
NESTED_ENTRY AsmJsJitCall, _TEXT, NoHandler
    PROLOG_SAVE_REG_PAIR_INDEXED  fp, lr, -(16+16*8)   ; establish stack frame
    stp     x19, x20, [sp, #16+0*8]
    stp     x21, x22, [sp, #16+2*8]
    mov     x9, x0              ; entrypoint (saved before ldp clobbers x0)
    mov     x19, x1             ; layout (16-aligned)
    mov     x20, x2             ; outRet (16 bytes: x0 at +0, d0 at +8)
    ldp     x0, x1, [x19, #0]
    ldp     x2, x3, [x19, #16]
    ldp     x4, x5, [x19, #32]
    ldp     x6, x7, [x19, #48]
    mov     sp, x19             ; callee sp = image base: arg slot s at [sp + 8*s]
    blr     x9                  ; call JIT entry (LR = return here)
    str     x0, [x20, #0]       ; int return (x0)
    str     d0, [x20, #8]       ; float return (d0)
    ldr     x19, [fp, #16+0*8]
    ldr     x20, [fp, #16+1*8]
    ldr     x21, [fp, #16+2*8]
    mov     sp, fp              ; back to bridge frame
    EPILOG_RESTORE_REG_PAIR_INDEXED  fp, lr, (16+16*8)
    ret
NESTED_END AsmJsJitCall, _TEXT
