//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
#ifdef INTL_ICU
    class IntlDisplayNames
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo SupportedLocalesOf;
            static FunctionInfo Of;
            static FunctionInfo ResolvedOptions;
        };

        static void Initialize(JavascriptLibrary* library, DynamicObject* intlObject);

        static Var EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryOf(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...);
    };
#endif
}
