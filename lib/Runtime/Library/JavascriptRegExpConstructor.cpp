//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "RegexPattern.h"

namespace Js
{
    // The VS2013 linker treats this as a redefinition of an already
    // defined constant and complains. So skip the declaration if we're compiling
    // with VS2013 or below.
#if !defined(_MSC_VER) || _MSC_VER >= 1900
    const int JavascriptRegExpConstructor::NumCtorCaptures;
#endif

    JavascriptRegExpConstructor::JavascriptRegExpConstructor(DynamicType* type, ConstructorCache* cache) :
        RuntimeFunction(type, &JavascriptRegExp::EntryInfo::NewInstance, cache),
        reset(false),
        invalidatedLastMatch(false),
        frozenSpecialMask(0),
        lastPattern(nullptr),
        frozenLastInput(nullptr),
        lastMatch() // undefined
    {
        DebugOnly(VerifyEntryPoint());
        ScriptContext* scriptContext = this->GetScriptContext();
        JavascriptString* emptyString = scriptContext->GetLibrary()->GetEmptyString();
        this->lastInput = emptyString;
        this->index = JavascriptNumber::ToVar(-1, scriptContext);
        this->lastIndex = JavascriptNumber::ToVar(-1, scriptContext);
        this->lastParen = emptyString;
        this->leftContext = emptyString;
        this->rightContext = emptyString;
        for (int i = 0; i < NumCtorCaptures; i++)
        {
            this->captures[i] = emptyString;
        }
    }

    BOOL JavascriptRegExpConstructor::GetEnumerator(JavascriptStaticEnumerator * enumerator, EnumeratorFlags flags, ScriptContext* requestContext, EnumeratorCache * enumeratorCache)
    {
        return GetEnumeratorWithPrefix(
            RecyclerNew(GetScriptContext()->GetRecycler(), JavascriptRegExpEnumerator, this, flags, requestContext),
            enumerator, flags, requestContext, enumeratorCache);
    }

    void JavascriptRegExpConstructor::SetLastMatch(UnifiedRegex::RegexPattern* lastPattern, JavascriptString* lastInput, UnifiedRegex::GroupInfo lastMatch)
    {
        AssertMsg(!lastMatch.IsUndefined(), "SetLastMatch should only be called if there's a successful match");
        AssertMsg(lastPattern != nullptr, "lastPattern should not be null");
        AssertMsg(lastInput != nullptr, "lastInput should not be null");
        AssertMsg(JavascriptOperators::GetTypeId(lastInput) != TypeIds_Null, "lastInput should not be JavaScript null");

        this->lastPattern = lastPattern;
        this->lastInput = lastInput;
        this->lastMatch = lastMatch;
        this->reset = true;
        this->invalidatedLastMatch = false;
    }

    void JavascriptRegExpConstructor::InvalidateLastMatch(UnifiedRegex::RegexPattern* lastPattern, JavascriptString* lastInput)
    {
        AssertMsg(lastPattern != nullptr, "lastPattern should not be null");
        AssertMsg(lastInput != nullptr, "lastInput should not be null");
        AssertMsg(JavascriptOperators::GetTypeId(lastInput) != TypeIds_Null, "lastInput should not be JavaScript null");

        this->lastPattern = lastPattern;
        this->lastInput = lastInput;
        this->lastMatch.Reset();
        this->reset = true;
        this->invalidatedLastMatch = true;
    }

    enum RegExpSpecialFreezeBit : uint32
    {
        FreezeBit_Capture0     = 1u << 0,
        FreezeBit_Capture1     = 1u << 1,
        FreezeBit_Capture2     = 1u << 2,
        FreezeBit_Capture3     = 1u << 3,
        FreezeBit_Capture4     = 1u << 4,
        FreezeBit_Capture5     = 1u << 5,
        FreezeBit_Capture6     = 1u << 6,
        FreezeBit_Capture7     = 1u << 7,
        FreezeBit_Capture8     = 1u << 8,
        FreezeBit_Capture9     = 1u << 9,
        FreezeBit_Input        = 1u << 10,
        FreezeBit_LastParen    = 1u << 11,
        FreezeBit_LeftContext  = 1u << 12,
        FreezeBit_RightContext = 1u << 13,
        FreezeBit_Index        = 1u << 14,
    };

