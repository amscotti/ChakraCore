//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace JSON
{
    // TaggedInt brand sentinel stored in InternalPropertyIds::HiddenObject on
    // null-prototype frozen objects created by JSON.rawJSON().
    static constexpr int32 kRawJsonHiddenSentinel = 0x124A534E;

    class EntryInfo
    {
    public:
        static Js::FunctionInfo Stringify;
        static Js::FunctionInfo Parse;
        static Js::FunctionInfo RawJSON;
        static Js::FunctionInfo IsRawJSON;
    };

    Js::Var Stringify(Js::RecyclableObject* function, Js::CallInfo callInfo, ...);
    Js::Var Parse(Js::RecyclableObject* function, Js::CallInfo callInfo, ...);
    Js::Var RawJSON(Js::RecyclableObject* function, Js::CallInfo callInfo, ...);
    Js::Var IsRawJSON(Js::RecyclableObject* function, Js::CallInfo callInfo, ...);
    bool IsRawJSONObject(Js::Var val, Js::ScriptContext* scriptContext, Js::JavascriptString** outRawString = nullptr);
} // namespace JSON
