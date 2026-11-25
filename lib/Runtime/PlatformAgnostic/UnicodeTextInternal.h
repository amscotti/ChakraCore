//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace PlatformAgnostic
{
namespace UnicodeText
{

// Instantiate templates here rather than in each implementing file
// (declared extern here; the definitions live at the end of each implementing
// .cpp, after the template definition is visible -- clang rejects explicit
// instantiation definitions that precede the definition).
extern template charcount_t ChangeStringLinguisticCase<true, true>(const char16* sourceString, charcount_t sourceLength, char16* destString, charcount_t destLength, ApiError* pErrorOut);
extern template charcount_t ChangeStringLinguisticCase<true, false>(const char16* sourceString, charcount_t sourceLength, char16* destString, charcount_t destLength, ApiError* pErrorOut);
extern template charcount_t ChangeStringLinguisticCase<false, true>(const char16* sourceString, charcount_t sourceLength, char16* destString, charcount_t destLength, ApiError* pErrorOut);
extern template charcount_t ChangeStringLinguisticCase<false, false>(const char16* sourceString, charcount_t sourceLength, char16* destString, charcount_t destLength, ApiError* pErrorOut);

namespace Internal
{

int LogicalStringCompareImpl(const char16* p1, int p1size, const char16* p2, int p2size);

}; // namespace Internal
}; // namespace UnicodeText
}; // namespace PlatformAgnostic