    uint32 JavascriptRegExpConstructor::GetSpecialFreezeMask(PropertyId propertyId)
    {
        switch (propertyId)
        {
        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:
            return FreezeBit_Capture0;
        case PropertyIds::$1: return FreezeBit_Capture1;
        case PropertyIds::$2: return FreezeBit_Capture2;
        case PropertyIds::$3: return FreezeBit_Capture3;
        case PropertyIds::$4: return FreezeBit_Capture4;
        case PropertyIds::$5: return FreezeBit_Capture5;
        case PropertyIds::$6: return FreezeBit_Capture6;
        case PropertyIds::$7: return FreezeBit_Capture7;
        case PropertyIds::$8: return FreezeBit_Capture8;
        case PropertyIds::$9: return FreezeBit_Capture9;
        case PropertyIds::input:
        case PropertyIds::$_:
            return FreezeBit_Input;
        case PropertyIds::lastParen:
        case PropertyIds::$Plus:
            return FreezeBit_LastParen;
        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:
            return FreezeBit_LeftContext;
        case PropertyIds::rightContext:
        case PropertyIds::$Tick:
            return FreezeBit_RightContext;
        case PropertyIds::index:
            return FreezeBit_Index;
        default:
            return 0;
        }
    }

    void JavascriptRegExpConstructor::EnsureValues()
    {
        if (reset)
        {
            ScriptContext* scriptContext = this->GetScriptContext();
            const CharCount lastInputLen = lastInput->GetLength();
            const char16* lastInputStr = lastInput->GetString();
            UnifiedRegex::RegexPattern* pattern = lastPattern;

            // When we perform a regex test operation it's possible the result of the operation will be loaded from a cache and the match will not be computed and updated in the ctor.
            // In that case we invalidate the last match stored in the ctor and will need to compute it before it will be accessible via $1 etc.
            // Since we only do this for the case of RegExp.prototype.test cache hit, we know several things:
            //  - The regex is not global or sticky
            //  - There was a match (test returned true)
            if (invalidatedLastMatch)
            {
                this->lastMatch = RegexHelper::SimpleMatch(scriptContext, pattern, lastInputStr, lastInputLen, 0);
                invalidatedLastMatch = false;
            }

            Assert(!lastMatch.IsUndefined());
            JavascriptString* emptyString = scriptContext->GetLibrary()->GetEmptyString();
            CharCount lastIndexVal = lastMatch.EndOffset();
            if (!(frozenSpecialMask & FreezeBit_Index))
            {
                this->index = JavascriptNumber::ToVar(lastMatch.offset, scriptContext);
            }
            this->lastIndex = JavascriptNumber::ToVar(lastIndexVal, scriptContext);
            if (!(frozenSpecialMask & FreezeBit_LeftContext))
            {
                this->leftContext = lastMatch.offset > 0 ? SubString::New(lastInput, 0, lastMatch.offset) : emptyString;
            }
            if (!(frozenSpecialMask & FreezeBit_RightContext))
            {
                this->rightContext = lastIndexVal > 0 && lastIndexVal < lastInputLen ? SubString::New(lastInput, lastIndexVal, lastInputLen - lastIndexVal) : emptyString;
            }

            Var nonMatchValue = RegexHelper::NonMatchValue(scriptContext, true);
            if (!(frozenSpecialMask & FreezeBit_Capture0))
            {
                captures[0] = RegexHelper::GetString(scriptContext, lastInput, nonMatchValue, lastMatch);
            }
            int numGroups = pattern->NumGroups();
            if (numGroups > 1)
            {
                // The RegExp constructor's lastMatch holds the last *successful* match on any regular expression.
                // That regular expression may since have been used for *unsuccessful* matches, in which case
                // its groups will have been reset. Updating the RegExp constructor with the group binding after
                // every match is prohibitively slow. Instead, run the match again using the known last input string.
                if (!pattern->WasLastMatchSuccessful())
                {
                    RegexHelper::SimpleMatch(scriptContext, pattern, lastInputStr, lastInputLen, lastMatch.offset);
                }
                Assert(pattern->WasLastMatchSuccessful());
                Var computedLastParen = nullptr;
                for (int groupId = 1; groupId < min(numGroups, NumCtorCaptures); groupId++)
                {
                    Var groupVal = RegexHelper::GetGroup(scriptContext, pattern, lastInput, nonMatchValue, groupId);
                    if (!(frozenSpecialMask & (1u << groupId)))
                    {
                        captures[groupId] = groupVal;
                    }
                    if (groupId == numGroups - 1)
                    {
                        computedLastParen = groupVal;
                    }
                }

                if (!(frozenSpecialMask & FreezeBit_LastParen))
                {
                    this->lastParen = numGroups <= NumCtorCaptures ?
                        computedLastParen :
                        RegexHelper::GetGroup(scriptContext, pattern, lastInput, nonMatchValue, numGroups - 1);
                }
            }
            else if (!(frozenSpecialMask & FreezeBit_LastParen))
            {
                this->lastParen = emptyString;
            }
            for (int groupId = numGroups; groupId < NumCtorCaptures; groupId++)
            {
                if (!(frozenSpecialMask & (1u << groupId)))
                {
                    captures[groupId] = emptyString;
                }
            }
            reset = false;
        }
        else
        {
            // If we are not resetting the values, the last match cannot be invalidated.
            Assert(!invalidatedLastMatch);
        }
    }

