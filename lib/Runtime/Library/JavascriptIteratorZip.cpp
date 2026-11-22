//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/JavascriptIteratorZip.h"

namespace Js
{
    Var JavascriptIterator::EntryZip(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        (void)args;
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.zip is not yet implemented"));
    }

    Var JavascriptIterator::EntryZipKeyed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        (void)args;
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.zipKeyed is not yet implemented"));
    }

    bool JavascriptIteratorHelper::AdvanceZip(ScriptContext* scriptContext, Var* outValue)
    {
        (void)scriptContext;
        (void)outValue;
        return false;
    }

    void JavascriptIteratorHelper::CloseZipNormal(ScriptContext* scriptContext)
    {
        (void)scriptContext;
    }

    void JavascriptIteratorHelper::CloseZipSwallow(ScriptContext* scriptContext)
    {
        (void)scriptContext;
    }

    void JavascriptIteratorHelper::ReturnZip(ScriptContext* scriptContext)
    {
        (void)scriptContext;
    }
} // namespace Js
