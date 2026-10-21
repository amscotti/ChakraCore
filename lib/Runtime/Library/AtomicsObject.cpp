//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

#define ATOMICS_FUNCTION_ENTRY_CHECKS(length, methodName) \
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault); \
        ARGUMENTS(args, callInfo); \
        ScriptContext* scriptContext = function->GetScriptContext(); \
        Assert(!(callInfo.Flags & CallFlags_New)); \
        if (args.Info.Count <= length) \
        { \
            JavascriptError::ThrowRangeError(scriptContext, JSERR_WinRTFunction_TooFewArguments, _u(methodName)); \
        } \


namespace Js
{
    ArrayBufferBase* AtomicsObject::ValidateIntegerTypedArray(Var typedArray, ScriptContext *scriptContext, bool onlyInt32, bool requireShared)
    {
        if (!VarIs<TypedArrayBase>(typedArray))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedTypedArrayObject);
        }

        TypeId typeId = JavascriptOperators::GetTypeId(typedArray);
        if (onlyInt32)
        {
            if (typeId != TypeIds_Int32Array)
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_InvalidOperationOnTypedArray);
            }
        }
        else
        {
            if (!(typeId == TypeIds_Int8Array || typeId == TypeIds_Uint8Array || typeId == TypeIds_Int16Array ||
                typeId == TypeIds_Uint16Array || typeId == TypeIds_Int32Array || typeId == TypeIds_Uint32Array))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_InvalidOperationOnTypedArray);
            }
        }

        TypedArrayBase *typedArrayBase = UnsafeVarTo<TypedArrayBase>(typedArray);
        ArrayBufferBase* arrayBuffer = typedArrayBase->GetArrayBuffer();
        if (arrayBuffer == nullptr || !VarIsCorrectType(arrayBuffer))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedTypedArrayObject);
        }

        // ValidateTypedArray throws TypeError for a detached buffer and for a
        // view that became out of bounds after a resize.
        if (arrayBuffer->IsDetached() || typedArrayBase->IsOutOfBounds())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }

        if (requireShared && !arrayBuffer->IsSharedArrayBuffer())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        return arrayBuffer;
    }

    uint32 AtomicsObject::ValidateAtomicAccess(Var typedArray, Var requestIndex, ScriptContext *scriptContext)
    {
        Assert(VarIs<TypedArrayBase>(typedArray));

        // TypedArrayLength is read before index coercion; a getter may resize
        // or grow the buffer, but the access is checked against the captured
        // length.
        uint32 length = VarTo<TypedArrayBase>(typedArray)->GetLength();
        uint32 accessIndex = ArrayBuffer::ToIndex(
            requestIndex, JSERR_InvalidTypedArrayIndex, scriptContext, ArrayBuffer::MaxArrayBufferLength, false);

        if (accessIndex >= length)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayIndex);
        }

        return accessIndex;
    }

    void AtomicsObject::CheckAtomicAccessStillValid(TypedArrayBase *typedArray, uint32 accessIndex, ScriptContext *scriptContext, const char16 *funcName)
    {
        // The index getter above can detach a non-shared buffer or shrink a
        // resizable one. Load has no value conversion, so this is its only
        // revalidation point. The value-coercing operations revalidate inside
        // their TypedArray helpers after the value conversion (RevalidateAtomicAccess
        // follows the conversion in the spec), so they must not call this early.
        if (typedArray->IsDetachedBuffer() || typedArray->IsOutOfBounds() || accessIndex >= typedArray->GetLength())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, funcName);
        }
    }

    TypedArrayBase * AtomicsObject::ValidateAndGetTypedArray(Var typedArray, Var index, _Out_ uint32 *accessIndex, ScriptContext *scriptContext, bool onlyInt32, bool requireShared)
    {
        ValidateIntegerTypedArray(typedArray, scriptContext, onlyInt32, requireShared);
        uint32 i = ValidateAtomicAccess(typedArray, index, scriptContext);
        if (accessIndex != nullptr)
        {
            *accessIndex = i;
        }

        return VarTo<TypedArrayBase>(typedArray);
    }

    Var AtomicsObject::EntryAdd(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.add");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedAdd(accessIndex, args[3]);
    }

    Var AtomicsObject::EntryAnd(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.and");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedAnd(accessIndex, args[3]);
    }

    Var AtomicsObject::EntryCompareExchange(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(4, "Atomics.compareExchange");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedCompareExchange(accessIndex, args[3], args[4]);
    }

    Var AtomicsObject::EntryExchange(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.exchange");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedExchange(accessIndex, args[3]);
    }

    Var AtomicsObject::EntryIsLockFree(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(1, "Atomics.isLockFree");
        uint32 size = JavascriptConversion::ToUInt32(args[1], scriptContext);
        // TODO : Currently for the size 1, 2 and 4 we are treating them lock free, we will take a look at this later.
        return (size == 1 || size == 2 || size == 4) ? scriptContext->GetLibrary()->GetTrue() : scriptContext->GetLibrary()->GetFalse();
    }

    Var AtomicsObject::EntryPause(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        // ES2026 25.4.12: no argument validation or coercion, no observable
        // effect other than timing. Use the architecture spin-wait hint.
#if defined(_M_ARM64)
        __yield();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        YieldProcessor();
#endif
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var AtomicsObject::EntryLoad(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(2, "Atomics.load");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        CheckAtomicAccessStillValid(typedArrayBase, accessIndex, scriptContext, _u("Atomics.load"));
        return typedArrayBase->TypedLoad(accessIndex);
    }

    Var AtomicsObject::EntryOr(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.or");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedOr(accessIndex, args[3]);
    }

    Var AtomicsObject::EntryStore(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.store");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedStore(accessIndex, args[3]);
    }

    Var AtomicsObject::EntrySub(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.sub");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedSub(accessIndex, args[3]);
    }

    Var AtomicsObject::EntryWait(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.wait");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext, true /*onlyInt32*/, true /*requireShared*/);

        int32 value = JavascriptConversion::ToInt32(args[3], scriptContext);
        uint32 timeout = INFINITE;

        if (args.Info.Count > 4 && !JavascriptOperators::IsUndefinedObject(args[4]))
        {
            double t =JavascriptConversion::ToNumber(args[4], scriptContext);
            if (!(NumberUtilities::IsNan(t) || JavascriptNumber::IsPosInf(t)))
            {
                int32 t1 = JavascriptConversion::ToInt32(t);
                timeout = (uint32)max(0, t1);
            }
        }

        if (!AgentOfBuffer::AgentCanSuspend(scriptContext))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_CannotSuspendBuffer);
        }

        Assert(typedArrayBase->GetBytesPerElement() == 4);
        uint32 bufferIndex = (accessIndex * 4) + typedArrayBase->GetByteOffset();
        Assert(bufferIndex < typedArrayBase->GetArrayBuffer()->GetByteLength());
        SharedArrayBuffer *sharedArrayBuffer = typedArrayBase->GetArrayBuffer()->GetAsSharedArrayBuffer();
        WaiterList *waiterList = sharedArrayBuffer->GetWaiterList(bufferIndex);


        bool awoken = false;

        {
            AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());

            int32 w = JavascriptConversion::ToInt32(typedArrayBase->DirectGetItem(accessIndex), scriptContext);
            if (value != w)
            {
                return scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("not-equal"));
            }

            DWORD_PTR agent = (DWORD_PTR)scriptContext;
            Assert(sharedArrayBuffer->GetSharedContents()->IsValidAgent(agent));