    /*static*/
    PropertyId const JavascriptRegExpConstructor::specialPropertyIds[] =
    {
        PropertyIds::$_,
        PropertyIds::$Ampersand,
        PropertyIds::$Plus,
        PropertyIds::$BackTick,
        PropertyIds::$Tick,
        PropertyIds::index,
    };

    PropertyId const JavascriptRegExpConstructor::specialEnumPropertyIds[] =
    {
        PropertyIds::$1,
        PropertyIds::$2,
        PropertyIds::$3,
        PropertyIds::$4,
        PropertyIds::$5,
        PropertyIds::$6,
        PropertyIds::$7,
        PropertyIds::$8,
        PropertyIds::$9,
        PropertyIds::input,
        PropertyIds::rightContext,
        PropertyIds::leftContext,
        PropertyIds::lastParen,
        PropertyIds::lastMatch,
    };

    PropertyId const JavascriptRegExpConstructor::specialnonEnumPropertyIds[] =
    {
        PropertyIds::$_,
        PropertyIds::$Ampersand,
        PropertyIds::$Plus,
        PropertyIds::$BackTick,
        PropertyIds::$Tick,
        PropertyIds::index,
    };

    PropertyQueryFlags JavascriptRegExpConstructor::HasPropertyQuery(PropertyId propertyId, _Inout_opt_ PropertyValueInfo* info)
    {
        switch (propertyId)
        {
        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:
        case PropertyIds::lastParen:
        case PropertyIds::$Plus:
        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:
        case PropertyIds::rightContext:
        case PropertyIds::$Tick:
        case PropertyIds::index:
        case PropertyIds::input:
        case PropertyIds::$_:
        case PropertyIds::$1:
        case PropertyIds::$2:
        case PropertyIds::$3:
        case PropertyIds::$4:
        case PropertyIds::$5:
        case PropertyIds::$6:
        case PropertyIds::$7:
        case PropertyIds::$8:
        case PropertyIds::$9:
            return PropertyQueryFlags::Property_Found;
        default:
            return JavascriptFunction::HasPropertyQuery(propertyId, info);
        }
    }

