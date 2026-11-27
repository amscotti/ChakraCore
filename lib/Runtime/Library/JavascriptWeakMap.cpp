//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    JavascriptWeakMap::JavascriptWeakMap(DynamicType* type)
        : DynamicObject(type),
        keySet(type->GetScriptContext()->GetRecycler()),
        symbolMap(nullptr)
    {
    }

    void JavascriptWeakMap::EnsureSymbolMap()
    {
        if (symbolMap == nullptr)
        {
            Recycler* recycler = GetScriptContext()->GetRecycler();
            symbolMap = RecyclerNew(recycler, SymbolMap, recycler);
        }
    }

    JavascriptWeakMap::WeakMapKeyMap* JavascriptWeakMap::GetWeakMapKeyMapFromKey(RecyclableObject* key) const
    {
        AssertOrFailFast(DynamicType::Is(key->GetTypeId()) || JavascriptOperators::GetTypeId(key) == TypeIds_HostDispatch);

        Var weakMapKeyData = nullptr;

        if (!key->GetInternalProperty(key, InternalPropertyIds::WeakMapKeyMap, &weakMapKeyData, nullptr, key->GetScriptContext()))
        {
            return nullptr;
        }

        if (key->GetLibrary()->GetUndefined() == weakMapKeyData)
        {
            return nullptr;
        }

        return static_cast<WeakMapKeyMap*>(weakMapKeyData);
    }

    JavascriptWeakMap::WeakMapKeyMap* JavascriptWeakMap::AddWeakMapKeyMapToKey(RecyclableObject* key)
    {
        AssertOrFailFast(DynamicType::Is(key->GetTypeId()) || JavascriptOperators::GetTypeId(key) == TypeIds_HostDispatch);

        // The internal property may exist on an object that has had DynamicObject::ResetObject called on itself.
        // In that case the value stored in the property slot should be null.
        DebugOnly(Var unused = nullptr);
        Assert(!key->GetInternalProperty(key, InternalPropertyIds::WeakMapKeyMap, &unused, nullptr, key->GetScriptContext()) || unused == nullptr);

        WeakMapKeyMap* weakMapKeyData = RecyclerNew(GetScriptContext()->GetRecycler(), WeakMapKeyMap, GetScriptContext()->GetRecycler());
        BOOL success = key->SetInternalProperty(InternalPropertyIds::WeakMapKeyMap, weakMapKeyData, PropertyOperation_Force, nullptr);
        Assert(success);

        return weakMapKeyData;
    }

    bool JavascriptWeakMap::KeyMapGet(WeakMapKeyMap* map, Var* value) const
    {
        if (map->ContainsKey(GetWeakMapId()))
        {
            *value = map->Item(GetWeakMapId());
            return true;
        }

        return false;
    }

    Var JavascriptWeakMap::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);
        CHAKRATEL_LANGSTATS_INC_LANGFEATURECOUNT(ES6, WeakMap, scriptContext);

        JavascriptWeakMap* weakMapObject = nullptr;

        if (callInfo.Flags & CallFlags_New)
        {
            weakMapObject = library->CreateWeakMap();
        }
        else
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakMap"), _u("WeakMap"));
        }
        Assert(weakMapObject != nullptr);

        if (isCtorSuperCall)
        {
            weakMapObject = VarTo<JavascriptWeakMap>(JavascriptOperators::OrdinaryCreateFromConstructor(
                VarTo<RecyclableObject>(newTarget), weakMapObject, nullptr, scriptContext));
        }

        Var iterable = (args.Info.Count > 1) ? args[1] : library->GetUndefined();

        RecyclableObject* iter = nullptr;
        RecyclableObject* adder = nullptr;

        if (JavascriptConversion::CheckObjectCoercible(iterable, scriptContext))
        {
            Var adderVar = JavascriptOperators::GetPropertyNoCache(weakMapObject, PropertyIds::set, scriptContext);
            if (!JavascriptConversion::IsCallable(adderVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
            }
            adder = VarTo<RecyclableObject>(adderVar);
            iter = JavascriptOperators::GetIterator(iterable, scriptContext);
        }

        if (iter != nullptr)
        {
            Var undefined = library->GetUndefined();

            JavascriptOperators::DoIteratorStepAndValue(iter, scriptContext, [&](Var nextItem) {
                if (!JavascriptOperators::IsObject(nextItem))
                {
                    JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
                }

                RecyclableObject* obj = VarTo<RecyclableObject>(nextItem);

                Var key = nullptr, value = nullptr;

                if (!JavascriptOperators::GetItem(obj, 0u, &key, scriptContext))
                {
                    key = undefined;
                }

                if (!JavascriptOperators::GetItem(obj, 1u, &value, scriptContext))
                {
                    value = undefined;
                }

                BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
                {
                    CALL_FUNCTION(scriptContext->GetThreadContext(), adder, CallInfo(CallFlags_Value, 3), weakMapObject, key, value);
                }
                END_SAFE_REENTRANT_CALL
            });
        }

#if ENABLE_TTD
        //TODO: right now we always GC before snapshots (assuming we have a weak collection)
        //      may want to optimize this and use a notify here that we have a weak container -- also update post inflate and post snap
#endif

        return weakMapObject;
    }

    Var JavascriptWeakMap::EntryDelete(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        if (!VarIs<JavascriptWeakMap>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakMap.prototype.delete"), _u("WeakMap"));
        }

        JavascriptWeakMap* weakMap = VarTo<JavascriptWeakMap>(args[0]);

        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        bool didDelete = false;

        if (VarIs<JavascriptSymbol>(key) && !VarTo<JavascriptSymbol>(key)->IsRegistered())
        {
            didDelete = weakMap->Delete(VarTo<JavascriptSymbol>(key));
        }
        else if (JavascriptOperators::IsObject(key))
        {
            RecyclableObject* keyObj = VarTo<RecyclableObject>(key);

            didDelete = weakMap->Delete(keyObj);
        }

#if ENABLE_TTD
        if(scriptContext->IsTTDRecordOrReplayModeEnabled())
        {
            if(scriptContext->IsTTDRecordModeEnabled())
            {
                function->GetScriptContext()->GetThreadContext()->TTDLog->RecordWeakCollectionContainsEvent(didDelete);
            }
            else
            {
                didDelete = function->GetScriptContext()->GetThreadContext()->TTDLog->ReplayWeakCollectionContainsEvent();
            }
        }
#endif

        return scriptContext->GetLibrary()->CreateBoolean(didDelete);
    }

    Var JavascriptWeakMap::EntryGet(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        if (!VarIs<JavascriptWeakMap>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakMap.prototype.get"), _u("WeakMap"));
        }

        JavascriptWeakMap* weakMap = VarTo<JavascriptWeakMap>(args[0]);

        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();

        bool loaded = false;
        Var value = nullptr;
        if (VarIs<JavascriptSymbol>(key) && !VarTo<JavascriptSymbol>(key)->IsRegistered())
        {
            loaded = weakMap->Get(VarTo<JavascriptSymbol>(key), &value);
        }
        else if (JavascriptOperators::IsObject(key))
        {
            RecyclableObject* keyObj = VarTo<RecyclableObject>(key);
            loaded = weakMap->Get(keyObj, &value);
        }

#if ENABLE_TTD
        if(scriptContext->IsTTDRecordOrReplayModeEnabled())
        {
            if(scriptContext->IsTTDRecordModeEnabled())
            {
                function->GetScriptContext()->GetThreadContext()->TTDLog->RecordWeakCollectionContainsEvent(loaded);
            }
            else
            {
                loaded = function->GetScriptContext()->GetThreadContext()->TTDLog->ReplayWeakCollectionContainsEvent();
            }
        }
#endif

        return loaded ? CrossSite::MarshalVar(scriptContext, value) : scriptContext->GetLibrary()->GetUndefined();
    }

    Var JavascriptWeakMap::EntryHas(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        if (!VarIs<JavascriptWeakMap>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakMap.prototype.has"), _u("WeakMap"));
        }

        JavascriptWeakMap* weakMap = VarTo<JavascriptWeakMap>(args[0]);

        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        bool hasValue = false;

        if (VarIs<JavascriptSymbol>(key) && !VarTo<JavascriptSymbol>(key)->IsRegistered())
        {
            hasValue = weakMap->Has(VarTo<JavascriptSymbol>(key));
        }
        else if (JavascriptOperators::IsObject(key))
        {
            RecyclableObject* keyObj = VarTo<RecyclableObject>(key);

            hasValue = weakMap->Has(keyObj);
        }

#if ENABLE_TTD
        if(scriptContext->IsTTDRecordOrReplayModeEnabled())
        {
            if(scriptContext->IsTTDRecordModeEnabled())
            {
                function->GetScriptContext()->GetThreadContext()->TTDLog->RecordWeakCollectionContainsEvent(hasValue);
            }
            else
            {
                hasValue = function->GetScriptContext()->GetThreadContext()->TTDLog->ReplayWeakCollectionContainsEvent();
            }
        }
#endif

        return scriptContext->GetLibrary()->CreateBoolean(hasValue);
    }

    Var JavascriptWeakMap::EntrySet(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        if (!VarIs<JavascriptWeakMap>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakMap.prototype.set"), _u("WeakMap"));
        }

        JavascriptWeakMap* weakMap = VarTo<JavascriptWeakMap>(args[0]);

        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        Var value = (args.Info.Count > 2) ? args[2] : scriptContext->GetLibrary()->GetUndefined();

        if (VarIs<JavascriptSymbol>(key) && !VarTo<JavascriptSymbol>(key)->IsRegistered())
        {
            weakMap->Set(VarTo<JavascriptSymbol>(key), value);
            return weakMap;
        }

        if (!JavascriptOperators::IsObject(key))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_WeakMapSetKeyNotAnObject, _u("WeakMap.prototype.set"));
        }

        RecyclableObject* keyObj = VarTo<RecyclableObject>(key);

