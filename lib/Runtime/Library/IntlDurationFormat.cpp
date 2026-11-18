//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlDurationFormat.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlDurationFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("DurationFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.DurationFormat"),
            nullptr,
            &ctor);

        library->SetIntlDurationFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlDurationFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DurationFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DurationFormat"));
        }
        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat is not yet implemented"));
    }

#define INTL_DURATIONFORMAT_STUB(Name, EntryTag, ErrorMessage) \
    Var IntlDurationFormat::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        ScriptContext* scriptContext = function->GetScriptContext(); \
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, EntryTag); \
        if (callInfo.Flags & CallFlags_New) \
        { \
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ErrorOnNew); \
        } \
        Var arg0 = args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(); \
        (void)arg0; \
        IntlModern::ThrowRangeErrorMessage(scriptContext, ErrorMessage); \
    }

    INTL_DURATIONFORMAT_STUB(
        EntrySupportedLocalesOf,
        _u("Intl.DurationFormat.supportedLocalesOf"),
        _u("Intl.DurationFormat.supportedLocalesOf is not yet implemented"))
    INTL_DURATIONFORMAT_STUB(
        EntryFormat,
        _u("Intl.DurationFormat.prototype.format"),
        _u("Intl.DurationFormat.prototype.format is not yet implemented"))
    INTL_DURATIONFORMAT_STUB(
        EntryFormatToParts,
        _u("Intl.DurationFormat.prototype.formatToParts"),
        _u("Intl.DurationFormat.prototype.formatToParts is not yet implemented"))
    INTL_DURATIONFORMAT_STUB(
        EntryResolvedOptions,
        _u("Intl.DurationFormat.prototype.resolvedOptions"),
        _u("Intl.DurationFormat.prototype.resolvedOptions is not yet implemented"))

#undef INTL_DURATIONFORMAT_STUB
} // namespace Js

#endif // INTL_ICU
