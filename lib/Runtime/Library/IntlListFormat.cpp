//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlListFormat.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlListFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("ListFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.ListFormat"),
            nullptr,
            &ctor);

        library->SetIntlListFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlListFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.ListFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.ListFormat"));
        }
        return IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlListFormatPrototype);
    }

#define INTL_LISTFORMAT_STUB(Name) \
    Var IntlListFormat::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        return function->GetScriptContext()->GetLibrary()->GetUndefined(); \
    }

    INTL_LISTFORMAT_STUB(EntrySupportedLocalesOf)
    INTL_LISTFORMAT_STUB(EntryFormat)
    INTL_LISTFORMAT_STUB(EntryFormatToParts)
    INTL_LISTFORMAT_STUB(EntryResolvedOptions)

#undef INTL_LISTFORMAT_STUB
} // namespace Js

#endif // INTL_ICU
