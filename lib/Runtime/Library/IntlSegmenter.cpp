//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlSegmenter.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlSegmenter::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("Segmenter"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.Segmenter"),
            nullptr,
            &ctor);

        library->SetIntlSegmenterPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        IntlModern::AddIntlMethod(library, proto, _u("segment"), &EntryInfo::Segment, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);

        DynamicObject* segmentsProto = library->CreateObject();
        IntlModern::AddIntlMethod(library, segmentsProto, _u("containing"), &EntryInfo::SegmentsContaining, 1);
        library->AddFunctionToLibraryObjectWithName(
            segmentsProto,
            PropertyIds::_symbolIterator,
            PropertyIds::_RuntimeFunctionNameId_iterator,
            &EntryInfo::SegmentsIterator,
            0);
        library->SetIntlSegmentsPrototype(segmentsProto);

        DynamicObject* segIterProto = library->CreateObject(library->GetIteratorPrototype());
        library->AddFunctionToLibraryObject(segIterProto, PropertyIds::next, &EntryInfo::SegmentIteratorNext, 0);
        library->AddMember(
            segIterProto,
            PropertyIds::_symbolToStringTag,
            library->CreateStringFromCppLiteral(_u("Segmenter String Iterator")),
            PropertyConfigurable);
        library->SetIntlSegmentIteratorPrototype(segIterProto);
    }

    Var IntlSegmenter::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Segmenter"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Segmenter"));
        }
        return IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlSegmenterPrototype);
    }

#define INTL_SEGMENTER_STUB(Name) \
    Var IntlSegmenter::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        return function->GetScriptContext()->GetLibrary()->GetUndefined(); \
    }

    INTL_SEGMENTER_STUB(EntrySupportedLocalesOf)
    INTL_SEGMENTER_STUB(EntrySegment)
    INTL_SEGMENTER_STUB(EntryResolvedOptions)
    INTL_SEGMENTER_STUB(EntrySegmentsContaining)
    INTL_SEGMENTER_STUB(EntrySegmentsIterator)
    INTL_SEGMENTER_STUB(EntrySegmentIteratorNext)

#undef INTL_SEGMENTER_STUB
} // namespace Js

#endif // INTL_ICU
