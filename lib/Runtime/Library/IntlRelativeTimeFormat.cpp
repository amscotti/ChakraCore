//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlRelativeTimeFormat.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlRelativeTimeFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("RelativeTimeFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.RelativeTimeFormat"),
            nullptr,
            &ctor);

        library->SetIntlRelativeTimeFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 2);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 2);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlRelativeTimeFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.RelativeTimeFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.RelativeTimeFormat"));
        }
        return IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlRelativeTimeFormatPrototype);
    }

#define INTL_RTF_STUB(Name) \
    Var IntlRelativeTimeFormat::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        return function->GetScriptContext()->GetLibrary()->GetUndefined(); \
    }

    INTL_RTF_STUB(EntrySupportedLocalesOf)
    INTL_RTF_STUB(EntryFormat)
    INTL_RTF_STUB(EntryFormatToParts)
    INTL_RTF_STUB(EntryResolvedOptions)

#undef INTL_RTF_STUB
} // namespace Js

#endif // INTL_ICU
