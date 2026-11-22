//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/JavascriptIteratorZip.h"

namespace Js
{
namespace
{
    struct ZipRecord
    {
        RecyclableObject* iterator = nullptr;
        Var nextMethod = nullptr;
        bool done = false;
    };

    Var ArgOrUndefined(Arguments& args, uint index, ScriptContext* scriptContext)
    {
        return args.Info.Count > index ? args[index] : scriptContext->GetLibrary()->GetUndefined();
    }

    Var CallZipMethod(ScriptContext* scriptContext, Var funcVar, Var thisVar)
    {
        if (!JavascriptConversion::IsCallable(funcVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        RecyclableObject* callable = VarTo<RecyclableObject>(funcVar);
        Var argsArr[1] = { thisVar };
        CallInfo callInfo(CallFlags_Value, 1);
        return scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [&]() -> Var
        {
            return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callInfo, argsArr));
        });
    }

    void GetIteratorDirect(ZipRecord& record, RecyclableObject* iterator, ScriptContext* scriptContext)
    {
        record.iterator = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, iterator));
        record.done = false;
        Var nextMethod = JavascriptOperators::GetPropertyNoCache(record.iterator, PropertyIds::next, scriptContext);
        record.nextMethod = CrossSite::MarshalVar(scriptContext, nextMethod);
    }

    // ES2026 GetIterator(obj, sync): requires callable @@iterator.
    void GetIteratorSync(ZipRecord& record, RecyclableObject* obj, ScriptContext* scriptContext)
    {
        Var method = JavascriptOperators::GetPropertyNoCache(obj, PropertyIds::_symbolIterator, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(method) || !JavascriptConversion::IsCallable(method))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        Var iterator = CallZipMethod(scriptContext, method, obj);
        if (!JavascriptOperators::IsObject(iterator))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        GetIteratorDirect(record, VarTo<RecyclableObject>(iterator), scriptContext);
    }

    // ES2026 GetIteratorFlattenable(obj, reject-primitives): rejects all primitives,
    // falls back to treating obj as an iterator when @@iterator is undefined/null.
    void GetIteratorFlattenableRejectPrimitives(ZipRecord& record, Var obj, ScriptContext* scriptContext)
    {
        if (!JavascriptOperators::IsObject(obj))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        RecyclableObject* objRecord = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, obj));
        Var method = JavascriptOperators::GetPropertyNoCache(objRecord, PropertyIds::_symbolIterator, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(method))
        {
            GetIteratorDirect(record, objRecord, scriptContext);
            return;
        }
        if (!JavascriptConversion::IsCallable(method))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        Var iterator = CallZipMethod(scriptContext, method, objRecord);
        if (!JavascriptOperators::IsObject(iterator))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        GetIteratorDirect(record, VarTo<RecyclableObject>(iterator), scriptContext);
    }

    void CloseRecordNormal(ZipRecord& record, ScriptContext* scriptContext)
    {
        if (record.done || record.iterator == nullptr)
        {
            return;
        }
        RecyclableObject* iterator = record.iterator;
        record.done = true;
        record.iterator = nullptr;
        record.nextMethod = nullptr;

        Var returnMethod = JavascriptOperators::GetPropertyNoCache(iterator, PropertyIds::return_, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
        {
            return;
        }
        if (!JavascriptConversion::IsCallable(returnMethod))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        Var result = CallZipMethod(scriptContext, returnMethod, iterator);
        if (!JavascriptOperators::IsObject(result))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
    }

    void CloseRecordSwallow(ZipRecord& record, ScriptContext* scriptContext)
    {
        if (record.done || record.iterator == nullptr)
        {
            return;
        }
        RecyclableObject* iterator = record.iterator;
        record.done = true;
        record.iterator = nullptr;
        record.nextMethod = nullptr;

        try
        {
            Var returnMethod = JavascriptOperators::GetPropertyNoCache(iterator, PropertyIds::return_, scriptContext);
            if (JavascriptOperators::IsUndefinedOrNull(returnMethod) || !JavascriptConversion::IsCallable(returnMethod))
            {
                return;
            }
            CallZipMethod(scriptContext, returnMethod, iterator);
        }
        catch (const JavascriptException& err)
        {
            err.GetAndClear();
        }
    }

    bool StepRecordValue(ZipRecord& record, ScriptContext* scriptContext, Var* outValue)
    {
        bool done = false;
        try
        {
            Var resultVar = CallZipMethod(scriptContext, record.nextMethod, record.iterator);
            if (!JavascriptOperators::IsObject(resultVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            RecyclableObject* result = VarTo<RecyclableObject>(resultVar);
            done = JavascriptOperators::IteratorComplete(result, scriptContext);
            if (!done)
            {
                *outValue = JavascriptOperators::IteratorValue(result, scriptContext);
            }
        }
        catch (const JavascriptException& err)
        {
            record.done = true;
            JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
        }
        if (done)
        {
            record.done = true;
            return false;
        }
        return true;
    }

    void MarkEntryDone(ZipIterEntry* entry, JavascriptIteratorZipState* state)
    {
        if (!entry->done)
        {
            entry->done = true;
            entry->iterator = nullptr;
            entry->nextMethod = nullptr;
            if (state->openCount > 0)
            {
                state->openCount--;
            }
        }
    }

    void ClearResultsScratch(JavascriptIteratorZipState* state)
    {
        if (state != nullptr && state->resultsScratch != nullptr)
        {
            for (uint32 i = 0; i < state->iterCount; i++)
            {
                state->resultsScratch[i] = nullptr;
            }
        }
    }

    void CloseEntryNormal(ZipIterEntry* entry, JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        if (entry->done || entry->iterator == nullptr)
        {
            return;
        }
        ZipRecord record;
        record.iterator = entry->iterator;
        record.nextMethod = entry->nextMethod;
        record.done = false;
        MarkEntryDone(entry, state);
        CloseRecordNormal(record, scriptContext);
    }

    void CloseEntrySwallow(ZipIterEntry* entry, JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        if (entry->done || entry->iterator == nullptr)
        {
            return;
        }
        ZipRecord record;
        record.iterator = entry->iterator;
        record.nextMethod = entry->nextMethod;
        record.done = false;
        MarkEntryDone(entry, state);
        CloseRecordSwallow(record, scriptContext);
    }

    void CloseZipEntriesSwallow(JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        if (state == nullptr || state->entries == nullptr)
        {
            return;
        }
        for (int i = state->entries->Count() - 1; i >= 0; i--)
        {
            CloseEntrySwallow(state->entries->Item(i), state, scriptContext);
        }
    }

    // ES2026 IteratorCloseAll(openIters, ReturnCompletion(undefined)):
    // Closes open entries in reverse order. If an abrupt completion occurs while
    // closing, preserves the first thrown exception, continues closing remaining
    // entries in throw-swallow mode, and rethrows the first exception.
    void CloseZipEntriesNormal(JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        if (state == nullptr || state->entries == nullptr)
        {
            return;
        }
        JavascriptExceptionObject* firstException = nullptr;
        for (int i = state->entries->Count() - 1; i >= 0; i--)
        {
            ZipIterEntry* entry = state->entries->Item(i);
            if (entry->done || entry->iterator == nullptr)
            {
                continue;
            }
            if (firstException == nullptr)
            {
                try
                {
                    CloseEntryNormal(entry, state, scriptContext);
                }
                catch (const JavascriptException& err)
                {
                    firstException = err.GetAndClear();
                }
            }
            else
            {
                CloseEntrySwallow(entry, state, scriptContext);
            }
        }
        if (firstException != nullptr)
        {
            JavascriptExceptionOperators::DoThrowCheckClone(firstException, scriptContext);
        }
    }

    bool StepEntryValue(ZipIterEntry* entry, JavascriptIteratorZipState* state, ScriptContext* scriptContext, Var* outValue)
    {
        RecyclableObject* iterator = entry->iterator;
        Var nextMethod = entry->nextMethod;
        bool done = false;
        try
        {
            Var resultVar = CallZipMethod(scriptContext, nextMethod, iterator);
            if (!JavascriptOperators::IsObject(resultVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            RecyclableObject* result = VarTo<RecyclableObject>(resultVar);
            done = JavascriptOperators::IteratorComplete(result, scriptContext);
            if (!done)
            {
                *outValue = JavascriptOperators::IteratorValue(result, scriptContext);
            }
        }
        catch (const JavascriptException& err)
        {
            MarkEntryDone(entry, state);
            JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
        }
        if (done)
        {
            MarkEntryDone(entry, state);
            return false;
        }
        return true;
    }

    // IteratorStep without reading .value (used by strict mode when iter 0 finishes).
    // Returns true if the iterator is still open (done == false), false if done == true.
    bool StepEntryIsOpen(ZipIterEntry* entry, JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        RecyclableObject* iterator = entry->iterator;
        Var nextMethod = entry->nextMethod;
        bool done = false;
        try
        {
            Var resultVar = CallZipMethod(scriptContext, nextMethod, iterator);
            if (!JavascriptOperators::IsObject(resultVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            RecyclableObject* result = VarTo<RecyclableObject>(resultVar);
            done = JavascriptOperators::IteratorComplete(result, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            MarkEntryDone(entry, state);
            JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
        }
        if (done)
        {
            MarkEntryDone(entry, state);
            return false;
        }
        return true;
    }

    struct ZipOptions
    {
        ZipMode mode = ZipMode::Shortest;
        Var paddingOption = nullptr;
    };

    ZipOptions ParseZipOptions(Var optionsVar, ScriptContext* scriptContext, const char16* apiName)
    {
        ZipOptions parsed;
        parsed.paddingOption = scriptContext->GetLibrary()->GetUndefined();

        if (JavascriptOperators::IsUndefined(optionsVar))
        {
            return parsed;
        }
        if (!JavascriptOperators::IsObject(optionsVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        RecyclableObject* optionsObj = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, optionsVar));

        PropertyId modePropId = scriptContext->GetOrAddPropertyIdTracked(_u("mode"), 4);
        Var modeVar = JavascriptOperators::GetPropertyNoCache(optionsObj, modePropId, scriptContext);
        if (!JavascriptOperators::IsUndefined(modeVar))
        {
            if (!VarIs<JavascriptString>(modeVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
            }
            JavascriptString* modeStr = UnsafeVarTo<JavascriptString>(modeVar);
            if (modeStr->BufferEquals(_u("shortest"), 8))
            {
                parsed.mode = ZipMode::Shortest;
            }
            else if (modeStr->BufferEquals(_u("longest"), 7))
            {
                parsed.mode = ZipMode::Longest;
            }
            else if (modeStr->BufferEquals(_u("strict"), 6))
            {
                parsed.mode = ZipMode::Strict;
            }
            else
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
            }
        }

        if (parsed.mode == ZipMode::Longest)
        {
            PropertyId paddingPropId = scriptContext->GetOrAddPropertyIdTracked(_u("padding"), 7);
            Var paddingVar = JavascriptOperators::GetPropertyNoCache(optionsObj, paddingPropId, scriptContext);
            if (!JavascriptOperators::IsUndefined(paddingVar) && !JavascriptOperators::IsObject(paddingVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            parsed.paddingOption = paddingVar;
        }

        return parsed;
    }

    void FinalizeZipStateCounts(JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        uint32 count = static_cast<uint32>(state->entries->Count());
        state->iterCount = count;
        state->openCount = count;
        if (count > 0)
        {
            state->resultsScratch = RecyclerNewArrayZ(scriptContext->GetRecycler(), Field(Var), count);
        }
    }

    Var BuildZipResult(JavascriptIteratorZipState* state, ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        uint32 count = state->iterCount;
        if (!state->isKeyed)
        {
            JavascriptArray* arr = library->CreateArray(count);
            for (uint32 i = 0; i < count; i++)
            {
                arr->DirectSetItemAt(i, state->resultsScratch[i]);
                state->resultsScratch[i] = nullptr;
            }
            return arr;
        }

        DynamicObject* obj = library->CreateObject(library->GetNull());
        for (uint32 i = 0; i < count; i++)
        {
            ZipIterEntry* entry = state->entries->Item(i);
            PropertyDescriptor desc;
            desc.SetValue(state->resultsScratch[i]);
            desc.SetWritable(true);
            desc.SetEnumerable(true);
            desc.SetConfigurable(true);
            BOOL created = JavascriptObject::DefineOwnPropertyHelper(
                obj,
                entry->keyPropertyRecord->GetPropertyId(),
                desc,
                scriptContext);
            Assert(created);
            (void)created;
            state->resultsScratch[i] = nullptr;
        }
        return obj;
    }
} // namespace

    Var JavascriptIterator::EntryZip(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Recycler* recycler = scriptContext->GetRecycler();
        Assert(!(callInfo.Flags & CallFlags_New));

        Var iterablesVar = ArgOrUndefined(args, 1, scriptContext);
        if (!JavascriptOperators::IsObject(iterablesVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        RecyclableObject* iterablesObj = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, iterablesVar));

        Var optionsVar = ArgOrUndefined(args, 2, scriptContext);
        ZipOptions opts = ParseZipOptions(optionsVar, scriptContext, _u("Iterator.zip"));

        JavascriptIteratorZipState* state = RecyclerNew(recycler, JavascriptIteratorZipState, recycler, opts.mode, false);

        ZipRecord inputIter;
        GetIteratorSync(inputIter, iterablesObj, scriptContext);

        for (;;)
        {
            Var nextItem = nullptr;
            bool hasNext = false;
            try
            {
                hasNext = StepRecordValue(inputIter, scriptContext, &nextItem);
            }
            catch (const JavascriptException& err)
            {
                JavascriptExceptionObject* exception = err.GetAndClear();
                CloseZipEntriesSwallow(state, scriptContext);
                JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
            }
            if (!hasNext)
            {
                break;
            }

            ZipRecord innerRecord;
            try
            {
                GetIteratorFlattenableRejectPrimitives(innerRecord, nextItem, scriptContext);
            }
            catch (const JavascriptException& err)
            {
                JavascriptExceptionObject* exception = err.GetAndClear();
                CloseZipEntriesSwallow(state, scriptContext);
                CloseRecordSwallow(inputIter, scriptContext);
                JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
            }

            ZipIterEntry* entry = RecyclerNew(recycler, ZipIterEntry);
            entry->iterator = innerRecord.iterator;
            entry->nextMethod = innerRecord.nextMethod;
            entry->padding = library->GetUndefined();
            state->entries->Add(entry);
        }

        const uint32 iterCount = static_cast<uint32>(state->entries->Count());
        if (opts.mode == ZipMode::Longest)
        {
            Var undefinedVar = library->GetUndefined();
            if (JavascriptOperators::IsUndefined(opts.paddingOption))
            {
                for (uint32 i = 0; i < iterCount; i++)
                {
                    state->entries->Item(i)->padding = undefinedVar;
                }
            }
            else
            {
                RecyclableObject* paddingObj = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, opts.paddingOption));
                ZipRecord paddingIter;
                try
                {
                    GetIteratorSync(paddingIter, paddingObj, scriptContext);
                }
                catch (const JavascriptException& err)
                {
                    JavascriptExceptionObject* exception = err.GetAndClear();
                    CloseZipEntriesSwallow(state, scriptContext);
                    JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
                }

                bool usingIterator = true;
                for (uint32 i = 0; i < iterCount; i++)
                {
                    Var padVal = undefinedVar;
                    if (usingIterator)
                    {
                        bool hasPad = false;
                        try
                        {
                            hasPad = StepRecordValue(paddingIter, scriptContext, &padVal);
                        }
                        catch (const JavascriptException& err)
                        {
                            JavascriptExceptionObject* exception = err.GetAndClear();
                            CloseZipEntriesSwallow(state, scriptContext);
                            JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
                        }
                        if (!hasPad)
                        {
                            usingIterator = false;
                            padVal = undefinedVar;
                        }
                    }
                    state->entries->Item(i)->padding = CrossSite::MarshalVar(scriptContext, padVal);
                }

                if (usingIterator)
                {
                    try
                    {
                        CloseRecordNormal(paddingIter, scriptContext);
                    }
                    catch (const JavascriptException& err)
                    {
                        JavascriptExceptionObject* exception = err.GetAndClear();
                        CloseZipEntriesSwallow(state, scriptContext);
                        JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
                    }
                }
            }
        }

        FinalizeZipStateCounts(state, scriptContext);
        return JavascriptIteratorHelper::CreateZipHelper(scriptContext, IteratorHelperKind::Zip, state);
    }

    Var JavascriptIterator::EntryZipKeyed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Recycler* recycler = scriptContext->GetRecycler();
        Assert(!(callInfo.Flags & CallFlags_New));

        Var iterablesVar = ArgOrUndefined(args, 1, scriptContext);
        if (!JavascriptOperators::IsObject(iterablesVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        RecyclableObject* iterablesObj = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, iterablesVar));

        Var optionsVar = ArgOrUndefined(args, 2, scriptContext);
        ZipOptions opts = ParseZipOptions(optionsVar, scriptContext, _u("Iterator.zipKeyed"));

        JavascriptIteratorZipState* state = RecyclerNew(recycler, JavascriptIteratorZipState, recycler, opts.mode, true);

        JavascriptArray* allKeys = JavascriptOperators::GetOwnPropertyKeys(iterablesObj, scriptContext);
        uint32 allKeysLen = allKeys->GetLength();

        try
        {
            for (uint32 i = 0; i < allKeysLen; i++)
            {
                Var keyVar = nullptr;
                BOOL gotKey = allKeys->DirectGetItemAt(i, &keyVar);
                Assert(gotKey);
                if (!gotKey)
                {
                    continue;
                }

                const PropertyRecord* propertyRecord = nullptr;
                JavascriptConversion::ToPropertyKey(keyVar, scriptContext, &propertyRecord, nullptr);
                PropertyId propId = propertyRecord->GetPropertyId();

                PropertyDescriptor desc;
                if (JavascriptObject::GetOwnPropertyDescriptorHelper(iterablesObj, propId, scriptContext, desc) && desc.IsEnumerable())
                {
                    Var value = JavascriptOperators::GetPropertyNoCache(iterablesObj, propId, scriptContext);
                    if (!JavascriptOperators::IsUndefined(value))
                    {
                        ZipRecord innerRecord;
                        GetIteratorFlattenableRejectPrimitives(innerRecord, value, scriptContext);

                        ZipIterEntry* entry = RecyclerNew(recycler, ZipIterEntry);
                        entry->iterator = innerRecord.iterator;
                        entry->nextMethod = innerRecord.nextMethod;
                        entry->padding = library->GetUndefined();
                        entry->keyPropertyRecord = propertyRecord;
                        entry->keyVar = CrossSite::MarshalVar(scriptContext, keyVar);
                        state->entries->Add(entry);
                    }
                }
            }
        }
        catch (const JavascriptException& err)
        {
            JavascriptExceptionObject* exception = err.GetAndClear();
            CloseZipEntriesSwallow(state, scriptContext);
            JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
        }

        const uint32 iterCount = static_cast<uint32>(state->entries->Count());
        if (opts.mode == ZipMode::Longest)
        {
            Var undefinedVar = library->GetUndefined();
            if (JavascriptOperators::IsUndefined(opts.paddingOption))
            {
                for (uint32 i = 0; i < iterCount; i++)
                {
                    state->entries->Item(i)->padding = undefinedVar;
                }
            }
            else
            {
                RecyclableObject* paddingObj = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, opts.paddingOption));
                try
                {
                    for (uint32 i = 0; i < iterCount; i++)
                    {
                        ZipIterEntry* entry = state->entries->Item(i);
                        Var padVal = JavascriptOperators::GetPropertyNoCache(
                            paddingObj,
                            entry->keyPropertyRecord->GetPropertyId(),
                            scriptContext);
                        entry->padding = CrossSite::MarshalVar(scriptContext, padVal);
                    }
                }
                catch (const JavascriptException& err)
                {
                    JavascriptExceptionObject* exception = err.GetAndClear();
                    CloseZipEntriesSwallow(state, scriptContext);
                    JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
                }
            }
        }

        FinalizeZipStateCounts(state, scriptContext);
        return JavascriptIteratorHelper::CreateZipHelper(scriptContext, IteratorHelperKind::ZipKeyed, state);
    }

    bool JavascriptIteratorHelper::AdvanceZip(ScriptContext* scriptContext, Var* outValue)
    {
        JavascriptIteratorZipState* state = this->GetZipState();
        if (state == nullptr || state->iterCount == 0)
        {
            return false;
        }

        const uint32 iterCount = state->iterCount;
        try
        {
            for (uint32 i = 0; i < iterCount; i++)
            {
                ZipIterEntry* entry = state->entries->Item(i);
                if (entry->done)
                {
                    Assert(state->mode == ZipMode::Longest);
                    state->resultsScratch[i] = entry->padding;
                    continue;
                }

                Var stepVal = nullptr;
                if (!StepEntryValue(entry, state, scriptContext, &stepVal))
                {
                    if (state->mode == ZipMode::Shortest)
                    {
                        ClearResultsScratch(state);
                        CloseZipEntriesNormal(state, scriptContext);
                        return false;
                    }
                    if (state->mode == ZipMode::Strict)
                    {
                        ClearResultsScratch(state);
                        if (i != 0)
                        {
                            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.zip"));
                        }
                        for (uint32 k = 1; k < iterCount; k++)
                        {
                            ZipIterEntry* other = state->entries->Item(k);
                            if (StepEntryIsOpen(other, state, scriptContext))
                            {
                                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("Iterator.zip"));
                            }
                        }
                        return false;
                    }
                    Assert(state->mode == ZipMode::Longest);
                    if (state->openCount == 0)
                    {
                        ClearResultsScratch(state);
                        return false;
                    }
                    state->resultsScratch[i] = entry->padding;
                }
                else
                {
                    state->resultsScratch[i] = CrossSite::MarshalVar(scriptContext, stepVal);
                }
            }
        }
        catch (...)
        {
            ClearResultsScratch(state);
            throw;
        }

        *outValue = BuildZipResult(state, scriptContext);
        return true;
    }

    void JavascriptIteratorHelper::CloseZipNormal(ScriptContext* scriptContext)
    {
        CloseZipEntriesNormal(this->GetZipState(), scriptContext);
    }

    void JavascriptIteratorHelper::CloseZipSwallow(ScriptContext* scriptContext)
    {
        CloseZipEntriesSwallow(this->GetZipState(), scriptContext);
    }

    void JavascriptIteratorHelper::ReturnZip(ScriptContext* scriptContext)
    {
        CloseZipEntriesNormal(this->GetZipState(), scriptContext);
    }
} // namespace Js
