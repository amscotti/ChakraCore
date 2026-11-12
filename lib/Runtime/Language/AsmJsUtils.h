//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// Portions of this file are copyright 2014 Mozilla Foundation, available under the Apache 2.0 license.
//-------------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------------
// Copyright 2014 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http ://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-------------------------------------------------------------------------------------------------------

#pragma once

#ifdef ENABLE_ASMJS
// Removed code from original location, if the expression is true, check if extra code needed
#define MaybeTodo( expr ) AssertMsg( !(expr), "Unhandled scenario in asm.js" )

#ifdef ENABLE_WASM
namespace Wasm
{
    class WasmSignature;
}
#endif

namespace Js {

#pragma warning (suppress: 25057) // Suppress unannotated buffer warning
    void * UnboxAsmJsArguments(ScriptFunction* func, Var * origArgs, char * argDst, CallInfo callInfo);
    // W4 slice 1: resolve the internal-call entrypoint (EntryPointInfo
    // jsMethod) from JIT code via the helper machinery.
    JavascriptMethod AsmJsGetEntryPoint(ScriptFunction* func);
#if _M_X64
    Var BoxAsmJsReturnValue(ScriptFunction* func, int64 intRetVal, double doubleRetVal, float floatRetVal, __m128 simdReturn);
    int GetStackSizeForAsmJsUnboxing(ScriptFunction* func);
#elif defined(_M_ARM64)
    // ARM64 (doc/wasm-arm64-abi.md sections 2-3,6): no __m128 on ARM64.
    // SIMD box takes AsmJsSIMDValue; SIMD returns fail controlled (W8).
    Var BoxAsmJsReturnValue(ScriptFunction* func, int64 intRetVal, double doubleRetVal, float floatRetVal, const AsmJsSIMDValue& simdReturn);
    int GetStackSizeForAsmJsUnboxing(ScriptFunction* func);
#endif

#if defined(_M_X64) || defined(_M_ARM64)
#ifdef ENABLE_WASM
    // W7 multi-value: box N raw 8B result bits per WasmSignature types into
    // a JS Array (element k from values[k]); i64/v128 fail with TypeError
    // exactly like BoxAsmJsReturnValue unless WasmI64 objects are enabled.
    Var BoxWasmMultiResult(ScriptFunction* func, const Wasm::WasmSignature* sig, const uint64* values);
#endif
#endif

    class AsmJsCompilationException
    {
        char16 msg_[256];
    public:
        AsmJsCompilationException( const char16* _msg, ... );
        inline char16* msg() { return msg_; }
    };

    class ParserWrapper
    {
    public:
        static PropertyName FunctionName( ParseNode *node );
        static PropertyName VariableName( ParseNode *node );
        static ParseNode* FunctionArgsList( ParseNode *node, ArgSlot &numformals );
        static ParseNode* NextVar( ParseNode *node );
        static ParseNode* NextInList( ParseNode *node );
        static inline ParseNode *GetListHead( ParseNode *node );
        static inline bool IsNameDeclaration(ParseNode *node);
        static inline bool IsUInt(ParseNode *node);
        static inline uint GetUInt(ParseNode *node);
        static inline bool IsNegativeZero(ParseNode* node);
        static inline bool IsMinInt(ParseNode *node){ return node && node->nop == knopFlt && node->AsParseNodeFloat()->maybeInt && node->AsParseNodeFloat()->dbl == -2147483648.0; };
        static inline bool IsUnsigned(ParseNode *node)
        {
            return node &&
                node->nop == knopFlt &&
                node->AsParseNodeFloat()->maybeInt &&
                node->AsParseNodeFloat()->dbl > (double)INT_MAX &&
                node->AsParseNodeFloat()->dbl <= (double)UINT_MAX;
        }

        static bool IsDefinition( ParseNode *arg );
        static bool ParseVarOrConstStatement( AsmJSParser &parser, ParseNode **var );
        static inline bool IsNumericLiteral(ParseNode* node) { return node && (node->nop == knopInt || node->nop == knopFlt); }
        static inline bool IsFroundNumericLiteral(ParseNode* node) { return node && (IsNumericLiteral(node) || IsNegativeZero(node)); }
        static inline ParseNode* GetUnaryNode( ParseNode* node ){return node->AsParseNodeUni()->pnode1;}
        static inline ParseNode* GetBinaryLeft( ParseNode* node ){return node->AsParseNodeBin()->pnode1;}
        static inline ParseNode* GetBinaryRight( ParseNode* node ){return node->AsParseNodeBin()->pnode2;}
        static inline ParseNode* DotBase( ParseNode *node );
        static inline bool IsDotMember( ParseNode *node );
        static inline PropertyName DotMember( ParseNode *node );
        // Get the VarDecl from the node or nullptr if unable to find
        static ParseNode* GetVarDeclList(ParseNode* node);
        // Goes through the nodes until the end of the list of VarDecl
        static void ReachEndVarDeclList( ParseNode** node );
    };

    bool ParserWrapper::IsNameDeclaration( ParseNode *node )
    {
        return node->nop == knopName || node->nop == knopStr;
    }

    bool ParserWrapper::IsNegativeZero(ParseNode *node)
    {
        return node && ((node->nop == knopFlt && JavascriptNumber::IsNegZero(node->AsParseNodeFloat()->dbl)) ||
            (node->nop == knopNeg && node->AsParseNodeUni()->pnode1->nop == knopInt && node->AsParseNodeUni()->pnode1->AsParseNodeInt()->lw == 0));
    }

    bool ParserWrapper::IsUInt( ParseNode *node )
    {
        return node->nop == knopInt || IsUnsigned(node);
    }

    uint ParserWrapper::GetUInt( ParseNode *node )
    {
        Assert( IsUInt( node ) );
        if( node->nop == knopInt )
        {
            return (uint)node->AsParseNodeInt()->lw;
        }
        Assert( node->nop == knopFlt );
        return (uint)node->AsParseNodeFloat()->dbl;
    }

    bool ParserWrapper::IsDotMember( ParseNode *node )
    {
        return node && (node->nop == knopDot || node->nop == knopIndex);
    }

    PropertyName ParserWrapper::DotMember( ParseNode *node )
    {
        Assert( IsDotMember(node) );
        if( IsNameDeclaration( GetBinaryRight( node ) ) )
        {
            return GetBinaryRight( node )->name();
        }
        return nullptr;
    }

    ParseNode* ParserWrapper::DotBase( ParseNode *node )
    {
        Assert( IsDotMember( node ) );
        return GetBinaryLeft( node );
    }

    ParseNode * ParserWrapper::GetListHead( ParseNode *node )
    {
        Assert( node->nop == knopList );
        return node->AsParseNodeBin()->pnode1;
    }
};
#endif
