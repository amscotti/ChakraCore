//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "JSON.h"
#include "JSONParser.h"


using namespace Js;

namespace JSON
{
    // -------- Parser implementation ------------//
    void JSONParser::Finalizer()
    {
        m_scanner.Finalizer();
        if(arenaAllocatorObject)
        {
            this->scriptContext->ReleaseTemporaryGuestAllocator(arenaAllocatorObject);
        }
    }

    Js::Var JSONParser::Parse(LPCWSTR str, uint length)
    {
        if (length > MIN_CACHE_LENGTH)
        {
            if (!this->arenaAllocatorObject)
            {
                this->arenaAllocatorObject = scriptContext->GetTemporaryGuestAllocator(_u("JSONParse"));
                this->arenaAllocator = arenaAllocatorObject->GetAllocator();
            }
        }
        m_scanner.Init(str, length, &m_token, scriptContext, str, this->arenaAllocator);
        Scan();
        Js::Var ret = ParseObject();
        if (m_token.tk != tkEOF)
        {
            m_scanner.ThrowSyntaxError(JSERR_JsonSyntax);
        }
        return ret;
    }

    Js::Var JSONParser::Parse(Js::JavascriptString* input)
    {
        return Parse(input->GetSz(), input->GetLength());
    }

    /*
     * Memory ownership record (doc/memory-management.md section 3) for JSONParser::Walk / WalkProperty:
     * - Resource / creation call: Recycler-managed JS values (holder, val, newElement, propName,
     *   reviver), enumerable key snapshot array (keys = GetOwnEnumerablePropertyNames), and interned
     *   PropertyRecord* instances (scriptContext->GetOrAddPropertyRecord).
     * - Domain and allocator: ChakraCore Recycler GC heap (conservative stack scan); no untraced
     *   native C++ heap/arena arrays store Var or object pointers across WalkProperty / reviver calls.
     * - Physical allocation / release: Recycler owns allocation and collection of JS objects, strings,
     *   keys arrays, and finalizable PropertyRecord entries; C++ stack frames hold no owning native buffers.
     * - Logical views: uint64 array length len is captured once per array walk; keys snapshot array
     *   is never exposed to JS user code.
     * - Owner / transfer / aliases: Each recursive WalkProperty stack frame holds stack-scanned locals
     *   (holder, val, keys, keyStr, keyRecord, newElement) across child walks and reviver calls.
     *   Keeping keyStr and keyRecord live across WalkProperty prevents PropertyRecord finalization on GC.
     * - Borrow invalidation: Reentrant reviver/getter/Proxy traps can run GC, mutate elements/properties,
     *   change array length, freeze/seal/preventExtensions, convert JavascriptArray to ES5Array or
     *   CopyOnAccessNativeIntArray, or revoke Proxies. Non-ES5 array fast-path is re-checked after
     *   WalkProperty returns, and CopyOnAccess arrays are converted before DeleteItem/DefineOwnProperty.
     * - Reentrancy points: GetItem/GetProperty, IsArray (revoked Proxy), OP_GetLength + ToLength,
     *   GetOwnEnumerablePropertyNames (ownKeys / getOwnPropertyDescriptor), WalkProperty recursion,
     *   DeleteItem/DeleteProperty, DefineOwnPropertyHelper (defineProperty), and CallFunction(reviver).
     * - Thread/context: Bound script thread with active ScriptContext (BEGIN_SAFE_REENTRANT_CALL for CallFunction).
     * - Release / exit: Stack frames unwind normally on return or JS exception; Recycler reclaims
     *   unreferenced wrapper/keys objects and PropertyRecords on subsequent GC cycles.
     * - Bounds / exhaustion: PROBE_STACK guards recursion depth; len is bounded by ES2025 ToLength
     *   ([0, 2^53 - 1]) using uint64 indices so indices >= InvalidIndex use GetProperty/DeleteProperty.
     */
    static Js::JavascriptString* UInt64ToPropertyString(uint64 index, Js::ScriptContext* scriptContext)
    {
        if (index <= UINT_MAX)
        {
            return scriptContext->GetIntegerString(static_cast<uint>(index));
        }
        char16 buffer[24];
        ::_ui64tow_s(index, buffer, _countof(buffer), 10);
        return Js::JavascriptString::NewCopyBuffer(buffer, Js::JavascriptString::GetBufferLength(buffer), scriptContext);
    }

