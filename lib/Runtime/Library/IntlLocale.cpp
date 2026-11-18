//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlLocale.h"

#ifdef INTL_ICU

namespace Js
{
    void IntlLocale::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("Locale"),
            &EntryInfo::NewInstance,
            1,
            _u("Intl.Locale"));

        library->SetIntlLocalePrototype(proto);

        library->AddFunctionToLibraryObject(proto, PropertyIds::toString, &EntryInfo::ToString, 0);
        IntlModern::AddIntlMethod(library, proto, _u("maximize"), &EntryInfo::Maximize, 0);
        IntlModern::AddIntlMethod(library, proto, _u("minimize"), &EntryInfo::Minimize, 0);

        IntlModern::AddIntlGetter(library, proto, _u("baseName"), &EntryInfo::GetterBaseName);
        library->AddGetterToLibraryObject(proto, PropertyIds::calendar, &EntryInfo::GetterCalendar);
        IntlModern::AddIntlGetter(library, proto, _u("caseFirst"), &EntryInfo::GetterCaseFirst);
        IntlModern::AddIntlGetter(library, proto, _u("collation"), &EntryInfo::GetterCollation);
        IntlModern::AddIntlGetter(library, proto, _u("firstDayOfWeek"), &EntryInfo::GetterFirstDayOfWeek);
        IntlModern::AddIntlGetter(library, proto, _u("hourCycle"), &EntryInfo::GetterHourCycle);
        IntlModern::AddIntlGetter(library, proto, _u("language"), &EntryInfo::GetterLanguage);
        library->AddGetterToLibraryObject(proto, PropertyIds::numberingSystem, &EntryInfo::GetterNumberingSystem);
        library->AddGetterToLibraryObject(proto, PropertyIds::numeric, &EntryInfo::GetterNumeric);
        IntlModern::AddIntlGetter(library, proto, _u("region"), &EntryInfo::GetterRegion);
        IntlModern::AddIntlGetter(library, proto, _u("script"), &EntryInfo::GetterScript);
        IntlModern::AddIntlGetter(library, proto, _u("variants"), &EntryInfo::GetterVariants);

        IntlModern::AddIntlMethod(library, proto, _u("getCalendars"), &EntryInfo::GetCalendars, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getCollations"), &EntryInfo::GetCollations, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getHourCycles"), &EntryInfo::GetHourCycles, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getNumberingSystems"), &EntryInfo::GetNumberingSystems, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getTimeZones"), &EntryInfo::GetTimeZones, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getTextInfo"), &EntryInfo::GetTextInfo, 0);
        IntlModern::AddIntlMethod(library, proto, _u("getWeekInfo"), &EntryInfo::GetWeekInfo, 0);
    }

    Var IntlLocale::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Locale"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.Locale"));
        }
        return IntlModern::OrdinaryCreateFromConstructor(newTarget, scriptContext, &JavascriptLibrary::GetIntlLocalePrototype);
    }

#define INTL_LOCALE_STUB(Name) \
    Var IntlLocale::Name(RecyclableObject* function, CallInfo callInfo, ...) \
    { \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        return function->GetScriptContext()->GetLibrary()->GetUndefined(); \
    }

    INTL_LOCALE_STUB(EntryToString)
    INTL_LOCALE_STUB(EntryMaximize)
    INTL_LOCALE_STUB(EntryMinimize)
    INTL_LOCALE_STUB(EntryGetterBaseName)
    INTL_LOCALE_STUB(EntryGetterCalendar)
    INTL_LOCALE_STUB(EntryGetterCaseFirst)
    INTL_LOCALE_STUB(EntryGetterCollation)
    INTL_LOCALE_STUB(EntryGetterFirstDayOfWeek)
    INTL_LOCALE_STUB(EntryGetterHourCycle)
    INTL_LOCALE_STUB(EntryGetterLanguage)
    INTL_LOCALE_STUB(EntryGetterNumberingSystem)
    INTL_LOCALE_STUB(EntryGetterNumeric)
    INTL_LOCALE_STUB(EntryGetterRegion)
    INTL_LOCALE_STUB(EntryGetterScript)
    INTL_LOCALE_STUB(EntryGetterVariants)
    INTL_LOCALE_STUB(EntryGetCalendars)
    INTL_LOCALE_STUB(EntryGetCollations)
    INTL_LOCALE_STUB(EntryGetHourCycles)
    INTL_LOCALE_STUB(EntryGetNumberingSystems)
    INTL_LOCALE_STUB(EntryGetTimeZones)
    INTL_LOCALE_STUB(EntryGetTextInfo)
    INTL_LOCALE_STUB(EntryGetWeekInfo)

#undef INTL_LOCALE_STUB
} // namespace Js

#endif // INTL_ICU
