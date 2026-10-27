//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace UnifiedRegex
{
    enum RegexFlags : uint8
    {
        NoRegexFlags        = 0,
        IgnoreCaseRegexFlag = 1 << 0,
        GlobalRegexFlag     = 1 << 1,
        MultilineRegexFlag  = 1 << 2,
        UnicodeRegexFlag    = 1 << 3,
        StickyRegexFlag     = 1 << 4,
        DotAllRegexFlag     = 1 << 5,
        HasIndicesRegexFlag = 1 << 6,   // ES2022 'd' flag (match indices); unconditional, no config flag
        UnicodeSetsRegexFlag = 1 << 7,  // ES2024 'v' flag (Unicode sets); gated by ES6UnicodeExtensions like 'u'
        AllRegexFlags       = (1 << 8) - 1
    };
}
