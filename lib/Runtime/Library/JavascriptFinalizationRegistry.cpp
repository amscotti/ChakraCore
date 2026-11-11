//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    template <> bool VarIsImpl<JavascriptFinalizationRegistryCleanupTaskFunction>(RecyclableObject* obj)
    {
        return VarIs<JavascriptFunction>(obj) &&
            (VirtualTableInfo<JavascriptFinalizationRegistryCleanupTaskFunction>::HasVirtualTable(obj) ||
             VirtualTableInfo<CrossSiteObject<JavascriptFinalizationRegistryCleanupTaskFunction>>::HasVirtualTable(obj));
    }

    // OrdinaryCreateFromConstructor -> GetPrototypeFromConstructor ->
    // GetFunctionRealm is shared with JavascriptOperators (proxy/bound target
    // chain); JavascriptFinalizationRegistry kept its local copy until the
    // helper moved so both sites can't drift apart.

    static bool IsValidWeakKey(Var value, RecyclableObject** object)
    {
        // ES CanBeHeldWeakly: objects and unregistered Symbols. Registered
        // Symbol.for entries have process-lifetime identity and are rejected;
        // well-known symbols are not registry entries and are eligible.
        if (VarIs<JavascriptSymbol>(value))
        {
            JavascriptSymbol* symbol = VarTo<JavascriptSymbol>(value);
            if (symbol->IsRegistered())
            {
                return false;
            }
            *object = symbol;
            return true;
        }

        if (JavascriptOperators::IsObject(value) &&
            JavascriptOperators::GetTypeId(value) != TypeIds_HostDispatch)
        {
            *object = VarTo<RecyclableObject>(value);
            return true;
        }

        return false;
    }

    Var JavascriptFinalizationRegistry::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("FinalizationRegistry"));
        }

        // A normal `new FinalizationRegistry` does not carry CallFlags_NewTarget;
        // its effective NewTarget is the active constructor.
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("FinalizationRegistry"));
        }

        Var cleanupCallback = (args.Info.Count > 1) ? args[1] : library->GetUndefined();
        if (!JavascriptConversion::IsCallable(cleanupCallback))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }

        JavascriptFinalizationRegistry* registry = library->CreateFinalizationRegistry(cleanupCallback);

        // Register a weak handle before the reentrant NewTarget.prototype
        // lookup. The registry itself is kept alive by this stack local while
        // the lookup may run user code/GC.
        scriptContext->RegisterFinalizationRegistry(registry);

        DynamicObject* intrinsicProto = library->GetFinalizationRegistryPrototype();
        RecyclableObject* newTargetObject = VarTo<RecyclableObject>(newTarget);
        Var proto = JavascriptOperators::GetPropertyNoCache(newTargetObject, PropertyIds::prototype, scriptContext);

        if (JavascriptOperators::IsObject(proto))
        {
            if (VarTo<RecyclableObject>(proto) != intrinsicProto)
            {
                JavascriptObject::ChangePrototype(registry, VarTo<RecyclableObject>(proto), true, scriptContext);
            }
        }
        else
        {
            ScriptContext* newTargetContext = JavascriptOperators::GetFunctionRealm(newTargetObject, scriptContext);
            if (newTargetContext != nullptr && newTargetContext != scriptContext)
            {
                DynamicObject* realmProto = newTargetContext->GetLibrary()->GetFinalizationRegistryPrototype();
                if (realmProto != intrinsicProto)
                {
                    JavascriptObject::ChangePrototype(registry, realmProto, true, scriptContext);
                }
            }
        }

        return registry;
    }

    Var JavascriptFinalizationRegistry::EntryRegister(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        if (!VarIs<JavascriptFinalizationRegistry>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType,
                _u("FinalizationRegistry.prototype.register"), _u("FinalizationRegistry"));
        }

        JavascriptFinalizationRegistry* registry = VarTo<JavascriptFinalizationRegistry>(args[0]);
        Var targetVar = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        Var heldValue = (args.Info.Count > 2) ? args[2] : scriptContext->GetLibrary()->GetUndefined();
        Var unregisterTokenVar = (args.Info.Count > 3) ? args[3] : scriptContext->GetLibrary()->GetUndefined();

        RecyclableObject* target = nullptr;
        if (!IsValidWeakKey(targetVar, &target))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("FinalizationRegistry"));
        }

        if (JavascriptConversion::SameValue(targetVar, heldValue))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("FinalizationRegistry"));
        }

        RecyclableObject* unregisterToken = nullptr;
        if (!JavascriptOperators::IsUndefined(unregisterTokenVar) &&
            !IsValidWeakKey(unregisterTokenVar, &unregisterToken))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("FinalizationRegistry"));
        }

        registry->Register(target, heldValue, unregisterToken);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var JavascriptFinalizationRegistry::EntryUnregister(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        if (!VarIs<JavascriptFinalizationRegistry>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType,
                _u("FinalizationRegistry.prototype.unregister"), _u("FinalizationRegistry"));
        }

        JavascriptFinalizationRegistry* registry = VarTo<JavascriptFinalizationRegistry>(args[0]);
        Var tokenVar = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();

        RecyclableObject* token = nullptr;
        if (!IsValidWeakKey(tokenVar, &token))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("FinalizationRegistry"));
        }

        return JavascriptBoolean::ToVar(registry->Unregister(token), scriptContext);
    }

    Var JavascriptFinalizationRegistry::EntryCleanupJob(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptFinalizationRegistryCleanupTaskFunction* task =
            VarTo<JavascriptFinalizationRegistryCleanupTaskFunction>(function);
        task->GetRegistry()->CleanupDeadCells();
        return function->GetScriptContext()->GetLibrary()->GetUndefined();
    }

    void JavascriptFinalizationRegistry::Register(
        RecyclableObject* target,
        Var heldValue,
        RecyclableObject* unregisterToken)
    {
        ScriptContext* scriptContext = GetScriptContext();
        Recycler* recycler = scriptContext->GetRecycler();

        // Validate both keys before adding either handle to the recycler's
        // weak-reference map, so a rejected token does not leave a stray entry.
        RecyclerHeapObjectInfo objectInfo;
        if (!recycler->FindHeapObject(target, FindHeapObjectFlags_NoFlags, objectInfo) ||
            (unregisterToken != nullptr &&
             !recycler->FindHeapObject(unregisterToken, FindHeapObjectFlags_NoFlags, objectInfo)))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("FinalizationRegistry"));
        }

        RecyclerWeakReference<RecyclableObject>* targetWeakRef = nullptr;
        recycler->FindOrCreateWeakReferenceHandle(target, &targetWeakRef);

        RecyclerWeakReference<RecyclableObject>* unregisterTokenWeakRef = nullptr;
        if (unregisterToken != nullptr)
        {
            recycler->FindOrCreateWeakReferenceHandle(unregisterToken, &unregisterTokenWeakRef);
        }

        if (cells == nullptr)
        {
            cells = RecyclerNew(recycler, CellList, recycler);
        }

        // heldValue is a strong Field(Var) in the cell, traced with the
        // cell. The target/token handles are RecyclerWeakReference objects
        // tracked by the recycler's weak-reference map and cleared by
        // SweepWeakReference at collect time; the cell holds only the
        // handle pointers and never traces through them to the
        // target/token objects.
        cells->Add(RecyclerNew(recycler, FinalizationRegistryCell, targetWeakRef, heldValue, unregisterTokenWeakRef));
    }

    bool JavascriptFinalizationRegistry::Unregister(RecyclableObject* unregisterToken)
    {
        if (cells == nullptr)
        {
            return false;
        }

        bool removed = false;
        for (int i = 0; i < cells->Count();)
        {
            FinalizationRegistryCell* cell = cells->Item(i);
            RecyclableObject* cellToken =
                cell->unregisterTokenWeakRef != nullptr ? cell->unregisterTokenWeakRef->Get() : nullptr;

            if (cellToken != nullptr && cellToken == unregisterToken)
            {
                cells->RemoveAt(i);
                removed = true;
            }
            else
            {
                ++i;
            }
        }

        return removed;
    }

    bool JavascriptFinalizationRegistry::HasClearedTargets() const
    {
        if (cells == nullptr)
        {
            return false;
        }

        for (int i = 0; i < cells->Count(); ++i)
        {
            FinalizationRegistryCell* cell = cells->Item(i);
            if (cell->targetWeakRef->Get() == nullptr)
            {
                return true;
            }
        }

        return false;
    }

    void JavascriptFinalizationRegistry::ScheduleCleanupJob()
    {
        if (cleanupJobActive)
        {
            // A reentrant host call must not start a second cleanup job while
            // this registry's cleanup is executing.
            return;
        }

        ScriptContext* scriptContext = GetScriptContext();
        if (scriptContext == nullptr || scriptContext->IsClosed())
        {
            cleanupJobScheduled = FALSE;
            return;
        }

        if (!HasClearedTargets())
        {
            return;
        }

        // cleanupJobScheduled only prevents a duplicate enqueue inside the
        // same scheduling attempt. At the next outermost boundary a still
        // pending task has either been dropped or delayed; reset the advisory
        // flag and enqueue an idempotent retry. The ch host drains the normal
        // queue between jobs, so a task scheduled here runs before the next
        // boundary unless the host drops or never drains it.
        cleanupJobScheduled = FALSE;

        JavascriptLibrary* library = scriptContext->GetLibrary();
        if (!library->HasNativeHostPromiseContinuationCallback())
        {
            // Finalization is not guaranteed when the host has no job queue.
            // Keep the cells and retry at a later boundary instead of running
            // callbacks synchronously inside OnScriptEnd.
            return;
        }

        cleanupJobScheduled = TRUE;
        try
        {
            library->EnqueueTask(library->CreateFinalizationRegistryCleanupTaskFunction(this));
        }
        catch (...)
        {
            // Task creation/OOM/host enqueue failure must not leave the
            // advisory flag set and wedge all future cleanup attempts.
            cleanupJobScheduled = FALSE;
            throw;
        }
    }

    void JavascriptFinalizationRegistry::CleanupDeadCells()
    {
        if (cleanupJobActive)
        {
            // ES IsFinalizationRegistryCleanupJobActive: nested/reentrant
            // cleanup is not allowed; leave remaining cells for a later job.
            return;
        }

        cleanupJobScheduled = FALSE;

        ScriptContext* scriptContext = GetScriptContext();
        if (scriptContext == nullptr || scriptContext->IsClosed() || cells == nullptr)
        {
            return;
        }

        ThreadContext* threadContext = scriptContext->GetThreadContext();
        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        RecyclableObject* callback = VarTo<RecyclableObject>(cleanupCallback);

        cleanupJobActive = TRUE;
        try
        {
            // CleanupFinalizationRegistry removes one cell before invoking the
            // callback. If the callback throws, the exception leaves the job
            // and the remaining dead cells are retried at a later boundary.
            for (int i = 0; i < cells->Count();)
            {
                FinalizationRegistryCell* cell = cells->Item(i);
                if (cell->targetWeakRef->Get() != nullptr)
                {
                    ++i;
                    continue;
                }

                Var heldValue = cell->heldValue;
                // The registered heldValue may belong to another script
                // context; marshal it into this registry's context before
                // handing it to the cleanup callback, exactly as
                // WeakRef::Deref marshals its target (identity when the
                // value is already in this context). Marshal before
                // removing the cell so a marshaling failure leaves the
                // cell in place for a later retry instead of losing it.
                heldValue = CrossSite::MarshalVar(scriptContext, heldValue);
                cells->RemoveAt(i);

                BEGIN_SAFE_REENTRANT_CALL(threadContext)
                {
                    CALL_FUNCTION(threadContext, callback, CallInfo(CallFlags_Value, 2), undefinedVar, heldValue);
                }
                END_SAFE_REENTRANT_CALL
            }
        }
        catch (...)
        {
            // The callback may throw. Clear the job-active guard before
            // propagating to the host so the next boundary can retry the
            // remaining cells.
            cleanupJobActive = FALSE;
            throw;
        }

        cleanupJobActive = FALSE;
    }

    BOOL JavascriptFinalizationRegistry::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("FinalizationRegistry"));
        return TRUE;
    }
}