#if ENABLE_TTD
        //In replay we need to pin the object (and will release at snapshot points) -- in record we don't need to do anything
        if(scriptContext->IsTTDReplayModeEnabled())
        {
            scriptContext->TTDContextInfo->TTDWeakReferencePinSet->AddNew(keyObj);
        }
#endif

        weakMap->Set(keyObj, value);

        return weakMap;
    }

    static JavascriptWeakMap* RequireWeakMapReceiver(const Arguments& args, PCWSTR methodName, ScriptContext* scriptContext)
    {
        if (args.Info.Count == 0 || !VarIs<JavascriptWeakMap>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, methodName, _u("WeakMap"));
        }
        return VarTo<JavascriptWeakMap>(args[0]);
    }

    static bool TryExtractWeakMapKey(Var key, JavascriptSymbol** keySymOut, RecyclableObject** keyObjOut)
    {
        if (VarIs<JavascriptSymbol>(key) && !VarTo<JavascriptSymbol>(key)->IsRegistered())
        {
            *keySymOut = VarTo<JavascriptSymbol>(key);
            *keyObjOut = nullptr;
            return true;
        }
        if (JavascriptOperators::IsObject(key))
        {
            *keySymOut = nullptr;
            *keyObjOut = VarTo<RecyclableObject>(key);
            return true;
        }
        return false;
    }

    static bool SyncWeakMapContainsTTD(ScriptContext* scriptContext, bool loaded)
    {
#if ENABLE_TTD
        if (scriptContext->IsTTDRecordOrReplayModeEnabled())
        {
            if (scriptContext->IsTTDRecordModeEnabled())
            {
                scriptContext->GetThreadContext()->TTDLog->RecordWeakCollectionContainsEvent(loaded);
            }
            else
            {
                loaded = scriptContext->GetThreadContext()->TTDLog->ReplayWeakCollectionContainsEvent();
            }
        }
#else
        (void)scriptContext;
#endif
        return loaded;
    }

    static bool WeakMapGetEntry(
        JavascriptWeakMap* weakMap,
        ScriptContext* scriptContext,
        JavascriptSymbol* keySym,
        RecyclableObject* keyObj,
        Var* existing)
    {
        bool loaded = (keySym != nullptr) ? weakMap->Get(keySym, existing) : weakMap->Get(keyObj, existing);
        return SyncWeakMapContainsTTD(scriptContext, loaded);
    }

    static void WeakMapSetEntry(
        JavascriptWeakMap* weakMap,
        ScriptContext* scriptContext,
        JavascriptSymbol* keySym,
        RecyclableObject* keyObj,
        Var value)
    {
        if (keySym != nullptr)
        {
            weakMap->Set(keySym, value);
            return;
        }

#if ENABLE_TTD
        // In replay we need to pin the object (and will release at snapshot points)
        if (scriptContext->IsTTDReplayModeEnabled())
        {
            scriptContext->TTDContextInfo->TTDWeakReferencePinSet->AddNew(keyObj);
        }
#else
        (void)scriptContext;
#endif
        weakMap->Set(keyObj, value);
    }

    Var JavascriptWeakMap::EntryGetOrInsert(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptWeakMap* weakMap = RequireWeakMapReceiver(args, _u("WeakMap.prototype.getOrInsert"), scriptContext);
        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        Var value = (args.Info.Count > 2) ? args[2] : scriptContext->GetLibrary()->GetUndefined();

        JavascriptSymbol* keySym = nullptr;
        RecyclableObject* keyObj = nullptr;
        if (!TryExtractWeakMapKey(key, &keySym, &keyObj))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_WeakMapSetKeyNotAnObject, _u("WeakMap.prototype.getOrInsert"));
        }

        Var existing = nullptr;
        if (WeakMapGetEntry(weakMap, scriptContext, keySym, keyObj, &existing))
        {
            return CrossSite::MarshalVar(scriptContext, existing);
        }

        WeakMapSetEntry(weakMap, scriptContext, keySym, keyObj, value);
        return CrossSite::MarshalVar(scriptContext, value);
    }

    Var JavascriptWeakMap::EntryGetOrInsertComputed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("WeakMap.prototype.getOrInsertComputed"));
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptWeakMap* weakMap = RequireWeakMapReceiver(args, _u("WeakMap.prototype.getOrInsertComputed"), scriptContext);

        // Check IsCallable(callbackfn) before CanBeHeldWeakly(key) (ES2026 steps 3-4).
        if (args.Info.Count < 3 || !JavascriptConversion::IsCallable(args[2]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("WeakMap.prototype.getOrInsertComputed"));
        }
        RecyclableObject* callbackfn = VarTo<RecyclableObject>(args[2]);

        Var key = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptSymbol* keySym = nullptr;
        RecyclableObject* keyObj = nullptr;
        if (!TryExtractWeakMapKey(key, &keySym, &keyObj))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_WeakMapSetKeyNotAnObject, _u("WeakMap.prototype.getOrInsertComputed"));
        }

        Var existing = nullptr;
        if (WeakMapGetEntry(weakMap, scriptContext, keySym, keyObj, &existing))
        {
            return CrossSite::MarshalVar(scriptContext, existing);
        }

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var value = nullptr;
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            value = CALL_FUNCTION(scriptContext->GetThreadContext(), callbackfn, CallInfo(CallFlags_Value, 2), undefined, key);
        }
        END_SAFE_REENTRANT_CALL

        // Re-commit unconditionally: callbackfn may have set/deleted/cleared key or
        // triggered GC/rehash; WeakMap::Set re-acquires WeakMapKeyMap/symbolMap fresh.
        WeakMapSetEntry(weakMap, scriptContext, keySym, keyObj, value);
        return CrossSite::MarshalVar(scriptContext, value);
    }

    void JavascriptWeakMap::Clear()
    {
        keySet.Map([&](RecyclableObject* key, bool value, WeakType weakRef) {
            WeakMapKeyMap* keyMap = GetWeakMapKeyMapFromKey(key);

            // It may be the case that a CEO has been reset and the keyMap is now null.
            // Just ignore it in this case, the keyMap has already been collected.
            if (keyMap != nullptr)
            {
                // It may also be the case that a CEO has been reset and then added to a separate WeakMap,
                // creating a new WeakMapKeyMap on the CEO.  In this case GetWeakMapId() may not be in the
                // keyMap, so don't assert successful removal here.
                keyMap->Remove(GetWeakMapId());
            }
        });
        keySet.Clear();
        if (symbolMap != nullptr)
        {
            symbolMap->Clear();
        }
    }

    bool JavascriptWeakMap::Delete(RecyclableObject* key)
    {
        WeakMapKeyMap* keyMap = GetWeakMapKeyMapFromKey(key);

        if (keyMap != nullptr)
        {
            bool unused = false;
            bool inSet = keySet.TryGetValueAndRemove(key, &unused);
            bool inData = keyMap->Remove(GetWeakMapId());
            Assert(inSet == inData);

            return inData;
        }

        return false;
    }

    bool JavascriptWeakMap::Get(RecyclableObject* key, Var* value) const
    {
        WeakMapKeyMap* keyMap = GetWeakMapKeyMapFromKey(key);

        if (keyMap != nullptr)
        {
            return KeyMapGet(keyMap, value);
        }

        return false;
    }

    bool JavascriptWeakMap::Has(RecyclableObject* key) const
    {
        WeakMapKeyMap* keyMap = GetWeakMapKeyMapFromKey(key);

        if (keyMap != nullptr)
        {
            return keyMap->ContainsKey(GetWeakMapId());
        }

        return false;
    }

    void JavascriptWeakMap::Set(RecyclableObject* key, Var value)
    {
        WeakMapKeyMap* keyMap = GetWeakMapKeyMapFromKey(key);

        if (keyMap == nullptr)
        {
            keyMap = AddWeakMapKeyMapToKey(key);
        }

        keyMap->Item(GetWeakMapId(), value);
        keySet.Item(key, true);
    }

    bool JavascriptWeakMap::Delete(JavascriptSymbol* key)
    {
        return symbolMap != nullptr && symbolMap->Remove(key);
    }

    bool JavascriptWeakMap::Get(JavascriptSymbol* key, Var* value) const
    {
        return symbolMap != nullptr && symbolMap->TryGetValue(key, value);
    }

    bool JavascriptWeakMap::Has(JavascriptSymbol* key) const
    {
        return symbolMap != nullptr && symbolMap->ContainsKey(key);
    }

    void JavascriptWeakMap::Set(JavascriptSymbol* key, Var value)
    {
        EnsureSymbolMap();
        symbolMap->Item(key, value);
    }

    BOOL JavascriptWeakMap::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("WeakMap"));
        return TRUE;
    }