    PropertyQueryFlags JavascriptRegExpConstructor::GetPropertyReferenceQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        return JavascriptRegExpConstructor::GetPropertyQuery(originalInstance, propertyId, value, info, requestContext);
    }

    PropertyQueryFlags JavascriptRegExpConstructor::GetPropertyQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        BOOL result;
        if (GetPropertyBuiltIns(propertyId, value, &result))
        {
            return JavascriptConversion::BooleanToPropertyQueryFlags(result);
        }

        return JavascriptFunction::GetPropertyQuery(originalInstance, propertyId, value, info, requestContext);
    }

    PropertyQueryFlags JavascriptRegExpConstructor::GetPropertyQuery(Var originalInstance, JavascriptString* propertyNameString, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        BOOL result;
        PropertyRecord const* propertyRecord;
        this->GetScriptContext()->FindPropertyRecord(propertyNameString, &propertyRecord);

        if (propertyRecord != nullptr && GetPropertyBuiltIns(propertyRecord->GetPropertyId(), value, &result))
        {
            return JavascriptConversion::BooleanToPropertyQueryFlags(result);
        }

        return JavascriptFunction::GetPropertyQuery(originalInstance, propertyNameString, value, info, requestContext);
    }

    bool JavascriptRegExpConstructor::GetPropertyBuiltIns(PropertyId propertyId, Var* value, BOOL* result)
    {
        switch (propertyId)
        {
        case PropertyIds::input:
        case PropertyIds::$_:
            this->EnsureValues();
            *value = (frozenSpecialMask & FreezeBit_Input) ? this->frozenLastInput : this->lastInput;
            *result = true;
            return true;
        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:
            this->EnsureValues();
            *value = this->captures[0];
            *result = true;
            return true;
        case PropertyIds::lastParen:
        case PropertyIds::$Plus:
            this->EnsureValues();
            *value = this->lastParen;
            *result = true;
            return true;
        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:
            this->EnsureValues();
            *value = this->leftContext;
            *result = true;
            return true;
        case PropertyIds::rightContext:
        case PropertyIds::$Tick:
            this->EnsureValues();
            *value = this->rightContext;
            *result = true;
            return true;
        case PropertyIds::$1:
            this->EnsureValues();
            *value = this->captures[1];
            *result = true;
            return true;
        case PropertyIds::$2:
            this->EnsureValues();
            *value = this->captures[2];
            *result = true;
            return true;
        case PropertyIds::$3:
            this->EnsureValues();
            *value = this->captures[3];
            *result = true;
            return true;
        case PropertyIds::$4:
            this->EnsureValues();
            *value = this->captures[4];
            *result = true;
            return true;
        case PropertyIds::$5:
            this->EnsureValues();
            *value = this->captures[5];
            *result = true;
            return true;
        case PropertyIds::$6:
            this->EnsureValues();
            *value = this->captures[6];
            *result = true;
            return true;
        case PropertyIds::$7:
            this->EnsureValues();
            *value = this->captures[7];
            *result = true;
            return true;
        case PropertyIds::$8:
            this->EnsureValues();
            *value = this->captures[8];
            *result = true;
            return true;
        case PropertyIds::$9:
            this->EnsureValues();
            *value = this->captures[9];
            *result = true;
            return true;
        case PropertyIds::index:
            this->EnsureValues();
            *value = this->index;
            *result = true;
            return true;
        default:
            return false;
        }
    }

    BOOL JavascriptRegExpConstructor::SetProperty(PropertyId propertyId, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        BOOL result;
        if (SetPropertyBuiltIns(propertyId, value, &result))
        {
            return result;
        }

        return JavascriptFunction::SetProperty(propertyId, value, flags, info);
    }

    BOOL JavascriptRegExpConstructor::SetProperty(JavascriptString* propertyNameString, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        BOOL result;
        PropertyRecord const * propertyRecord;
        this->GetScriptContext()->FindPropertyRecord(propertyNameString, &propertyRecord);

        if (propertyRecord != nullptr && SetPropertyBuiltIns(propertyRecord->GetPropertyId(), value, &result))
        {
            return result;
        }

        return JavascriptFunction::SetProperty(propertyNameString, value, flags, info);
    }

    bool JavascriptRegExpConstructor::SetPropertyBuiltIns(PropertyId propertyId, Var value, BOOL* result)
    {
        switch (propertyId)
        {
        case PropertyIds::input:
        case PropertyIds::$_:
            //TODO: review: although the 'input' property is marked as readonly, it has a set on V5.8. There is no spec on this.
            if (frozenSpecialMask & FreezeBit_Input)
            {
                *result = false;
                return true;
            }
            {
                auto tempInput = JavascriptConversion::ToString(value, this->GetScriptContext());
                // Above toString call can cause user code to be called, which may call .match to invalidate our state, ensure that we have proper values in case that happens.
                EnsureValues(); // The last match info relies on the last input. Use it before it is changed.
                this->lastInput = tempInput;
            }

            // Set implicit call flags since we are not necessarily making the original stored value available on re-load
            // and are killing the store that backs two exposed properties.
            this->GetScriptContext()->GetThreadContext()->AddImplicitCallFlags(ImplicitCall_Accessor);
            *result = true;
            return true;
        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:
        case PropertyIds::lastParen:
        case PropertyIds::$Plus:
        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:
        case PropertyIds::rightContext:
        case PropertyIds::$Tick:
        case PropertyIds::$1:
        case PropertyIds::$2:
        case PropertyIds::$3:
        case PropertyIds::$4:
        case PropertyIds::$5:
        case PropertyIds::$6:
        case PropertyIds::$7:
        case PropertyIds::$8:
        case PropertyIds::$9:
        case PropertyIds::index:
            *result = false;
            return true;
        default:
            return false;
        }
    }

    BOOL JavascriptRegExpConstructor::InitProperty(PropertyId propertyId, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        return SetProperty(propertyId, value, flags, info);
    }

    BOOL JavascriptRegExpConstructor::DeleteProperty(PropertyId propertyId, PropertyOperationFlags flags)
    {
        switch (propertyId)
        {
            // all globals are 'fNoDelete' in V5.8
        case PropertyIds::input:
        case PropertyIds::$_:
        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:
        case PropertyIds::lastParen:
        case PropertyIds::$Plus:
        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:
        case PropertyIds::rightContext:
        case PropertyIds::$Tick:
        case PropertyIds::$1:
        case PropertyIds::$2:
        case PropertyIds::$3:
        case PropertyIds::$4:
        case PropertyIds::$5:
        case PropertyIds::$6:
        case PropertyIds::$7:
        case PropertyIds::$8:
        case PropertyIds::$9:
        case PropertyIds::index:
            JavascriptError::ThrowCantDeleteIfStrictMode(flags, GetScriptContext(), GetScriptContext()->GetPropertyName(propertyId)->GetBuffer());
            return false;

        default:
            return JavascriptFunction::DeleteProperty(propertyId, flags);
        }
    }

    BOOL JavascriptRegExpConstructor::DeleteProperty(JavascriptString *propertyNameString, PropertyOperationFlags flags)
    {
        PropertyRecord const * propertyRecord = nullptr;
        propertyNameString->GetScriptContext()->GetOrAddPropertyRecord(propertyNameString, &propertyRecord);
        PropertyId propertyId = propertyRecord->GetPropertyId();

        if (BuiltInPropertyRecords::input.Equals(propertyId)
            || BuiltInPropertyRecords::$_.Equals(propertyId)
            || BuiltInPropertyRecords::lastMatch.Equals(propertyId)
            || BuiltInPropertyRecords::$Ampersand.Equals(propertyId)
            || BuiltInPropertyRecords::lastParen.Equals(propertyId)
            || BuiltInPropertyRecords::$Plus.Equals(propertyId)
            || BuiltInPropertyRecords::leftContext.Equals(propertyId)
            || BuiltInPropertyRecords::$BackTick.Equals(propertyId)
            || BuiltInPropertyRecords::rightContext.Equals(propertyId)
            || BuiltInPropertyRecords::$Tick.Equals(propertyId)
            || BuiltInPropertyRecords::$1.Equals(propertyId)
            || BuiltInPropertyRecords::$2.Equals(propertyId)
            || BuiltInPropertyRecords::$3.Equals(propertyId)
            || BuiltInPropertyRecords::$4.Equals(propertyId)
            || BuiltInPropertyRecords::$5.Equals(propertyId)
            || BuiltInPropertyRecords::$6.Equals(propertyId)
            || BuiltInPropertyRecords::$7.Equals(propertyId)
            || BuiltInPropertyRecords::$8.Equals(propertyId)
            || BuiltInPropertyRecords::$9.Equals(propertyId)
            || BuiltInPropertyRecords::index.Equals(propertyId))
        {
            JavascriptError::ThrowCantDeleteIfStrictMode(flags, GetScriptContext(), propertyNameString->GetString());
            return false;
        }

        return JavascriptFunction::DeleteProperty(propertyNameString, flags);
    }

    BOOL JavascriptRegExpConstructor::GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(JS_DIAG_VALUE_JavascriptRegExpConstructor);
        return TRUE;
    }

    BOOL JavascriptRegExpConstructor::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(JS_DIAG_TYPE_JavascriptRegExpConstructor);
        return TRUE;
    }

    BOOL JavascriptRegExpConstructor::IsEnumerable(PropertyId propertyId)
    {
        switch (propertyId)
        {
        case PropertyIds::input:
        case PropertyIds::$1:
        case PropertyIds::$2:
        case PropertyIds::$3:
        case PropertyIds::$4:
        case PropertyIds::$5:
        case PropertyIds::$6:
        case PropertyIds::$7:
        case PropertyIds::$8:
        case PropertyIds::$9:
        case PropertyIds::leftContext:
        case PropertyIds::rightContext:
        case PropertyIds::lastMatch:
        case PropertyIds::lastParen:
            return true;
        case PropertyIds::$_:
        case PropertyIds::$Ampersand:
        case PropertyIds::$Plus:
        case PropertyIds::$BackTick:
        case PropertyIds::$Tick:
        case PropertyIds::index:
            return false;
        default:
            return JavascriptFunction::IsEnumerable(propertyId);
        }
    }

    BOOL JavascriptRegExpConstructor::IsConfigurable(PropertyId propertyId)
    {
        switch (propertyId)
        {
        case PropertyIds::input:
        case PropertyIds::$_:

        case PropertyIds::lastMatch:
        case PropertyIds::$Ampersand:

        case PropertyIds::lastParen:
        case PropertyIds::$Plus:

        case PropertyIds::leftContext:
        case PropertyIds::$BackTick:

        case PropertyIds::rightContext:
        case PropertyIds::$Tick:

        case PropertyIds::$1:
        case PropertyIds::$2:
        case PropertyIds::$3:
        case PropertyIds::$4:
        case PropertyIds::$5:
        case PropertyIds::$6:
        case PropertyIds::$7:
        case PropertyIds::$8:
        case PropertyIds::$9:
        case PropertyIds::index:
            return false;
        default:
            return JavascriptFunction::IsConfigurable(propertyId);
        }
    }

    BOOL JavascriptRegExpConstructor::IsWritable(PropertyId propertyId)
    {
        uint32 mask = GetSpecialFreezeMask(propertyId);
        if (mask != 0)
        {
            return (frozenSpecialMask & mask) == 0;
        }
        return JavascriptFunction::IsWritable(propertyId);
    }

    BOOL JavascriptRegExpConstructor::SetWritable(PropertyId propertyId, BOOL value)
    {
        uint32 mask = GetSpecialFreezeMask(propertyId);
        if (mask != 0)
        {
            if (!value)
            {
                this->EnsureValues();
                if ((mask & FreezeBit_Input) && !(frozenSpecialMask & FreezeBit_Input))
                {
                    this->frozenLastInput = this->lastInput;
                }
                frozenSpecialMask |= mask;
                return TRUE;
            }
            return (frozenSpecialMask & mask) == 0;
        }
        return JavascriptFunction::SetWritable(propertyId, value);
    }

    BOOL JavascriptRegExpConstructor::SetPropertyWithAttributes(PropertyId propertyId, Var value, PropertyAttributes attributes, PropertyValueInfo* info, PropertyOperationFlags flags, SideEffects possibleSideEffects)
    {
        uint32 mask = GetSpecialFreezeMask(propertyId);
        if (mask != 0)
        {
            this->EnsureValues();
            if ((mask & FreezeBit_Input) && !(frozenSpecialMask & FreezeBit_Input))
            {
                this->lastInput = JavascriptConversion::ToString(value, this->GetScriptContext());
            }
            if (!(attributes & PropertyWritable))
            {
                if ((mask & FreezeBit_Input) && !(frozenSpecialMask & FreezeBit_Input))
                {
                    this->frozenLastInput = this->lastInput;
                }
                frozenSpecialMask |= mask;
            }
            PropertyValueInfo::SetNoCache(info, this);
            return TRUE;
        }
        return JavascriptFunction::SetPropertyWithAttributes(propertyId, value, attributes, info, flags, possibleSideEffects);
    }

    BOOL JavascriptRegExpConstructor::GetSpecialNonEnumerablePropertyName(uint32 index, Var *propertyName, ScriptContext * requestContext)
    {
        uint length = GetSpecialNonEnumerablePropertyCount();
        if (index < length)
        {
            *propertyName = requestContext->GetPropertyString(specialnonEnumPropertyIds[index]);
            return true;
        }
        return false;
    }

    // Returns the number of special non-enumerable properties this type has.
    uint JavascriptRegExpConstructor::GetSpecialNonEnumerablePropertyCount() const
    {
        return _countof(specialnonEnumPropertyIds);
    }

    // Returns the list of special properties for the type.
    PropertyId const * JavascriptRegExpConstructor::GetSpecialNonEnumerablePropertyIds() const
    {
        return specialnonEnumPropertyIds;
    }


    BOOL JavascriptRegExpConstructor::GetSpecialEnumerablePropertyName(uint32 index, JavascriptString ** propertyName, ScriptContext * requestContext)
    {
        uint length = GetSpecialEnumerablePropertyCount();
        if (index < length)
        {
            *propertyName = requestContext->GetPropertyString(specialEnumPropertyIds[index]);
            return true;
        }
        return false;
    }

    PropertyId const * JavascriptRegExpConstructor::GetSpecialEnumerablePropertyIds() const
    {
        return specialEnumPropertyIds;
    }

    // Returns the number of special non-enumerable properties this type has.
    uint JavascriptRegExpConstructor::GetSpecialEnumerablePropertyCount() const
    {
        return _countof(specialEnumPropertyIds);
    }

    // Returns the list of special properties for the type.
    PropertyId const * JavascriptRegExpConstructor::GetSpecialPropertyIds() const
    {
        return specialPropertyIds;
    }

    uint JavascriptRegExpConstructor::GetSpecialPropertyCount() const
    {
        return _countof(specialPropertyIds);
    }

    BOOL JavascriptRegExpConstructor::GetSpecialPropertyName(uint32 index, JavascriptString ** propertyName, ScriptContext * requestContext)
    {
        uint length = GetSpecialPropertyCount();
        if (index < length)
        {
            *propertyName = requestContext->GetPropertyString(specialPropertyIds[index]);
            return true;
        }
        return false;
    }

} // namespace Js
