//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
namespace
{
    void __declspec(noreturn) ThrowDisposedReferenceError(ScriptContext* scriptContext, PCWSTR message)
    {
        JavascriptError* error = scriptContext->GetLibrary()->CreateReferenceError();
        JavascriptError::SetErrorMessageProperties(error, JSERR_UseBeforeDeclaration, message, scriptContext);
        JavascriptExceptionOperators::Throw(error, scriptContext);
    }

    Var ExtractThrownObject(JavascriptExceptionObject* exception, ScriptContext* scriptContext)
    {
        if (exception == nullptr)
        {
            return scriptContext->GetLibrary()->GetUndefined();
        }
        exception = exception->CloneIfStaticExceptionObject(scriptContext);
        Var thrown = exception->GetThrownObject(scriptContext);
        return thrown != nullptr ? thrown : scriptContext->GetLibrary()->GetUndefined();
    }

    // Drives async disposal over `capability`, resolving or rejecting `promiseCapability`
    // when all queued records finish. Called both initially from disposeAsync() and
    // reentrantly from Promise.then settlement callbacks.
    void DriveAsyncDispose(
        DisposeCapability* capability,
        JavascriptPromiseCapability* promiseCapability,
        ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var undefinedVar = library->GetUndefined();

        while (true)
        {
            int32 step = capability->StepAsyncOrSync(scriptContext);
            if (step == 0)
            {
                if (capability->HasPendingError())
                {
                    Var err = capability->TakePendingError(scriptContext);
                    JavascriptPromise::TryCallResolveOrRejectHandler(promiseCapability->GetReject(), err, scriptContext);
                }
                else
                {
                    JavascriptPromise::TryCallResolveOrRejectHandler(promiseCapability->GetResolve(), undefinedVar, scriptContext);
                }
                return;
            }
            if (step == 1)
            {
                continue;
            }

            // step == 2: await the produced value before continuing to the next record.
            Var awaitVal = capability->TakeAwaitValue(scriptContext);
            JavascriptPromise* resolvedPromise = nullptr;
            JavascriptExceptionObject* resolveException = nullptr;
            try
            {
                resolvedPromise = JavascriptPromise::InternalPromiseResolve(awaitVal, scriptContext);
            }
            catch (const JavascriptException& err)
            {
                resolveException = err.GetAndClear();
            }

            if (resolveException != nullptr)
            {
                capability->RecordError(ExtractThrownObject(resolveException, scriptContext), scriptContext);
                continue;
            }

            JavascriptAsyncDisposableStackStepFunction* onFulfilled =
                library->CreateAsyncDisposableStackStepFunction(capability, promiseCapability, /*isReject*/false);
            JavascriptAsyncDisposableStackStepFunction* onRejected =
                library->CreateAsyncDisposableStackStepFunction(capability, promiseCapability, /*isReject*/true);

            JavascriptPromise::PerformPromiseThen(
                resolvedPromise,
                JavascriptPromise::UnusedPromiseCapability(scriptContext),
                onFulfilled,
                onRejected,
                scriptContext);
            return;
        }
    }

    Var ResolveDisposeMethod(
        Var value,
        DisposeHint hint,
        ScriptContext* scriptContext,
        bool* needsSyncToAsyncWrap)
    {
        *needsSyncToAsyncWrap = false;
        JavascriptLibrary* library = scriptContext->GetLibrary();
        RecyclableObject* obj = VarTo<RecyclableObject>(value);

        if (hint == DisposeHint::SyncDispose)
        {
            Var method = JavascriptOperators::GetPropertyNoCache(obj, library->GetSymbolDisposePropertyId(), scriptContext);
            if (JavascriptOperators::IsUndefinedOrNull(method) || !JavascriptConversion::IsCallable(method))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
            }
            return method;
        }