    static bool IsArrayOrThrowIfRevoked(Js::RecyclableObject* valObj, Js::ScriptContext* scriptContext)
    {
        Js::RecyclableObject* cur = valObj;
        while (Js::VarIs<Js::JavascriptProxy>(cur))
        {
            Js::JavascriptProxy* proxy = Js::VarTo<Js::JavascriptProxy>(cur);
            if (proxy->IsRevoked())
            {
                // Throw revoked-proxy TypeError in the caller's realm per ES2025 7.2.2 IsArray step 3a.
                Js::JavascriptError::ThrowTypeError(scriptContext, JSERR_ErrorOnRevokedProxy, _u(""));
            }
            cur = proxy->GetTarget();
        }
        return Js::JavascriptOperators::IsArray(cur);
    }

    static void CreateDataProperty(Js::RecyclableObject* obj, Js::PropertyId propId, Js::Var value, Js::ScriptContext* scriptContext)
    {
#if ENABLE_COPYONACCESS_ARRAY
        Js::JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Js::Var>(obj);
#endif
        Js::PropertyDescriptor desc;
        desc.SetValue(value);
        desc.SetWritable(true);
        desc.SetEnumerable(true);
        desc.SetConfigurable(true);
        Js::JavascriptObject::DefineOwnPropertyHelper(obj, propId, desc, scriptContext, false);
    }

    Js::Var JSONParser::Walk(Js::JavascriptString* name, Js::PropertyId id, Js::Var holder, uint32 index)
    {
        AssertMsg(reviver, "JSON post parse walk with null reviver");
        AssertMsg(Js::JavascriptOperators::IsObject(holder),
            "The holder argument in a JSON::Walk function must be an object or an array");
        Js::RecyclableObject* holderObj = Js::VarTo<Js::RecyclableObject>(holder);
        bool hasIndex = (index != Js::JavascriptArray::InvalidIndex);
        return WalkProperty(name, holderObj, id, index, hasIndex);
    }

    Js::Var JSONParser::WalkProperty(Js::JavascriptString* name, Js::RecyclableObject* holder, Js::PropertyId id, uint64 index, bool hasIndex)
    {
        AssertMsg(reviver, "JSON post parse walk with null reviver");
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);

        // ES2025 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver )
        // 1. Let val be ? Get(holder, name).
        Js::Var val = nullptr;
        if (hasIndex)
        {
            if (!Js::JavascriptOperators::GetItem(holder, index, &val, scriptContext))
            {
                val = scriptContext->GetLibrary()->GetUndefined();
            }
        }
        else
        {
            Assert(id != Js::Constants::NoProperty);
            if (!Js::JavascriptOperators::GetProperty(holder, id, &val, scriptContext, nullptr))
            {
                val = scriptContext->GetLibrary()->GetUndefined();
            }
        }

