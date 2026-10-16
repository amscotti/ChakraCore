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
        static const PropertyIndex IndicesIndex = 3;

    public:
        // Constructor for storing matches for a global regex (not captures)
        static JavascriptArray* Create(void *const stackAllocationPointer, JavascriptString* input, ScriptContext* const scriptContext);

        // Constructor for storing captures. hasIndices selects the ES2022
        // 'd'-flag result type (the same shape plus an "indices" own
        // property); without it the plain 3-entry shape is used and no
        // indices are stored.
        static JavascriptArray* Create(void *const stackAllocationPointer, const int numGroups, JavascriptString* input, ScriptContext* const scriptContext, const bool hasIndices);

        static void SetMatch(JavascriptArray* arr, const UnifiedRegex::GroupInfo match);

        // Own "groups" data property: the null-prototype groups object when
        // the pattern has named captures, undefined otherwise (ES2018: the
        // property exists unconditionally on exec/match results).
        static void SetGroups(JavascriptArray* arr, Var groups);

        // Own "indices" data property ('d' flag only, ES2022): the indices
        // array built by RegexHelper::CreateIndicesArray. Only valid on
        // results created with hasIndices == true.
        static void SetIndices(JavascriptArray* arr, Var indices);

        // Must be >= 2, and equal to the total slot count (input, index, the
        // S9 "groups" property and the S9 sub-batch 2 "indices" property, the
        // last of which only the 'd'-flag result type carries) so that the
        // slot array is not created, rounded up to
        // DynamicTypeHandler::RoundUpInlineSlotCapacity (3 -> 4; 4 -> 4;
        // HeapConstants::ObjectGranularity / sizeof(Var) is only 2).
        static const PropertyIndex InlineSlotCount = 4;

#if DEBUG
        static bool Is(JavascriptArray* arr);
#endif

    private:
        static void InstantiateForceInlinedMembers();
    };
} // namespace Js