#if ENABLE_TTD
    void JavascriptWeakMap::MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor)
    {
        //All weak things should be reachable from another root so no need to mark but do need to repopulate the pin sets if in replay mode
        Js::ScriptContext* scriptContext = this->GetScriptContext();
        if(scriptContext->IsTTDReplayModeEnabled())
        {
            this->Map([&](RecyclableObject* key, Js::Var value)
            {
                // Symbol keys are strongly held by symbolMap, so only object
                // keys need the replay weak-reference pin.
                if (JavascriptOperators::IsObject(key))
                {
                    scriptContext->TTDContextInfo->TTDWeakReferencePinSet->AddNew(key);
                }
            });
        }

        //Object keys are weak and stay reachable elsewhere; symbol keys are strong
        //in symbolMap. Values are not strongly reachable elsewhere, so walk them.
        this->Map([&](RecyclableObject* key, Js::Var value)
        {
            // Symbol keys are strong entries of this map, so the snapshot must
            // contain them even when no other root references the symbol.
            if (VarIs<JavascriptSymbol>(key))
            {
                extractor->MarkVisitVar(key);
            }
            extractor->MarkVisitVar(value);
        });
    }

    TTD::NSSnapObjects::SnapObjectType JavascriptWeakMap::GetSnapTag_TTD() const
    {
        return TTD::NSSnapObjects::SnapObjectType::SnapMapObject;
    }

    void JavascriptWeakMap::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
    {
        TTD::NSSnapObjects::SnapMapInfo* smi = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapMapInfo>();
        uint32 mapCountEst = this->Size() * 2;

        smi->MapSize = 0;
        smi->MapKeyValueArray = alloc.SlabReserveArraySpace<TTD::TTDVar>(mapCountEst + 1); //always reserve at least 1 element

        this->Map([&](RecyclableObject* key, Js::Var value)
        {
            AssertMsg(smi->MapSize + 1 < mapCountEst, "We are writing junk");

            smi->MapKeyValueArray[smi->MapSize] = key;
            smi->MapKeyValueArray[smi->MapSize + 1] = value;
            smi->MapSize += 2;
        });

        if(smi->MapSize == 0)
        {
            smi->MapKeyValueArray = nullptr;
            alloc.SlabAbortArraySpace<TTD::TTDVar>(mapCountEst + 1);
        }
        else
        {
            alloc.SlabCommitArraySpace<TTD::TTDVar>(smi->MapSize, mapCountEst + 1);
        }

        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapMapInfo*, TTD::NSSnapObjects::SnapObjectType::SnapMapObject>(objData, smi);
    }
#endif
}
