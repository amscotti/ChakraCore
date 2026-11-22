//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/JavascriptIteratorChunking.h"

namespace Js
{
    Var JavascriptIterator::EntryChunks(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        (void)args;
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.prototype.chunks is not yet implemented"));
    }

    Var JavascriptIterator::EntryWindows(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        (void)args;
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.prototype.windows is not yet implemented"));
    }

    bool JavascriptIteratorHelper::AdvanceChunking(ScriptContext* scriptContext, Var* outValue)
    {
        (void)scriptContext;
        (void)outValue;
        return false;
    }

    void JavascriptIteratorHelper::CloseChunkingNormal(ScriptContext* scriptContext)
    {
        (void)scriptContext;
    }

    void JavascriptIteratorHelper::CloseChunkingSwallow(ScriptContext* scriptContext)
    {
        (void)scriptContext;
    }
} // namespace Js