        Var method = JavascriptOperators::GetPropertyNoCache(obj, library->GetSymbolAsyncDisposePropertyId(), scriptContext);
        if (!JavascriptOperators::IsUndefinedOrNull(method))
        {
            if (!JavascriptConversion::IsCallable(method))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
            }
            return method;
        }

        Var syncMethod = JavascriptOperators::GetPropertyNoCache(obj, library->GetSymbolDisposePropertyId(), scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(syncMethod) || !JavascriptConversion::IsCallable(syncMethod))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        *needsSyncToAsyncWrap = true;
        return syncMethod;
    }

    template <typename StackType>
    StackType* RequireDisposableStack(const Arguments& args, ScriptContext* scriptContext, PCWSTR methodName, PCWSTR typeName)
    {
        Var thisVar = args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined();
        if (!VarIs<StackType>(thisVar))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, methodName, typeName);
        }
        return UnsafeVarTo<StackType>(thisVar);
    }

    template <typename StackType>
    StackType* RequireUndisposedStack(const Arguments& args, ScriptContext* scriptContext, PCWSTR methodName, PCWSTR typeName, PCWSTR disposedMsg)
    {
        StackType* stack = RequireDisposableStack<StackType>(args, scriptContext, methodName, typeName);
        if (stack->IsDisposed())
        {
            ThrowDisposedReferenceError(scriptContext, disposedMsg);
        }
        return stack;
    }

    template <typename StackType, DisposeHint Hint>
    Var StackUseImpl(const Arguments& args, ScriptContext* scriptContext, PCWSTR methodName, PCWSTR typeName, PCWSTR disposedMsg)
    {
        StackType* stack = RequireUndisposedStack<StackType>(args, scriptContext, methodName, typeName, disposedMsg);
        Var value = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        stack->GetCapability()->AddResource(value, Hint, scriptContext);
        return CrossSite::MarshalVar(scriptContext, value);
    }

    template <typename StackType, DisposeHint Hint>
    Var StackAdoptImpl(const Arguments& args, ScriptContext* scriptContext, PCWSTR methodName, PCWSTR typeName, PCWSTR disposedMsg)
    {
        StackType* stack = RequireUndisposedStack<StackType>(args, scriptContext, methodName, typeName, disposedMsg);
        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var value = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var onDispose = args.Info.Count > 2 ? args[2] : undefinedVar;
        stack->GetCapability()->AddAdoptResource(value, onDispose, Hint, scriptContext);
        return CrossSite::MarshalVar(scriptContext, value);
    }

    template <typename StackType, DisposeHint Hint>
    Var StackDeferImpl(const Arguments& args, ScriptContext* scriptContext, PCWSTR methodName, PCWSTR typeName, PCWSTR disposedMsg)
    {
        StackType* stack = RequireUndisposedStack<StackType>(args, scriptContext, methodName, typeName, disposedMsg);
        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var onDispose = args.Info.Count > 1 ? args[1] : undefinedVar;
        stack->GetCapability()->AddDeferResource(onDispose, Hint, scriptContext);
        return undefinedVar;
    }

    template <typename StackType, typename CreateFn>
    Var NewDisposableStackImpl(
        RecyclableObject* function,
        const Arguments& args,
        CallInfo callInfo,
        ScriptContext* scriptContext,
        PCWSTR typeName,
        CreateFn createFn,
        DynamicObject* intrinsicProto)
    {
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, typeName);
        }

        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, typeName);
        }

        StackType* stack = createFn(scriptContext->GetLibrary());
        return JavascriptOperators::OrdinaryCreateFromConstructor(
            VarTo<RecyclableObject>(newTarget),
            stack,
            intrinsicProto,
            scriptContext);
    }
} // namespace

    DisposeCapability::DisposeCapability(DynamicType* type)
        : DynamicObject(type)
        , head(nullptr)
        , pendingError(nullptr)
        , currentAwaitValue(nullptr)
        , hasPendingError(false)
        , needsAwait(false)
        , hasAwaited(false)
    {
    }

    DisposeCapability* DisposeCapability::New(ScriptContext* scriptContext)
    {
        return RecyclerNew(scriptContext->GetRecycler(), DisposeCapability, scriptContext->GetLibrary()->GetDisposeCapabilityType());
    }

    DisposeCapability* DisposeCapability::FromVarOrNew(Var curStack, ScriptContext* scriptContext)
    {
        if (curStack != nullptr && VarIs<DisposeCapability>(curStack))
        {
            return UnsafeVarTo<DisposeCapability>(curStack);
        }
        return DisposeCapability::New(scriptContext);
    }

    void DisposeCapability::PushRecord(
        Var resource,
        Var disposeMethod,
        DisposeHint hint,
        bool needsSyncToAsyncWrap,
        bool passResourceAsArgument,
        ScriptContext* scriptContext)
    {
        DisposableResourceRecord* record = RecyclerNew(
            scriptContext->GetRecycler(),
            DisposableResourceRecord,
            resource,
            disposeMethod,
            hint,
            needsSyncToAsyncWrap,
            passResourceAsArgument,
            head);
        head = record;
    }

    void DisposeCapability::AddResource(Var value, DisposeHint hint, ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        if (JavascriptOperators::IsUndefinedOrNull(value))
        {
            if (hint == DisposeHint::AsyncDispose)
            {
                // `await using x = null/undefined` still enqueues an async microtask tick on exit.
                PushRecord(library->GetUndefined(), nullptr, DisposeHint::AsyncDispose, false, false, scriptContext);
            }
            return;
        }

        if (!JavascriptOperators::IsObject(value))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }

        bool needsSyncToAsyncWrap = false;
        Var method = ResolveDisposeMethod(value, hint, scriptContext, &needsSyncToAsyncWrap);
        PushRecord(value, method, hint, needsSyncToAsyncWrap, false, scriptContext);
    }

    void DisposeCapability::AddAdoptResource(Var value, Var onDispose, DisposeHint hint, ScriptContext* scriptContext)
    {
        if (!JavascriptConversion::IsCallable(onDispose))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        PushRecord(value, onDispose, hint, false, true, scriptContext);
    }

    void DisposeCapability::AddDeferResource(Var onDispose, DisposeHint hint, ScriptContext* scriptContext)
    {
        if (!JavascriptConversion::IsCallable(onDispose))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        PushRecord(scriptContext->GetLibrary()->GetUndefined(), onDispose, hint, false, false, scriptContext);
    }

    void DisposeCapability::RecordError(Var caughtError, ScriptContext* scriptContext)
    {
        caughtError = CrossSite::MarshalVar(scriptContext, caughtError);
        if (!hasPendingError)
        {
            hasPendingError = true;
            pendingError = caughtError;
            return;
        }

        // Most recent error is `error`; previously pending error becomes `suppressed`.
        Var prevError = CrossSite::MarshalVar(scriptContext, pendingError);
        pendingError = JavascriptError::CreateSuppressedError(scriptContext, caughtError, prevError);
    }

    Var DisposeCapability::InvokeDisposeMethod(const DisposableResourceRecord* record, ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var undefinedVar = library->GetUndefined();
        RecyclableObject* callable = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, record->disposeMethod));
        Var resourceVal = CrossSite::MarshalVar(scriptContext, record->resource);

        return scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [&]() -> Var
        {
            if (record->passResourceAsArgument)
            {
                Var argsArr[2] = { undefinedVar, resourceVal };
                CallInfo callInfo(CallFlags_Value, 2);
                return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callInfo, argsArr));
            }
            else
            {
                Var thisArg = JavascriptOperators::IsUndefined(resourceVal)
                    ? undefinedVar
                    : resourceVal;
                Var argsArr[1] = { thisArg };
                CallInfo callInfo(CallFlags_Value, 1);
                return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callInfo, argsArr));
            }
        });
    }

    void DisposeCapability::DisposeSync(ScriptContext* scriptContext)
    {
        while (head != nullptr)
        {
            DisposableResourceRecord* record = head;
            head = record->next;

            if (record->disposeMethod == nullptr)
            {
                continue;
            }

            JavascriptExceptionObject* exception = nullptr;
            try
            {
                InvokeDisposeMethod(record, scriptContext);
            }
            catch (const JavascriptException& err)
            {
                exception = err.GetAndClear();
            }

            if (exception != nullptr)
            {
                RecordError(ExtractThrownObject(exception, scriptContext), scriptContext);
            }
        }

        Finalize(scriptContext);
    }

    int32 DisposeCapability::StepAsyncOrSync(ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();

        while (head != nullptr)
        {
            DisposableResourceRecord* record = head;
            head = record->next;

            if (record->disposeMethod == nullptr)
            {
                // ECMA-262 DisposeResources step 3.f:
                // null/undefined async-dispose records defer a single Await(undefined)
                // to the end of disposal if no other await occurred.
                needsAwait = true;
                continue;
            }

            Var result = library->GetUndefined();
            JavascriptExceptionObject* exception = nullptr;
            try
            {
                result = InvokeDisposeMethod(record, scriptContext);
            }
            catch (const JavascriptException& err)
            {
                exception = err.GetAndClear();
            }

            if (exception != nullptr)
            {
                Var thrown = ExtractThrownObject(exception, scriptContext);
                if (record->needsSyncToAsyncWrap)
                {
                    hasAwaited = true;
                    currentAwaitValue = JavascriptPromise::CreateRejectedPromise(thrown, scriptContext);
                    return 2;
                }
                RecordError(thrown, scriptContext);
                return 1;
            }

            if (record->hint == DisposeHint::AsyncDispose)
            {
                hasAwaited = true;
                currentAwaitValue = record->needsSyncToAsyncWrap ? library->GetUndefined() : result;
                return 2;
            }

            return 1;
        }

        if (needsAwait && !hasAwaited)
        {
            hasAwaited = true;
            currentAwaitValue = library->GetUndefined();
            return 2;
        }

        return 0;
    }

    Var DisposeCapability::TakeAwaitValue(ScriptContext* scriptContext)
    {
        Var val = currentAwaitValue;
        currentAwaitValue = nullptr;
        return val != nullptr ? val : scriptContext->GetLibrary()->GetUndefined();
    }

    Var DisposeCapability::TakePendingError(ScriptContext* scriptContext)
    {
        Var err = pendingError;
        pendingError = nullptr;
        hasPendingError = false;
        return err != nullptr ? err : scriptContext->GetLibrary()->GetUndefined();
    }

    void DisposeCapability::Finalize(ScriptContext* scriptContext)
    {
        if (!hasPendingError)
        {
            return;
        }

        Var err = TakePendingError(scriptContext);
        JavascriptExceptionOperators::Throw(err, scriptContext);
    }

    DisposeCapability* DisposeCapability::MoveToNew(ScriptContext* scriptContext)
    {
        DisposeCapability* moved = DisposeCapability::New(scriptContext);
        moved->head = this->head;
        this->head = nullptr;
        return moved;
    }

    template <> bool VarIsImpl<DisposeCapability>(RecyclableObject* obj)
    {
        return VirtualTableInfo<DisposeCapability>::HasVirtualTable(obj)
            || VirtualTableInfo<CrossSiteObject<DisposeCapability>>::HasVirtualTable(obj);
    }

    Var JavascriptAsyncDisposableStackStepFunction::EntryStep(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptAsyncDisposableStackStepFunction* stepFn = VarTo<JavascriptAsyncDisposableStackStepFunction>(function);
        DisposeCapability* capability = stepFn->GetCapability();

        if (stepFn->IsReject())
        {
            Var reason = args.Info.Count > 1 ? args[1] : library->GetUndefined();
            capability->RecordError(reason, scriptContext);
        }

        DriveAsyncDispose(capability, stepFn->GetPromiseCapability(), scriptContext);
        return library->GetUndefined();
    }

    template <> bool VarIsImpl<JavascriptAsyncDisposableStackStepFunction>(RecyclableObject* obj)
    {
        if (VarIs<JavascriptFunction>(obj))
        {
            return VirtualTableInfo<JavascriptAsyncDisposableStackStepFunction>::HasVirtualTable(obj)
                || VirtualTableInfo<CrossSiteObject<JavascriptAsyncDisposableStackStepFunction>>::HasVirtualTable(obj);
        }
        return false;
    }

    JavascriptDisposableStack::JavascriptDisposableStack(DynamicType* type, DisposeCapability* capability)
        : DynamicObject(type)
        , capability(capability)
        , disposed(false)
    {
    }

    Var JavascriptDisposableStack::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        return NewDisposableStackImpl<JavascriptDisposableStack>(
            function,
            args,
            callInfo,
            scriptContext,
            _u("DisposableStack"),
            [](JavascriptLibrary* lib) { return lib->CreateDisposableStack(); },
            scriptContext->GetLibrary()->GetDisposableStackPrototype());
    }

    Var JavascriptDisposableStack::EntryUse(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackUseImpl<JavascriptDisposableStack, DisposeHint::SyncDispose>(
            args,
            function->GetScriptContext(),
            _u("DisposableStack.prototype.use"),
            _u("DisposableStack"),
            _u("Cannot use a disposed DisposableStack"));
    }

    Var JavascriptDisposableStack::EntryAdopt(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackAdoptImpl<JavascriptDisposableStack, DisposeHint::SyncDispose>(
            args,
            function->GetScriptContext(),
            _u("DisposableStack.prototype.adopt"),
            _u("DisposableStack"),
            _u("Cannot adopt into a disposed DisposableStack"));
    }

    Var JavascriptDisposableStack::EntryDefer(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackDeferImpl<JavascriptDisposableStack, DisposeHint::SyncDispose>(
            args,
            function->GetScriptContext(),
            _u("DisposableStack.prototype.defer"),
            _u("DisposableStack"),
            _u("Cannot defer onto a disposed DisposableStack"));
    }

    Var JavascriptDisposableStack::EntryDispose(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptDisposableStack* stack = RequireDisposableStack<JavascriptDisposableStack>(
            args, scriptContext, _u("DisposableStack.prototype.dispose"), _u("DisposableStack"));

        if (!stack->disposed)
        {
            stack->disposed = true;
            stack->capability->DisposeSync(scriptContext);
        }

        return library->GetUndefined();
    }

    Var JavascriptDisposableStack::EntryMove(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptDisposableStack* stack = RequireUndisposedStack<JavascriptDisposableStack>(
            args,
            scriptContext,
            _u("DisposableStack.prototype.move"),
            _u("DisposableStack"),
            _u("Cannot move a disposed DisposableStack"));

        DisposeCapability* movedCapability = stack->capability->MoveToNew(scriptContext);
        stack->disposed = true;
        return scriptContext->GetLibrary()->CreateDisposableStack(movedCapability);
    }

    Var JavascriptDisposableStack::EntryGetterDisposed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptDisposableStack* stack = RequireDisposableStack<JavascriptDisposableStack>(
            args, scriptContext, _u("get DisposableStack.prototype.disposed"), _u("DisposableStack"));

        return JavascriptBoolean::ToVar(stack->IsDisposed(), scriptContext);
    }

    BOOL JavascriptDisposableStack::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("DisposableStack"));
        return TRUE;
    }

    template <> bool VarIsImpl<JavascriptDisposableStack>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_DisposableStack;
    }

    JavascriptAsyncDisposableStack::JavascriptAsyncDisposableStack(DynamicType* type, DisposeCapability* capability)
        : DynamicObject(type)
        , capability(capability)
        , disposed(false)
    {
    }

    Var JavascriptAsyncDisposableStack::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        return NewDisposableStackImpl<JavascriptAsyncDisposableStack>(
            function,
            args,
            callInfo,
            scriptContext,
            _u("AsyncDisposableStack"),
            [](JavascriptLibrary* lib) { return lib->CreateAsyncDisposableStack(); },
            scriptContext->GetLibrary()->GetAsyncDisposableStackPrototype());
    }

    Var JavascriptAsyncDisposableStack::EntryUse(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackUseImpl<JavascriptAsyncDisposableStack, DisposeHint::AsyncDispose>(
            args,
            function->GetScriptContext(),
            _u("AsyncDisposableStack.prototype.use"),
            _u("AsyncDisposableStack"),
            _u("Cannot use a disposed AsyncDisposableStack"));
    }

    Var JavascriptAsyncDisposableStack::EntryAdopt(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackAdoptImpl<JavascriptAsyncDisposableStack, DisposeHint::AsyncDispose>(
            args,
            function->GetScriptContext(),
            _u("AsyncDisposableStack.prototype.adopt"),
            _u("AsyncDisposableStack"),
            _u("Cannot adopt into a disposed AsyncDisposableStack"));
    }

    Var JavascriptAsyncDisposableStack::EntryDefer(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        return StackDeferImpl<JavascriptAsyncDisposableStack, DisposeHint::AsyncDispose>(
            args,
            function->GetScriptContext(),
            _u("AsyncDisposableStack.prototype.defer"),
            _u("AsyncDisposableStack"),
            _u("Cannot defer onto a disposed AsyncDisposableStack"));
    }

    Var JavascriptAsyncDisposableStack::EntryDisposeAsync(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptPromiseCapability* promiseCapability = JavascriptPromise::NewPromiseCapability(library->GetPromiseConstructor(), scriptContext);

        JavascriptAsyncDisposableStack* stack = nullptr;
        JavascriptExceptionObject* exception = nullptr;
        try
        {
            stack = RequireDisposableStack<JavascriptAsyncDisposableStack>(
                args, scriptContext, _u("AsyncDisposableStack.prototype.disposeAsync"), _u("AsyncDisposableStack"));
        }
        catch (const JavascriptException& err)
        {
            exception = err.GetAndClear();
        }
        if (exception != nullptr)
        {
            JavascriptPromise::TryRejectWithExceptionObject(exception, promiseCapability->GetReject(), scriptContext);
            return promiseCapability->GetPromise();
        }

        if (stack->disposed)
        {
            JavascriptPromise::TryCallResolveOrRejectHandler(promiseCapability->GetResolve(), library->GetUndefined(), scriptContext);
            return promiseCapability->GetPromise();
        }

        stack->disposed = true;
        DriveAsyncDispose(stack->capability, promiseCapability, scriptContext);
        return promiseCapability->GetPromise();
    }

    Var JavascriptAsyncDisposableStack::EntryMove(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptAsyncDisposableStack* stack = RequireUndisposedStack<JavascriptAsyncDisposableStack>(
            args,
            scriptContext,
            _u("AsyncDisposableStack.prototype.move"),
            _u("AsyncDisposableStack"),
            _u("Cannot move a disposed AsyncDisposableStack"));

        DisposeCapability* movedCapability = stack->capability->MoveToNew(scriptContext);
        stack->disposed = true;
        return scriptContext->GetLibrary()->CreateAsyncDisposableStack(movedCapability);
    }

    Var JavascriptAsyncDisposableStack::EntryGetterDisposed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptAsyncDisposableStack* stack = RequireDisposableStack<JavascriptAsyncDisposableStack>(
            args, scriptContext, _u("get AsyncDisposableStack.prototype.disposed"), _u("AsyncDisposableStack"));

        return JavascriptBoolean::ToVar(stack->IsDisposed(), scriptContext);
    }

    BOOL JavascriptAsyncDisposableStack::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("AsyncDisposableStack"));
        return TRUE;
    }

    template <> bool VarIsImpl<JavascriptAsyncDisposableStack>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_AsyncDisposableStack;
    }
}