#pragma prefast(suppress:__WARNING_CALLER_FAILING_TO_HOLD, "This is a prefast false-positive caused by it being unable to identify that the critical section used here is the same as the one held by the AutoCriticalSection")
            awoken = waiterList->AddAndSuspendWaiter(agent, timeout);
            if (!awoken)
            {
                waiterList->RemoveWaiter(agent);
            }
        }

        return awoken ? scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("ok"))
            : scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("timed-out"));
    }

    Var AtomicsObject::CreateWaitAsyncResult(ScriptContext *scriptContext, bool async, Var value)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        DynamicObject* result = library->CreateObject();
        PropertyId asyncId = scriptContext->GetOrAddPropertyIdTracked(_u("async"));
        PropertyDescriptor descriptor;
        descriptor.SetWritable(true);
        descriptor.SetEnumerable(true);
        descriptor.SetConfigurable(true);

        descriptor.SetValue(async ? library->GetTrue() : library->GetFalse());
        JavascriptObject::DefineOwnPropertyHelper(result, asyncId, descriptor, scriptContext);
        descriptor.SetValue(value);
        JavascriptObject::DefineOwnPropertyHelper(result, PropertyIds::value, descriptor, scriptContext);
        return result;
    }

    Var AtomicsObject::EntryWaitAsync(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.waitAsync");

        // DoWait(async): ValidateIntegerTypedArray (waitable = Int32 only, shared
        // only), then ValidateAtomicAccess captures TypedArrayLength before
        // ToIndex(index), then value, then timeout.
        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(
            args[1], args[2], &accessIndex, scriptContext, true /*onlyInt32*/, true /*requireShared*/);

        int32 value = JavascriptConversion::ToInt32(args[3], scriptContext);

        Var timeoutArg = args.Info.Count > 4 ? args[4] : scriptContext->GetLibrary()->GetUndefined();
        double timeoutNumber = JavascriptConversion::ToNumber(timeoutArg, scriptContext);
        bool infiniteTimeout = NumberUtilities::IsNan(timeoutNumber) || JavascriptNumber::IsPosInf(timeoutNumber);
        double realTimeout = 0;
        if (!infiniteTimeout && !JavascriptNumber::IsNegInf(timeoutNumber) && timeoutNumber > 0)
        {
            realTimeout = timeoutNumber;
        }

        Assert(typedArrayBase->GetBytesPerElement() == 4);
        uint32 bufferIndex = (accessIndex * 4) + typedArrayBase->GetByteOffset();
        Assert(bufferIndex < typedArrayBase->GetArrayBuffer()->GetByteLength());
        SharedArrayBuffer *sharedArrayBuffer = typedArrayBase->GetArrayBuffer()->GetAsSharedArrayBuffer();
        WaiterList *waiterList = sharedArrayBuffer->GetWaiterList(bufferIndex);

        JavascriptSharedArrayBuffer* rootBuffer = static_cast<JavascriptSharedArrayBuffer*>(sharedArrayBuffer);
        JavascriptPromiseCapability* capability = JavascriptPromise::NewPromiseCapability(
            scriptContext->GetLibrary()->GetPromiseConstructor(), scriptContext);
        AtomicsAsyncWaiter* waiter = RecyclerNew(
            scriptContext->GetRecycler(), AtomicsAsyncWaiter, rootBuffer, capability, scriptContext);

        {
            AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());

            int32 witness = JavascriptConversion::ToInt32(typedArrayBase->DirectGetItem(accessIndex), scriptContext);
            if (value != witness)
            {
                return CreateWaitAsyncResult(scriptContext, false,
                    scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("not-equal")));
            }

            if (!infiniteTimeout && realTimeout == 0)
            {
                return CreateWaitAsyncResult(scriptContext, false,
                    scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("timed-out")));
            }

            // Register while holding the waiter-list lock so a concurrent notify
            // cannot miss the waiter. The record is rooted by rootBuffer; the
            // native token identifies it to notify and to the finalizer.
            AtomicsAsyncWaiterRegistration* registration = rootBuffer->AddAsyncWaiter(waiter);
            waiterList->AddAsyncWaiter(registration);
        }

        // Finite positive timeouts are not schedulable by this embedding (there
        // is no core timer job); the promise stays pending until notify. See
        // doc/c2-s6-waitasync.md.
        return CreateWaitAsyncResult(scriptContext, true, capability->GetPromise());
    }

    Var AtomicsObject::EntryNotify(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(2, "Atomics.notify");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext, true /*onlyInt32*/);
        int32 count = INT_MAX;
        if (args.Info.Count > 3 && !JavascriptOperators::IsUndefinedObject(args[3]))
        {
            double d = JavascriptConversion::ToInteger(args[3], scriptContext);
            if (!(NumberUtilities::IsNan(d) || JavascriptNumber::IsPosInf(d)))
            {
                int32 c = JavascriptConversion::ToInt32(d);
                count = max(0, c);
            }
        }

        // ES2020: notify accepts a non-shared Int32Array, evaluates count,
        // then returns zero because there is no shared waiter list.
        if (!typedArrayBase->GetArrayBuffer()->IsSharedArrayBuffer())
        {
            return JavascriptNumber::ToVar(0, scriptContext);
        }

        Assert(typedArrayBase->GetBytesPerElement() == 4);
        uint32 bufferIndex = (accessIndex * 4) + typedArrayBase->GetByteOffset();
        Assert(bufferIndex < typedArrayBase->GetArrayBuffer()->GetByteLength());
        SharedArrayBuffer *sharedArrayBuffer = typedArrayBase->GetArrayBuffer()->GetAsSharedArrayBuffer();
        WaiterList *waiterList = sharedArrayBuffer->GetWaiterList(bufferIndex);

        uint32 removed = 0;
        AtomicsAsyncWaiterRegistration* asyncAwoken = nullptr;
        {
            AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());
            removed = waiterList->RemoveAndWakeWaiters(count, scriptContext, &asyncAwoken);
        }

        // Resolve outside the critical section: promise resolution allocates and
        // may enqueue reactions. Only same-ScriptContext waiters can be resolved
        // safely on this thread (single-agent contract).
        while (asyncAwoken != nullptr)
        {
            AtomicsAsyncWaiterRegistration* registration = asyncAwoken;
            asyncAwoken = registration->nextAwoken;
            AtomicsAsyncWaiter* waiter = registration->record;
            if (!waiter->IsSettled() && waiter->GetScriptContext() == scriptContext)
            {
                waiter->ResolveOk(); // releases the registration token
            }
            else
            {
                waiter->Abandon();
            }
        }

        return JavascriptNumber::ToVar(removed, scriptContext);
    }

    Var AtomicsObject::EntryXor(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ATOMICS_FUNCTION_ENTRY_CHECKS(3, "Atomics.xor");

        uint32 accessIndex = 0;
        TypedArrayBase *typedArrayBase = ValidateAndGetTypedArray(args[1], args[2], &accessIndex, scriptContext);
        return typedArrayBase->TypedXor(accessIndex, args[3]);
    }
}
