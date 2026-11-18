//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "EngineInterfaceObject.h"
#ifdef INTL_ICU
#include "Library/IntlModernShared.h"
#include "Library/IntlLocale.h"
#include "Library/IntlListFormat.h"
#include "Library/IntlRelativeTimeFormat.h"
#include "Library/IntlDisplayNames.h"
#include "Library/IntlSegmenter.h"
#include "Library/IntlEnumeration.h"
#include "Library/IntlDurationFormat.h"
#endif

namespace Js
{
    // Declare all the entry infos
#define BUILTIN(c, n, e, i) FunctionInfo c::EntryInfo::n(FORCE_NO_WRITE_BARRIER_TAG(c::e), (Js::FunctionInfo::Attributes)(i), JavascriptBuiltInFunction:: ## c ## _ ## n);
#define BUILTIN_TEMPLATE(c, n, e, i) template<> BUILTIN(c, n, e, i)
#include "JavascriptBuiltInFunctionList.h"
#undef BUILTIN


    FunctionInfo * const JavascriptBuiltInFunction::builtInFunctionInfo[MaxBuiltInEnum] =
    {
    #define BUILTIN(c, n, e, i) &c::EntryInfo::n,
    #include "JavascriptBuiltInFunctionList.h"
    #undef BUILTIN
    };

    FunctionInfo *
    JavascriptBuiltInFunction::GetFunctionInfo(Js::LocalFunctionId builtinId)
    {
        if (IsValidId(builtinId))
        {
            return builtInFunctionInfo[builtinId];
        }
        return nullptr;
    }

    bool
    JavascriptBuiltInFunction::CanChangeEntryPoint(Js::LocalFunctionId builtInId)
    {
        return IsValidId(builtInId);
    }

    bool
    JavascriptBuiltInFunction::IsValidId(Js::LocalFunctionId builtInId)
    {
        return builtInId < MaxBuiltInEnum;
    }
};
