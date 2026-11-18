//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
#ifdef INTL_ICU
    class IntlEnumeration
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo SupportedValuesOf;
        };

        static void Initialize(JavascriptLibrary* library, DynamicObject* intlObject);

        static Var EntrySupportedValuesOf(RecyclableObject* function, CallInfo callInfo, ...);
    };
#endif
}
