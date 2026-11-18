//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlEnumeration.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlEnumeration::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        IntlModern::AddIntlMethod(
            library,
            intlObject,
            _u("supportedValuesOf"),
            &EntryInfo::SupportedValuesOf,
            1);
    }

    Var IntlEnumeration::EntrySupportedValuesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.supportedValuesOf"));
        if (callInfo.Flags & CallFlags_New)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ErrorOnNew);
        }

        Var key = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        (void)key;
        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.supportedValuesOf is not yet implemented"));
    }
} // namespace Js

#endif // INTL_ICU