        // 2. If val is an Object, then
        if (Js::JavascriptOperators::IsObject(val))
        {
            Js::RecyclableObject* valObj = Js::VarTo<Js::RecyclableObject>(val);
            // a. Let isArray be ? IsArray(val).
            if (IsArrayOrThrowIfRevoked(valObj, scriptContext))
            {
                // b. If isArray is true, then
                //    i. Let len be ? LengthOfArrayLike(val).
                uint64 len = static_cast<uint64>(
                    Js::JavascriptConversion::ToLength(
                        Js::JavascriptOperators::OP_GetLength(valObj, scriptContext),
                        scriptContext));

                // ii. Let I be 0. iii. Repeat, while I < len,
                for (uint64 i = 0; i < len; i++)
                {
                    Js::JavascriptString* propName = UInt64ToPropertyString(i, scriptContext);
                    Js::Var newElement = WalkProperty(propName, valObj, Js::Constants::NoProperty, i, true);
                    if (Js::JavascriptOperators::IsUndefined(newElement))
                    {
#if ENABLE_COPYONACCESS_ARRAY
                        Js::JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Js::Var>(valObj);
#endif
                        if (i < Js::JavascriptArray::InvalidIndex)
                        {
                            Js::JavascriptOperators::DeleteItem(valObj, static_cast<uint32>(i), Js::PropertyOperation_None);
                        }
                        else
                        {
                            Js::JavascriptOperators::DeleteItem(valObj, i, Js::PropertyOperation_None);
                        }
                    }
                    else
                    {
#if ENABLE_COPYONACCESS_ARRAY
                        Js::JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Js::Var>(valObj);
#endif
                        Js::JavascriptArray* nonES5Array = Js::JavascriptArray::TryVarToNonES5Array(valObj);
                        if (nonES5Array != nullptr && !nonES5Array->IsCrossSiteObject() && i < Js::JavascriptArray::InvalidIndex)
                        {
                            nonES5Array->SetItem(static_cast<uint32>(i), newElement, Js::PropertyOperation_None);
                        }
                        else
                        {
                            const Js::PropertyRecord* elemRecord = nullptr;
                            scriptContext->GetOrAddPropertyRecord(propName, &elemRecord);
                            CreateDataProperty(valObj, elemRecord->GetPropertyId(), newElement, scriptContext);
                        }
                    }
                }
            }
            else
            {
                // c. Else,
                //    i. Let keys be ? EnumerableOwnProperties(val, KEY).
                Js::JavascriptArray* keys = Js::JavascriptOperators::GetOwnEnumerablePropertyNames(valObj, scriptContext);
                uint32 keyCount = keys->GetLength();
                for (uint32 k = 0; k < keyCount; k++)
                {
                    Js::Var keyVar = keys->DirectGetItem(k);
                    Assert(Js::VarIs<Js::JavascriptString>(keyVar));
                    Js::JavascriptString* keyStr = Js::UnsafeVarTo<Js::JavascriptString>(keyVar);
                    const Js::PropertyRecord* keyRecord = nullptr;
                    scriptContext->GetOrAddPropertyRecord(keyStr, &keyRecord);

                    if (keyRecord->IsNumeric())
                    {
                        uint32 propIndex = keyRecord->GetNumericValue();
                        Js::Var newElement = WalkProperty(keyStr, valObj, Js::Constants::NoProperty, propIndex, true);
                        if (Js::JavascriptOperators::IsUndefined(newElement))
                        {
#if ENABLE_COPYONACCESS_ARRAY
                            Js::JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Js::Var>(valObj);
#endif
                            Js::JavascriptOperators::DeleteItem(valObj, propIndex, Js::PropertyOperation_None);
                        }
                        else
                        {
                            CreateDataProperty(valObj, keyRecord->GetPropertyId(), newElement, scriptContext);
                        }
                    }
                    else
                    {
                        Js::Var newElement = WalkProperty(keyStr, valObj, keyRecord->GetPropertyId(), 0, false);
                        if (Js::JavascriptOperators::IsUndefined(newElement))
                        {
                            Js::JavascriptOperators::DeleteProperty(valObj, keyRecord->GetPropertyId(), Js::PropertyOperation_None);
                        }
                        else
                        {
                            CreateDataProperty(valObj, keyRecord->GetPropertyId(), newElement, scriptContext);
                        }
                    }
                }
            }
        }

