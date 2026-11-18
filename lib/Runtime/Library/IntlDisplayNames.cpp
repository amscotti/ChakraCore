//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlDisplayNames.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlDisplayNames::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("DisplayNames"),
            &EntryInfo::NewInstance,
            2,
            _u("Intl.DisplayNames"),
            nullptr,
            &ctor);

        library->SetIntlDisplayNamesPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::of, &EntryInfo::Of, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlDisplayNames::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DisplayNames"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DisplayNames"));
        }
        return IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlDisplayNamesPrototype);
    }

#define INTL_DISPLAYNAMES_STUB(Name) \
    Var IntlDisplayNames::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        return function->GetScriptContext()->GetLibrary()->GetUndefined(); \
    }

    INTL_DISPLAYNAMES_STUB(EntrySupportedLocalesOf)
    INTL_DISPLAYNAMES_STUB(EntryOf)
    INTL_DISPLAYNAMES_STUB(EntryResolvedOptions)

#undef INTL_DISPLAYNAMES_STUB
} // namespace Js

#endif // INTL_ICU
