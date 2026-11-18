//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    Var JavascriptAsyncFromSyncIterator::RejectPromiseWithCaughtException(const JavascriptException& err, ScriptContext* scriptContext)
    {
        JavascriptExceptionObject* exception = err.GetAndClear();
        Var thrownObject = exception != nullptr ? exception->GetThrownObject(scriptContext) : nullptr;
        if (thrownObject == nullptr)
        {
            thrownObject = scriptContext->GetLibrary()->GetUndefined();
        }
        return JavascriptPromise::CreateRejectedPromise(thrownObject, scriptContext);
    }

    void JavascriptAsyncFromSyncIterator::IteratorCloseNormalCompletion(RecyclableObject* syncIterator, ScriptContext* scriptContext)
    {
        // ES2025 7.4.7 IteratorClose ( iteratorRecord, NormalCompletion(empty) )
        Var returnMethod = JavascriptOperators::GetProperty(syncIterator, PropertyIds::return_, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
        {
            return;
        }
        if (!JavascriptConversion::IsCallable(returnMethod))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction, _u("return"));
        }

        RecyclableObject* callable = VarTo<RecyclableObject>(returnMethod);
        Var closeResult = scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [=]() -> Var
        {
            Js::Var callArgs[] = { syncIterator };
            Js::CallInfo callCallInfo(Js::CallFlags_Value, _countof(callArgs));
            return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callCallInfo, callArgs));
        });

        if (!JavascriptOperators::IsObject(closeResult))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NonObjectFromIterable, _u("AsyncFromSyncIterator.prototype.throw"));
        }
    }

    RecyclableObject* JavascriptAsyncFromSyncIterator::CreateCloseOnRejectFunction(RecyclableObject* syncIterator, ScriptContext* scriptContext)
    {
        static FunctionInfo s_closeOnRejectInfo(
            FORCE_NO_WRITE_BARRIER_TAG(&JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncCloseOnRejectFunction),
            FunctionInfo::ErrorOnNew);
        JavascriptLibrary* library = scriptContext->GetLibrary();
        DynamicType* type = library->CreateFunctionWithLengthAndNameType(&s_closeOnRejectInfo);
        AsyncFromSyncCloseOnRejectFunction* function = RecyclerNewEnumClass(
            scriptContext->GetRecycler(),
            JavascriptLibrary::EnumFunctionClass,
            AsyncFromSyncCloseOnRejectFunction,
            type,
            &s_closeOnRejectInfo,
            syncIterator);
        function->SetPropertyWithAttributes(PropertyIds::length, TaggedInt::ToVarUnchecked(1), PropertyConfigurable, nullptr);
        function->SetPropertyWithAttributes(PropertyIds::name, library->GetEmptyString(), PropertyConfigurable, nullptr);
        return function;
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncCloseOnRejectFunction(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var error = args.Info.Count > 1 ? CrossSite::MarshalVar(scriptContext, args[1]) : library->GetUndefined();

        AsyncFromSyncCloseOnRejectFunction* closeFn = VarTo<AsyncFromSyncCloseOnRejectFunction>(function);
        RecyclableObject* syncIterator = closeFn->GetSyncIterator();

        // ES2025 27.1.4.4 step 12.a.i:
        // Return ? IteratorClose(syncIteratorRecord, ThrowCompletion(error)).
        JavascriptOperators::IteratorClose(syncIterator, scriptContext);
        JavascriptExceptionOperators::Throw(error, scriptContext);
    }

    Var JavascriptAsyncFromSyncIterator::AsyncFromSyncIteratorContinuation(
        RecyclableObject* result,
        RecyclableObject* syncIteratorRecord,
        bool closeOnRejection,
        ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        // 1. Let done be IteratorComplete(result).
        // 2. IfAbruptRejectPromise(done, promiseCapability).
        bool done;
        try
        {
            done = JavascriptOperators::IteratorComplete(result, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 3. Let value be IteratorValue(result).
        // 4. IfAbruptRejectPromise(value, promiseCapability).
        Var value = nullptr;
        try
        {
            value = JavascriptOperators::IteratorValue(result, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 5. Let valueWrapper be PromiseResolve(%Promise%, value).
        JavascriptPromise* valueWrapper = nullptr;
        try
        {
            valueWrapper = JavascriptPromise::InternalPromiseResolve(value, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            // Clear the active exception before IteratorClose can reenter script.
            JavascriptExceptionObject* exception = err.GetAndClear();
            Var reason = exception != nullptr ? exception->GetThrownObject(scriptContext) : nullptr;
            if (reason == nullptr)
            {
                reason = library->GetUndefined();
            }

            // 6. If valueWrapper is an abrupt completion, done is false, and closeOnRejection is true, then
            //    a. Set valueWrapper to IteratorClose(syncIteratorRecord, valueWrapper).
            if (!done && closeOnRejection)
            {
                JavascriptOperators::IteratorClose(syncIteratorRecord, scriptContext);
            }

            // 7. IfAbruptRejectPromise(valueWrapper, promiseCapability).
            return JavascriptPromise::CreateRejectedPromise(reason, scriptContext);
        }

        // 8. Let unwrap be a new Abstract Closure with parameters (value) that captures done and performs the following steps when called:
        //    a. Return CreateIterResultObject(value, done).
        // 9. Let onFulfilled be CreateBuiltinFunction(unwrap, 1, "", << >>).
        RecyclableObject* onFulfilled;
        if (done)
        {
            onFulfilled = library->EnsureAsyncFromSyncIteratorValueUnwrapTrueFunction();
        }
        else
        {
            onFulfilled = library->EnsureAsyncFromSyncIteratorValueUnwrapFalseFunction();
        }

        // 11. If done is true, or if closeOnRejection is false, then
        //     a. Let onRejected be undefined.
        // 12. Else,
        //     a. Let closeIterator be a new Abstract Closure with parameters (error) that captures syncIteratorRecord and performs the following steps when called:
        //        i. Return ? IteratorClose(syncIteratorRecord, ThrowCompletion(error)).
        //     b. Let onRejected be CreateBuiltinFunction(closeIterator, 1, "", << >>).
        RecyclableObject* onRejected;
        if (done || !closeOnRejection)
        {
            onRejected = library->GetThrowerFunction();
        }
        else
        {
            onRejected = CreateCloseOnRejectFunction(syncIteratorRecord, scriptContext);
        }

        // 13. Perform PerformPromiseThen(valueWrapper, onFulfilled, onRejected, promiseCapability).
        // 14. Return promiseCapability.[[Promise]].
        JavascriptPromiseCapability* promiseCapability = JavascriptPromise::NewPromiseCapability(
            library->GetPromiseConstructor(),
            scriptContext);
        JavascriptPromise::PerformPromiseThen(
            valueWrapper,
            promiseCapability,
            onFulfilled,
            onRejected,
            scriptContext);
        return promiseCapability->GetPromise();
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncIteratorValueUnwrapTrueFunction(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        AssertOrFailFastMsg(args.Info.Count > 1, "AsyncFromSyncIteratorValueUnwrap should never be called without an argument");
        JavascriptLibrary* library = scriptContext->GetLibrary();

        return library->CreateIteratorResultObject(args[1], library->GetTrue());
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncIteratorValueUnwrapFalseFunction(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        AssertOrFailFastMsg(args.Info.Count > 1, "AsyncFromSyncIteratorValueUnwrap should never be called without an argument");
        JavascriptLibrary* library = scriptContext->GetLibrary();

        return library->CreateIteratorResultObject(args[1], library->GetFalse());
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncIteratorNext(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        AssertMsg(args.Info.Count > 0, "AsyncFromSyncIteratorNext should have implicit this");
        // 1. Let O be the this value.
        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        // promise creation deferred to the end to simplify logic

        // this step skipped as no codepath exists that will enable reaching here with wrong object type
        // 3. If Type(O) is not Object, or if O does not have a [[SyncIteratorRecord]] internal slot, then
        //    a. Let invalidIteratorError be a newly created TypeError object.
        //    b. Perform ! Call(promiseCapability.[[Reject]], undefined, << invalidIteratorError >>).
        //    c. Return promiseCapability.[[Promise]].

        // this will failfast if the logic that makes steps 3 irrelevant changes
        JavascriptAsyncFromSyncIterator* thisValue = VarTo<JavascriptAsyncFromSyncIterator>(args[0]);

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        RecyclableObject* syncIteratorRecord = thisValue->GetSyncIterator();
        RecyclableObject* result = nullptr;
        // 5. Let result be IteratorNext(syncIteratorRecord, value).
        try
        {
            result = JavascriptOperators::IteratorNext(syncIteratorRecord, scriptContext, thisValue->EnsureSyncNextFunction(scriptContext), args.Info.Count > 1 ? args[1] : nullptr);
        }
        catch (const JavascriptException& err)
        {
            // 6. IfAbruptRejectPromise(result, promiseCapability).
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 7. Return ! AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
        return JavascriptAsyncFromSyncIterator::AsyncFromSyncIteratorContinuation(result, syncIteratorRecord, true, scriptContext);
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncIteratorReturn(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        AssertMsg(args.Info.Count > 0, "AsyncFromSyncIteratorReturn should have implicit this");
        JavascriptLibrary* library = scriptContext->GetLibrary();

        // 1. Let O be the this value.
        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        // promise creation deferred to the end to simplify logic

        // this step skipped as no codepath exists that will enable reaching here with wrong object type
        // 3. If Type(O) is not Object, or if O does not have a [[SyncIteratorRecord]] internal slot, then
        //    a. Let invalidIteratorError be a newly created TypeError object.
        //    b. Perform ! Call(promiseCapability.[[Reject]], undefined, << invalidIteratorError >>).
        //    c. Return promiseCapability.[[Promise]].

        // this will failfast if the logic that makes steps 3-6 irrelevant changes
        JavascriptAsyncFromSyncIterator* thisValue = VarTo<JavascriptAsyncFromSyncIterator>(args[0]);

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        RecyclableObject* syncIteratorRecord = thisValue->GetSyncIterator();
        Var returnMethod = nullptr;
        Var result = nullptr;

        // 5. Let return be GetMethod(syncIterator, "return").
        // 6. IfAbruptRejectPromise(return, promiseCapability).
        // 7. If return is undefined, then
        //    a. Let iterResult be CreateIterResultObject(value, true).
        //    b. Perform ! Call(promiseCapability.[[Resolve]], undefined, << iterResult >>).
        //    c. Return promiseCapability.[[Promise]].
        try
        {
            returnMethod = JavascriptOperators::GetProperty(syncIteratorRecord, PropertyIds::return_, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
        {
            result = library->CreateIteratorResultObject(args.Info.Count > 1 ? args[1] : library->GetUndefined(), library->GetTrue());
            return JavascriptPromise::CreateResolvedPromise(result, scriptContext);
        }

        if (!JavascriptConversion::IsCallable(returnMethod))
        {
            JavascriptError* typeError = library->CreateTypeError();
            JavascriptError::SetErrorMessage(typeError, JSERR_NeedFunction, _u("AsyncFromSyncIterator.prototype.return"), scriptContext);
            return JavascriptPromise::CreateRejectedPromise(typeError, scriptContext);
        }

        // 8. Let result be Call(return, syncIterator, << value >>).
        try
        {
            RecyclableObject* callable = VarTo<RecyclableObject>(returnMethod);
            Var value = args.Info.Count > 1 ? args[1] : nullptr;
            result = scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [=]() -> Var
            {
                Js::Var callArgs[] = { syncIteratorRecord, value };
                Js::CallInfo callCallInfo(Js::CallFlags_Value, _countof(callArgs) + (value == nullptr ? -1 : 0));
                return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callCallInfo, callArgs));
            });
        }
        catch (const JavascriptException& err)
        {
            // 9. IfAbruptRejectPromise(result, promiseCapability).
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 10. If Type(result) is not Object, then
        //     a. Perform ! Call(promiseCapability.[[Reject]], undefined, << a newly created TypeError object >>).
        //     b. Return promiseCapability.[[Promise]].
        if (!JavascriptOperators::IsObject(result))
        {
            JavascriptError* typeError = library->CreateTypeError();
            JavascriptError::SetErrorMessage(typeError, JSERR_NonObjectFromIterable, _u("AsyncFromSyncIterator.prototype.return"), scriptContext);
            return JavascriptPromise::CreateRejectedPromise(typeError, scriptContext);
        }

        // 11. Return ! AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, false).
        return JavascriptAsyncFromSyncIterator::AsyncFromSyncIteratorContinuation(UnsafeVarTo<RecyclableObject>(result), syncIteratorRecord, false, scriptContext);
    }

    Var JavascriptAsyncFromSyncIterator::EntryAsyncFromSyncIteratorThrow(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        AssertMsg(args.Info.Count > 0, "AsyncFromSyncIteratorThrow should have implicit this");
        JavascriptLibrary* library = scriptContext->GetLibrary();

        // 1. Let O be the this value.
        // 2. Let promiseCapability be ! NewPromiseCapability(%Promise%).
        // promise creation deferred to the end to simplify logic

        // this step skipped as no codepath exists that will enable reaching here with wrong object type
        // 3. If Type(O) is not Object, or if O does not have a [[SyncIteratorRecord]] internal slot, then
        //    a. Let invalidIteratorError be a newly created TypeError object.
        //    b. Perform ! Call(promiseCapability.[[Reject]], undefined, << invalidIteratorError >>).
        //    c. Return promiseCapability.[[Promise]].

        // this will failfast if the logic that makes step 3 irrelevant changes
        JavascriptAsyncFromSyncIterator* thisValue = VarTo<JavascriptAsyncFromSyncIterator>(args[0]);

        // 4. Let syncIteratorRecord be O.[[SyncIteratorRecord]].
        RecyclableObject* syncIteratorRecord = thisValue->GetSyncIterator();
        Var throwMethod = nullptr;

        // 5. Let throw be GetMethod(syncIterator, "throw").
        // 6. IfAbruptRejectPromise(throw, promiseCapability).
        try
        {
            throwMethod = JavascriptOperators::GetProperty(syncIteratorRecord, PropertyIds::throw_, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 7. If throw is undefined, then
        //    a. NOTE: If syncIterator does not have a throw method, close it to give it a chance to clean up before we reject the capability.
        //    b. Let closeCompletion be NormalCompletion(empty).
        //    c. Let result be Completion(IteratorClose(syncIteratorRecord, closeCompletion)).
        //    d. IfAbruptRejectPromise(result, promiseCapability).
        //    g. Perform ! Call(promiseCapability.[[Reject]], undefined, << a newly created TypeError object >>).
        //    h. Return promiseCapability.[[Promise]].
        if (JavascriptOperators::IsUndefinedOrNull(throwMethod))
        {
            try
            {
                IteratorCloseNormalCompletion(syncIteratorRecord, scriptContext);
            }
            catch (const JavascriptException& err)
            {
                return RejectPromiseWithCaughtException(err, scriptContext);
            }

            JavascriptError* typeError = library->CreateTypeError();
            JavascriptError::SetErrorMessage(typeError, JSERR_YieldStarThrowMissing, static_cast<PCWSTR>(nullptr), scriptContext);
            return JavascriptPromise::CreateRejectedPromise(typeError, scriptContext);
        }

        if (!JavascriptConversion::IsCallable(throwMethod))
        {
            JavascriptError* typeError = library->CreateTypeError();
            JavascriptError::SetErrorMessage(typeError, JSERR_NeedFunction, _u("AsyncFromSyncIterator.prototype.throw"), scriptContext);
            return JavascriptPromise::CreateRejectedPromise(typeError, scriptContext);
        }

        // 8. Let result be Call(throw, syncIterator, << value >>).
        Var result = nullptr;
        try
        {
            RecyclableObject* callable = VarTo<RecyclableObject>(throwMethod);
            Var value = args.Info.Count > 1 ? args[1] : nullptr;
            result = scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [=]() -> Var
            {
                Js::Var callArgs[] = { syncIteratorRecord, value };
                Js::CallInfo callCallInfo(Js::CallFlags_Value, _countof(callArgs) + (value == nullptr ? -1 : 0));
                return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callCallInfo, callArgs));
            });
        }
        catch (const JavascriptException& err)
        {
            // 9. IfAbruptRejectPromise(result, promiseCapability).
            return RejectPromiseWithCaughtException(err, scriptContext);
        }

        // 10. If Type(result) is not Object, then
        //     a. Perform ! Call(promiseCapability.[[Reject]], undefined, << a newly created TypeError object >>).
        //     b. Return promiseCapability.[[Promise]].
        if (!JavascriptOperators::IsObject(result))
        {
            JavascriptError* typeError = library->CreateTypeError();
            JavascriptError::SetErrorMessage(typeError, JSERR_NonObjectFromIterable, _u("AsyncFromSyncIterator.prototype.throw"), scriptContext);
            return JavascriptPromise::CreateRejectedPromise(typeError, scriptContext);
        }

        // 11. Return ! AsyncFromSyncIteratorContinuation(result, promiseCapability, syncIteratorRecord, true).
        return JavascriptAsyncFromSyncIterator::AsyncFromSyncIteratorContinuation(UnsafeVarTo<RecyclableObject>(result), syncIteratorRecord, true, scriptContext);
    }

    RecyclableObject* JavascriptAsyncFromSyncIterator::EnsureSyncNextFunction(ScriptContext* scriptContext)
    {
        if (syncNextFunction == nullptr)
        {
            syncNextFunction = JavascriptOperators::CacheIteratorNext(syncIterator, scriptContext);
        }
        return syncNextFunction;
    }

    template <> bool VarIsImpl<JavascriptAsyncFromSyncIterator>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_AsyncFromSyncIterator;
    }

    template <> bool VarIsImpl<AsyncFromSyncCloseOnRejectFunction>(RecyclableObject* obj)
    {
        return VarIs<JavascriptFunction>(obj) && (
            VirtualTableInfo<AsyncFromSyncCloseOnRejectFunction>::HasVirtualTable(obj) ||
            VirtualTableInfo<CrossSiteObject<AsyncFromSyncCloseOnRejectFunction>>::HasVirtualTable(obj));
    }
}