        // 3. Return ? Call(reviver, holder, << name, val >>).
        Js::Var values[3];
        Js::Arguments args(0, values);
        args.Info.Count = 3;
        args.Values[0] = holder;
        args.Values[1] = name;
        args.Values[2] = val;
        BEGIN_SAFE_REENTRANT_CALL(reviver->GetScriptContext()->GetThreadContext())
        {
            val = Js::JavascriptFunction::CallFunction<true>(reviver, reviver->GetEntryPoint(), args);
        }
        END_SAFE_REENTRANT_CALL
        return val;
    }

    Js::Var JSONParser::ParseObject()
    {
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);

        Js::Var retVal;

        switch (m_token.tk)
        {

        case tkFltCon:
            retVal = Js::JavascriptNumber::ToVarIntCheck(m_token.GetDouble(), scriptContext);
            Scan();
            return retVal;

        case tkStrCon:
            {
                // will auto-null-terminate the string (as length=len+1)
                uint len = m_scanner.GetCurrentStringLen();
                retVal = Js::JavascriptString::NewCopyBuffer(m_scanner.GetCurrentString(), len, scriptContext);
                Scan();
                return retVal;
            }

        case tkTRUE:
            retVal = scriptContext->GetLibrary()->GetTrue();
            Scan();
            return retVal;

        case tkFALSE:
            retVal = scriptContext->GetLibrary()->GetFalse();
            Scan();
            return retVal;

        case tkNULL:
            retVal = scriptContext->GetLibrary()->GetNull();
            Scan();
            return retVal;

        case tkSub:  // unary minus

            if (Scan() == tkFltCon)
            {
                retVal = Js::JavascriptNumber::ToVarIntCheck(-m_token.GetDouble(), scriptContext);
                Scan();
                return retVal;
            }
            else
            {
                m_scanner.ThrowSyntaxError(JSERR_JsonBadNumber);
            }

        case tkLBrack:
            {

                Js::JavascriptArray* arrayObj = scriptContext->GetLibrary()->CreateArray(0);

                //skip '['
                Scan();

                //iterate over the array members, get JSON objects and add them in the pArrayMemberList
                uint k = 0;
                while (true)
                {
                    if(tkRBrack == m_token.tk)
                    {
                        break;
                    }
                    Js::Var value = ParseObject();
                    arrayObj->SetItem(k++, value, Js::PropertyOperation_None);

                    // if next token is not a comma consider the end of the array member list.
                    if (tkComma != m_token.tk)
                        break;
                    Scan();
                    if(tkRBrack == m_token.tk)
                    {
                        m_scanner.ThrowSyntaxError(JSERR_JsonIllegalChar);
                    }
                }
                //check and consume the ending ']'
                CheckCurrentToken(tkRBrack, JSERR_JsonNoRbrack);
                return arrayObj;

            }

        case tkLCurly:
            {

                // Parse an object, "{"name1" : ObjMember1, "name2" : ObjMember2, ...} "
                if(IsCaching())
                {
                    if(!typeCacheList)
                    {
                        typeCacheList = Anew(this->arenaAllocator, JsonTypeCacheList, this->arenaAllocator, 8);
                    }
                }

                // first, create the object
                Js::DynamicObject* object = scriptContext->GetLibrary()->CreateObject();
                JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(object));
#if ENABLE_DEBUG_CONFIG_OPTIONS
                if (Js::Configuration::Global.flags.IsEnabled(Js::autoProxyFlag))
                {
                    object = VarTo<DynamicObject>(JavascriptProxy::AutoProxyWrapper(object));
                }
#endif

                //next token after '{'
                Scan();

                //if empty object "{}" return;
                if(tkRCurly == m_token.tk)
                {
                    Scan();
                    return object;
                }
                JsonTypeCache* previousCache = nullptr;
                JsonTypeCache* currentCache = nullptr;
                //parse the list of members
                while(true)
                {
                    // parse a list member:  "name" : ObjMember
                    // and add it to the object.

                    //pick "name"
                    if(tkStrCon != m_token.tk)
                    {
                        m_scanner.ThrowSyntaxError(JSERR_JsonIllegalChar);
                    }

                    // currentStrLength = length w/o null-termination
                    WCHAR* currentStr = m_scanner.GetCurrentString();
                    uint currentStrLength = m_scanner.GetCurrentStringLen();

                    DynamicType* typeWithoutProperty = object->GetDynamicType();
                    if(IsCaching())
                    {
                        if(!previousCache)
                        {
                            // This is the first property in the list - see if we have an existing cache for it.
                            currentCache = typeCacheList->LookupWithKey(Js::HashedCharacterBuffer<WCHAR>(currentStr, currentStrLength), nullptr);
                        }
                        if(currentCache && currentCache->typeWithoutProperty == typeWithoutProperty &&
                            currentCache->propertyRecord->Equals(JsUtil::CharacterBuffer<WCHAR>(currentStr, currentStrLength)))
                        {
                            //check and consume ":"
                            if(Scan() != tkColon )
                            {
                                m_scanner.ThrowSyntaxError(JSERR_JsonNoColon);
                            }
                            Scan();

                            // Cache all values from currentCache as there is a chance that ParseObject might change the cache
                            DynamicType* typeWithProperty = currentCache->typeWithProperty;
                            PropertyId propertyId = currentCache->propertyRecord->GetPropertyId();
                            PropertyIndex propertyIndex = currentCache->propertyIndex;
                            previousCache = currentCache;
                            currentCache = currentCache->next;

                            // fast path for type transition and property set
                            object->EnsureSlots(typeWithoutProperty->GetTypeHandler()->GetSlotCapacity(),
                                typeWithProperty->GetTypeHandler()->GetSlotCapacity(), scriptContext, typeWithProperty->GetTypeHandler());
                            object->ReplaceType(typeWithProperty);
                            Js::Var value = ParseObject();
                            object->SetSlot(SetSlotArguments(propertyId, propertyIndex, value));

                            // if the next token is not a comma consider the list of members done.
                            if (tkComma != m_token.tk)
                                break;
                            Scan();
                            continue;
                        }
                    }

                    // slow path
                    Js::PropertyRecord const * propertyRecord;
                    scriptContext->GetOrAddPropertyRecord(currentStr, currentStrLength, &propertyRecord);

                    //check and consume ":"
                    if(Scan() != tkColon )
                    {
                        m_scanner.ThrowSyntaxError(JSERR_JsonNoColon);
                    }
                    Scan();
                    Js::Var value = ParseObject();
                    PropertyValueInfo info;
                    object->SetProperty(propertyRecord->GetPropertyId(), value, PropertyOperation_None, &info);

                    DynamicType* typeWithProperty = object->GetDynamicType();
                    if(IsCaching() && !propertyRecord->IsNumeric() && !info.IsNoCache() && typeWithProperty->GetIsShared() && typeWithProperty->GetTypeHandler()->IsPathTypeHandler())
                    {
                        PropertyIndex propertyIndex = info.GetPropertyIndex();

                        if(!previousCache)
                        {
                            // This is the first property in the set add it to the dictionary.
                            currentCache = JsonTypeCache::New(this->arenaAllocator, propertyRecord, typeWithoutProperty, typeWithProperty, propertyIndex);
                            typeCacheList->AddNew(propertyRecord, currentCache);
                        }
                        else if(!currentCache)
                        {
                            currentCache = JsonTypeCache::New(this->arenaAllocator, propertyRecord, typeWithoutProperty, typeWithProperty, propertyIndex);
                            previousCache->next = currentCache;
                        }
                        else
                        {
                            // cache miss!!
                            currentCache->Update(propertyRecord, typeWithoutProperty, typeWithProperty, propertyIndex);
                        }
                        previousCache = currentCache;
                        currentCache = currentCache->next;
                    }

                    // if the next token is not a comma consider the list of members done.
                    if (tkComma != m_token.tk)
                        break;
                    Scan();
                }

                // check  and consume the ending '}"
                CheckCurrentToken(tkRCurly, JSERR_JsonNoRcurly);
                return object;
            }

        default:
            m_scanner.ThrowSyntaxError(JSERR_JsonSyntax);
        }
    }
} // namespace JSON
