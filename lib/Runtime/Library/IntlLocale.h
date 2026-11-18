//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
#ifdef INTL_ICU
    class IntlLocale
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo ToString;
            static FunctionInfo Maximize;
            static FunctionInfo Minimize;
            static FunctionInfo GetterBaseName;
            static FunctionInfo GetterCalendar;
            static FunctionInfo GetterCaseFirst;
            static FunctionInfo GetterCollation;
            static FunctionInfo GetterFirstDayOfWeek;
            static FunctionInfo GetterHourCycle;
            static FunctionInfo GetterLanguage;
            static FunctionInfo GetterNumberingSystem;
            static FunctionInfo GetterNumeric;
            static FunctionInfo GetterRegion;
            static FunctionInfo GetterScript;
            static FunctionInfo GetterVariants;
            static FunctionInfo GetCalendars;
            static FunctionInfo GetCollations;
            static FunctionInfo GetHourCycles;
            static FunctionInfo GetNumberingSystems;
            static FunctionInfo GetTimeZones;
            static FunctionInfo GetTextInfo;
            static FunctionInfo GetWeekInfo;
        };

        static void Initialize(JavascriptLibrary* library, DynamicObject* intlObject);

        static Var EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToString(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryMaximize(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryMinimize(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterBaseName(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterCalendar(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterCaseFirst(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterCollation(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterFirstDayOfWeek(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterHourCycle(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterLanguage(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterNumberingSystem(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterNumeric(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterRegion(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterScript(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterVariants(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetCalendars(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetCollations(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetHourCycles(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetNumberingSystems(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetTimeZones(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetTextInfo(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetWeekInfo(RecyclableObject* function, CallInfo callInfo, ...);
    };
#endif
}
