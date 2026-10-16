//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptRegularExpressionResult
    {
    private:
        static const PropertyIndex InputIndex = 0;
        static const PropertyIndex IndexIndex = 1;
        static const PropertyIndex GroupsIndex = 2;

    public:
        // Constructor for storing matches for a global regex (not captures)
        static JavascriptArray* Create(void *const stackAllocationPointer, JavascriptString* input, ScriptContext* const scriptContext);

        // Constructor for storing captures
        static JavascriptArray* Create(void *const stackAllocationPointer, const int numGroups, JavascriptString* input, ScriptContext* const scriptContext);

        static void SetMatch(JavascriptArray* arr, const UnifiedRegex::GroupInfo match);

        // Own "groups" data property: the null-prototype groups object when
        // the pattern has named captures, undefined otherwise (ES2018: the
        // property exists unconditionally on exec/match results).
        static void SetGroups(JavascriptArray* arr, Var groups);

        // Must be >= 2, and equal to the total slot count (input, index, and
        // the S9 "groups" property) so that the slot array is not created,
        // rounded up to DynamicTypeHandler::RoundUpInlineSlotCapacity (3 -> 4;
        // HeapConstants::ObjectGranularity / sizeof(Var) is only 2).
        static const PropertyIndex InlineSlotCount = 4;

#if DEBUG
        static bool Is(JavascriptArray* arr);
#endif

    private:
        static void InstantiateForceInlinedMembers();
    };
} // namespace Js
