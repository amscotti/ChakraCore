//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    template <> bool VarIsImpl<AtomicsWaitAsyncTimeoutTaskFunction>(RecyclableObject* obj)
    {
        return VarIs<JavascriptFunction>(obj) &&
            (VirtualTableInfo<AtomicsWaitAsyncTimeoutTaskFunction>::HasVirtualTable(obj) ||
             VirtualTableInfo<CrossSiteObject<AtomicsWaitAsyncTimeoutTaskFunction>>::HasVirtualTable(obj));
    }

    template <> bool VarIsImpl<AtomicsWaitAsyncMonitorTaskFunction>(RecyclableObject* obj)
    {
        return VarIs<JavascriptFunction>(obj) &&
            (VirtualTableInfo<AtomicsWaitAsyncMonitorTaskFunction>::HasVirtualTable(obj) ||
             VirtualTableInfo<CrossSiteObject<AtomicsWaitAsyncMonitorTaskFunction>>::HasVirtualTable(obj));
    }

    bool SharedContents::MarkCrossRuntime()
    {
        // One-way latch; multiple markings only grow the counter, which is
        // read as nonzero/nonzero. Interlocked RMWs are full barriers, and
        // the host broadcast handshake (event/semaphore) already orders the
        // writer against the receiving runtime. Returns whether the contents
        // were already marked, so first-share work runs exactly once.
        return InterlockedExchangeAdd(&crossRuntime, 1L) != 0;
    }

    bool SharedContents::IsCrossRuntime()
    {
        return InterlockedExchangeAdd(&crossRuntime, 0L) != 0;
    }

    void JavascriptSharedArrayBuffer::MonitorPendingAsyncWaiters()
    {
        // First-share hook: waitAsync registrations made while this buffer
        // was still single-runtime never scheduled a monitor task (no
        // cross-runtime notifies were possible), so their tokens say
        // monitored == false and a foreign notify would skip and not count
        // them — a permanently deaf waiter. The contents becoming
        // cross-runtime changes that: schedule monitors for every pending
        // unmonitored record, exactly like EntryWaitAsync does at
        // registration. Runs on the extracting thread; on the first
        // extraction only the extracting runtime owns wrappers over these
        // contents, so this is the registrant's thread and touching the GC
        // records here is safe.
        ScriptContext* scriptContext = GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        if (!library->HasNativeHostTimerContinuationCallback())
        {
            // No scheduling mechanism: keep the S6b contract (foreign
            // notifies skip and do not count these waiters).
            return;
        }
#if ENABLE_TTD
        if (scriptContext->ShouldPerformRecordOrReplayAction())
        {
            return;
        }
#endif

        AtomicsAsyncWaiterRegistration* registration = asyncRegistrations;
        while (registration != nullptr)
        {
            AtomicsAsyncWaiter* record = registration->record;
            if (!registration->monitored && !record->IsSettled())
            {
                // Create and enqueue the task BEFORE publishing monitored
                // (same discipline as EntryWaitAsync): on task-creation OOM
                // the exception propagates and this waiter — and the ones
                // after it — keep the S6b skip-and-uncount contract instead
                // of a counted wake nobody can resolve. This thread is the
                // only drainer of its host queue, so the task cannot run
                // before publication completes.
                AtomicsWaitAsyncMonitorTaskFunction* task =
                    library->CreateAtomicsWaitAsyncMonitorTaskFunction(record);
                library->EnqueueDelayedTask(task, AtomicsAsyncWaiter::MonitorPollIntervalMs);

                WaiterList* waiterList = registration->waiterList;
                if (waiterList != nullptr)
                {
                    AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());
                    if (!registration->monitored && !record->IsSettled())
                    {
                        if (!record->HasRegistration())
                        {
                            // Pre-share registrations never cached their
                            // token links (only finite-timeout ones did);
                            // the monitor task's claim needs them.
                            record->SetRegistration(registration);
                        }
                        registration->monitored = true;
                    }
                }
                // A waiter that settled between the fast check and the lock
                // is left as-is: its settled flag already released it.
            }
            registration = registration->nextWrapper;
        }
    }

#if DBG
    void SharedContents::AddAgent(DWORD_PTR agent)
    {
        AutoCriticalSection autoCS(&csAgent);
        if (allowedAgents == nullptr)
        {
            allowedAgents = HeapNew(SharableAgents, &HeapAllocator::Instance);
        }

        allowedAgents->Add(agent);
    }

    bool SharedContents::IsValidAgent(DWORD_PTR agent)
    {
        AutoCriticalSection autoCS(&csAgent);
        return allowedAgents != nullptr && allowedAgents->Contains(agent);
    }
#endif

    long SharedContents::AddRef()
    {
        return InterlockedIncrement(&refCount);
    }
    long SharedContents::Release()
    {
        long ret = InterlockedDecrement(&refCount);
        AssertOrFailFastMsg(ret >= 0, "Buffer already freed");
        return ret;
    }

    void SharedContents::Cleanup()
    {
        Assert(refCount == 0);
        buffer = nullptr;
        bufferLength = 0;
#if DBG
        {
            AutoCriticalSection autoCS(&csAgent);
            if (allowedAgents != nullptr)
            {
                HeapDelete(allowedAgents);
                allowedAgents = nullptr;
            }
        }
#endif

        if (indexToWaiterList != nullptr)
        {
            // TODO: the map should be empty here?
            // or we need to wake all the waiters from current context?
            indexToWaiterList->Map([](uint index, WaiterList *waiters)
            {
                if (waiters != nullptr)
                {
                    waiters->Cleanup();
                    HeapDelete(waiters);
                    waiters = nullptr;
                }
            });

            HeapDelete(indexToWaiterList);
            indexToWaiterList = nullptr;
        }
    }

    // ToIndex without the engine's MaxSharedArrayBufferLength clamp. The
    // constructor performs the same ordering as ArrayBuffer: ToIndex(length),
    // read/ToIndex(options.maxByteLength), compare, resolve newTarget.prototype,
    // then apply the engine allocation limit before allocating the data block.
    static double ToIndexNoEngineCap(Var value, int32 errorCode, ScriptContext* scriptContext)
    {
        if (JavascriptOperators::IsUndefined(value))
        {
            return 0.0;
        }

        if (TaggedInt::Is(value))
        {
            int64 index = TaggedInt::ToInt64(value);
            if (index < 0)
            {
                JavascriptError::ThrowRangeError(scriptContext, errorCode);
            }
            return (double)index;
        }

        double d = JavascriptConversion::ToInteger(value, scriptContext);
        if (d < 0.0 || d > 9007199254740991.0) // 2^53 - 1
        {
            JavascriptError::ThrowRangeError(scriptContext, errorCode);
        }
        return d;
    }

    Var SharedArrayBuffer::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);

        if (!(callInfo.Flags & CallFlags_New) || (newTarget && JavascriptOperators::IsUndefinedObject(newTarget)))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("SharedArrayBuffer"));
        }

        double byteLengthValue = 0.0;
        if (args.Info.Count > 1)
        {
            byteLengthValue = ToIndexNoEngineCap(args[1], JSERR_ArrayLengthConstructIncorrect, scriptContext);
        }

        // GetArrayBufferMaxByteLengthOption: only objects participate; the
        // property is read and coerced before the byteLength/maxByteLength
        // comparison and before any object creation.
        bool hasMaxByteLength = false;
        double requestedMaxByteLength = 0.0;
        if (args.Info.Count > 2 && JavascriptOperators::IsObjectType(JavascriptOperators::GetTypeId(args[2])))
        {
            PropertyId maxByteLengthId = scriptContext->GetOrAddPropertyIdTracked(_u("maxByteLength"));
            Var maxByteLengthValue = JavascriptOperators::GetProperty(VarTo<RecyclableObject>(args[2]), maxByteLengthId, scriptContext);
            if (!JavascriptOperators::IsUndefined(maxByteLengthValue))
            {
                hasMaxByteLength = true;
                requestedMaxByteLength = ToIndexNoEngineCap(maxByteLengthValue, JSERR_ArrayLengthConstructIncorrect, scriptContext);
            }
        }

        if (hasMaxByteLength && byteLengthValue > requestedMaxByteLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        // AllocateSharedArrayBuffer creates the JS object before the data
        // block, so resolve the newTarget prototype before allocating.
        RecyclableObject* newTargetPrototype = nullptr;
        if (isCtorSuperCall)
        {
            Var proto = JavascriptOperators::GetPropertyNoCache(VarTo<RecyclableObject>(newTarget), PropertyIds::prototype, scriptContext);
            if (JavascriptOperators::IsObjectType(JavascriptOperators::GetTypeId(proto)))
            {
                newTargetPrototype = VarTo<RecyclableObject>(proto);
            }
        }

        double maxByteLength = hasMaxByteLength ? requestedMaxByteLength : byteLengthValue;
        if (byteLengthValue > (double)MaxSharedArrayBufferLength || maxByteLength > (double)MaxSharedArrayBufferLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        SharedArrayBuffer* newArr = library->CreateSharedArrayBuffer(
            (uint32)byteLengthValue, (uint32)maxByteLength, hasMaxByteLength);
        Assert(newArr != nullptr);
        if (byteLengthValue > 0 && newArr->GetByteLength() == 0)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid);
        }

        if (newTargetPrototype != nullptr)
        {
            JavascriptObject::ChangePrototype(newArr, newTargetPrototype, true, scriptContext);
        }
        return newArr;
    }

    // SharedArrayBuffer.prototype.byteLength
    Var SharedArrayBuffer::EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<SharedArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        SharedArrayBuffer* sharedArrayBuffer = VarTo<SharedArrayBuffer>(args[0]);
        return JavascriptNumber::ToVar(sharedArrayBuffer->GetByteLength(), scriptContext);
    }

    // SharedArrayBuffer.prototype.maxByteLength
    Var SharedArrayBuffer::EntryGetterMaxByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<SharedArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        return JavascriptNumber::ToVar(VarTo<SharedArrayBuffer>(args[0])->GetMaxByteLength(), scriptContext);
    }

    // SharedArrayBuffer.prototype.growable
    Var SharedArrayBuffer::EntryGetterGrowable(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<SharedArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        return VarTo<SharedArrayBuffer>(args[0])->IsGrowable()
            ? scriptContext->GetLibrary()->GetTrue()
            : scriptContext->GetLibrary()->GetFalse();
    }

    // SharedArrayBuffer.prototype.grow ( newLength )
    Var SharedArrayBuffer::EntryGrow(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        // Step order: brand and [[ArrayBufferMaxByteLength]] checks precede
        // argument coercion; range validation follows coercion.
        if (args.Info.Count == 0 || !VarIs<SharedArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        SharedArrayBuffer* sharedArrayBuffer = VarTo<SharedArrayBuffer>(args[0]);
        if (!sharedArrayBuffer->IsGrowable())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("SharedArrayBuffer.prototype.grow"));
        }

        Var newLength = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        uint32 newByteLength = ArrayBuffer::ToIndex(
            newLength, JSERR_ArrayLengthConstructIncorrect, scriptContext, MaxSharedArrayBufferLength);

        if (newByteLength < sharedArrayBuffer->GetByteLength() ||
            newByteLength > sharedArrayBuffer->GetMaxByteLength())
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        sharedArrayBuffer->Grow(newByteLength);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    void SharedArrayBuffer::Grow(uint32 newByteLength)
    {
        Assert(sharedContents != nullptr);
        Assert(sharedContents->isGrowable);
        Assert(newByteLength >= sharedContents->bufferLength);
        Assert(newByteLength <= sharedContents->maxBufferLength);

        uint32 oldByteLength = sharedContents->bufferLength;
        if (newByteLength > oldByteLength)
        {
            Assert(sharedContents->buffer != nullptr);
            ZeroMemory(sharedContents->buffer + oldByteLength, newByteLength - oldByteLength);
            sharedContents->bufferLength = newByteLength;
        }
    }

    // SharedArrayBuffer.prototype.slice
    Var SharedArrayBuffer::EntrySlice(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Assert(!(callInfo.Flags & CallFlags_New));

        if (!VarIs<SharedArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        JavascriptLibrary* library = scriptContext->GetLibrary();
        SharedArrayBuffer* currentBuffer = VarTo<SharedArrayBuffer>(args[0]);

        int64 currentLen = (int64)currentBuffer->GetByteLength();
        int64 start = 0, end = 0;
        int64 newLen = 0;

        // If no start or end arguments, use the entire length
        if (args.Info.Count < 2)
        {
            newLen = currentLen;
        }
        else
        {
            start = JavascriptArray::GetIndexFromVar(args[1], currentLen, scriptContext);

            // If no end argument, use length as the end
            if (args.Info.Count < 3 || args[2] == library->GetUndefined())
            {
                end = currentLen;
            }
            else
            {
                end = JavascriptArray::GetIndexFromVar(args[2], currentLen, scriptContext);
            }

            newLen = end > start ? end - start : 0;
        }

        // We can't have allocated an SharedArrayBuffer with byteLength > MaxArrayBufferLength.
        // start and end are clamped to valid indices, so the new length also cannot exceed MaxArrayBufferLength.
        // Therefore, should be safe to cast down newLen to uint32.
        Assert(newLen < MaxSharedArrayBufferLength);
        uint32 newbyteLength = static_cast<uint32>(newLen);

        SharedArrayBuffer* newBuffer = nullptr;

        JavascriptFunction* defaultConstructor = scriptContext->GetLibrary()->GetSharedArrayBufferConstructor();
        RecyclableObject* constructor = JavascriptOperators::SpeciesConstructor(currentBuffer, defaultConstructor, scriptContext);
        AssertOrFailFast(JavascriptOperators::IsConstructor(constructor));

        bool isDefaultConstructor = constructor == defaultConstructor;
        Js::Var newVar = JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
        {
            Js::Var constructorArgs[] = { constructor, JavascriptNumber::ToVar(newbyteLength, scriptContext) };
            Js::CallInfo constructorCallInfo(Js::CallFlags_New, _countof(constructorArgs));
            return JavascriptOperators::NewScObject(constructor, Js::Arguments(constructorCallInfo, constructorArgs), scriptContext);
        });

        if (!VarIs<SharedArrayBuffer>(newVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        newBuffer = VarTo<SharedArrayBuffer>(newVar);

        if (newBuffer == currentBuffer)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedSharedArrayBufferObject);
        }

        if (newBuffer->GetByteLength() < newbyteLength)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ArgumentOutOfRange, _u("SharedArrayBuffer.prototype.slice"));
        }

        Assert(newBuffer);
        Assert(newBuffer->GetByteLength() >= newbyteLength);

        // Don't bother doing memcpy if we aren't copying any elements
        if (newbyteLength > 0)
        {
            AssertMsg(currentBuffer->GetBuffer() != nullptr, "buffer must not be null when we copy from it");

            js_memcpy_s(newBuffer->GetBuffer(), newbyteLength, currentBuffer->GetBuffer() + start, newbyteLength);
        }

        return newBuffer;
    }

    Var SharedArrayBuffer::EntryGetterSymbolSpecies(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ARGUMENTS(args, callInfo);

        Assert(args.Info.Count > 0);

        return args[0];
    }

    BYTE* SharedArrayBuffer::AllocBuffer(uint32 length, uint32 maxLength, bool isGrowable)
    {
#if ENABLE_FAST_ARRAYBUFFER
        // Growable JS buffers cannot use the fixed-length virtual allocator:
        // growth never remaps, so the full capacity must be a heap allocation.
        if (!isGrowable && this->IsValidVirtualBufferLength(length))
        {
            return (BYTE*)AsmJsVirtualAllocator(length);
        }
        else
#endif
        {
            return HeapNewNoThrowArray(BYTE, isGrowable ? maxLength : length);
        }
    }

    void SharedArrayBuffer::FreeBuffer(BYTE* buffer, uint32 length, uint32 maxLength, bool isGrowable)
    {
#if ENABLE_FAST_ARRAYBUFFER
        //AsmJS Virtual Free
        if (!isGrowable && this->IsValidVirtualBufferLength(length))
        {
            FreeMemAlloc(buffer);
        }
        else
#endif
        {
            HeapDeleteArray(isGrowable ? maxLength : length, buffer);
        }
    }

    void SharedArrayBuffer::Init(uint32 length, uint32 maxLength, bool isGrowable)
    {
        AssertOrFailFast(!sharedContents && length <= maxLength);
        BYTE * buffer = nullptr;
        if (length > MaxSharedArrayBufferLength)
        {
            // http://tc39.github.io/ecmascript_sharedmem/shmem.html#DataTypesValues.SpecTypes.DataBlocks.CreateSharedByteDataBlock
            // Let db be a new Shared Data Block value consisting of size bytes.
            // If it is impossible to create such a Shared Data Block, throw a RangeError exception.
            JavascriptError::ThrowRangeError(GetScriptContext(), JSERR_FunctionArgument_Invalid);
        }
        SharedContents* localSharedContents = HeapNewNoThrow(SharedContents, nullptr, length, maxLength);
        if (localSharedContents == nullptr)
        {
            JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
        }
        localSharedContents->isGrowable = isGrowable;
        struct AutoCleanupSharedContents
        {
            SharedContents* sharedContents;
            bool allocationCompleted = false;
            AutoCleanupSharedContents(SharedContents* sharedContents) : sharedContents(sharedContents) {}
            ~AutoCleanupSharedContents()
            {
                if (!allocationCompleted)
                {
                    HeapDelete(sharedContents);
                }
            }
        } autoCleanupSharedContents(localSharedContents);

        Recycler* recycler = GetType()->GetLibrary()->GetRecycler();

        // A growable JS buffer allocates its full maxByteLength capacity once;
        // fixed buffers and WebAssembly buffers keep the existing length-based
        // allocation and accounting. A zero maxLength means there is no data
        // block to allocate (length 0 fixed buffer).
        uint32 allocationLength = isGrowable ? maxLength : length;

        if (maxLength != 0)
        {
            if (recycler->RequestExternalMemoryAllocation(allocationLength))
            {
                buffer = this->AllocBuffer(length, maxLength, isGrowable);
                if (buffer == nullptr)
                {
                    recycler->CollectNow<CollectOnTypedArrayAllocation>();

                    buffer = this->AllocBuffer(length, maxLength, isGrowable);
                    if (buffer == nullptr)
                    {
                        recycler->ReportExternalMemoryFailure(allocationLength);
                        JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
                    }
                }
            }
            else
            {
                JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
            }

            Assert(buffer != nullptr);
            // Zero the whole capacity: grow only exposes bytes in this range and
            // never reallocates, and the zero-fill contract starts here.
            ZeroMemory(buffer, isGrowable ? maxLength : length);
        }
        localSharedContents->buffer = buffer;
#if DBG
        localSharedContents->AddAgent((DWORD_PTR)GetScriptContext());
#endif
        sharedContents = localSharedContents;
        autoCleanupSharedContents.allocationCompleted = true;
    }

    SharedArrayBuffer::SharedArrayBuffer(DynamicType * type) :
        ArrayBufferBase(type), sharedContents(nullptr)
    {
    }

    SharedArrayBuffer::SharedArrayBuffer(SharedContents * contents, DynamicType * type) :
        ArrayBufferBase(type), sharedContents(nullptr)
    {
        if (contents == nullptr || contents->bufferLength > MaxSharedArrayBufferLength)
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_FunctionArgument_Invalid);
        }

        if (contents->AddRef() > 1)
        {
            sharedContents = contents;
        }
        else
        {
            Js::Throw::FatalInternalError();
        }
#if DBG
        sharedContents->AddAgent((DWORD_PTR)GetScriptContext());
#endif
    }

    SharedArrayBuffer * SharedArrayBuffer::GetAsSharedArrayBuffer()
    {
        AssertOrFailFast(VarIsCorrectType(this));
        return this;
    }

    CriticalSection SharedArrayBuffer::csSharedArrayBuffer;

    WaiterList *SharedArrayBuffer::GetWaiterList(uint index)
    {
        if (sharedContents != nullptr)
        {
            // REVIEW: only lock creating the map and pass the lock to the map?
            //         use one lock per instance?
            AutoCriticalSection autoCS(&csSharedArrayBuffer);

            if (sharedContents->indexToWaiterList == nullptr)
            {
                sharedContents->indexToWaiterList = HeapNew(IndexToWaitersMap, &HeapAllocator::Instance);
            }

            WaiterList * waiters = nullptr;
            if (!sharedContents->indexToWaiterList->TryGetValue(index, &waiters))
            {
                waiters = HeapNew(WaiterList);
                sharedContents->indexToWaiterList->Add(index, waiters);
            }
            return waiters;
        }

        Assert(false);
        return nullptr;
    }

    uint32 SharedArrayBuffer::GetByteLength() const
    {
        return sharedContents != nullptr ? sharedContents->bufferLength : 0;
    }

    BYTE* SharedArrayBuffer::GetBuffer() const
    {
        return sharedContents != nullptr ? sharedContents->buffer : nullptr;
    }

    BOOL SharedArrayBuffer::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("Object, (SharedArrayBuffer)"));
        return TRUE;
    }

    BOOL SharedArrayBuffer::GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("[object SharedArrayBuffer]"));
        return TRUE;
    }

    JavascriptSharedArrayBuffer::JavascriptSharedArrayBuffer(DynamicType * type) :
        SharedArrayBuffer(type), asyncWaiters(nullptr), asyncRegistrations(nullptr)
    {
    }
    JavascriptSharedArrayBuffer::JavascriptSharedArrayBuffer(SharedContents *sharedContents, DynamicType * type) :
        SharedArrayBuffer(sharedContents, type), asyncWaiters(nullptr), asyncRegistrations(nullptr)
    {
    }

    JavascriptSharedArrayBuffer* JavascriptSharedArrayBuffer::Create(uint32 length, DynamicType * type)
    {
        return Create(length, length, false /*isGrowable*/, type);
    }

    JavascriptSharedArrayBuffer* JavascriptSharedArrayBuffer::Create(uint32 length, uint32 maxByteLength, bool isGrowable, DynamicType * type)
    {
        AssertOrFailFast(length <= maxByteLength);
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptSharedArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptSharedArrayBuffer, type);
        result->Init(length, maxByteLength, isGrowable);
        // A growable buffer owns its whole capacity, so charge the capacity.
        recycler->AddExternalMemoryUsage(isGrowable ? maxByteLength : length);
        return result;
    }

    JavascriptSharedArrayBuffer* JavascriptSharedArrayBuffer::Create(SharedContents *sharedContents, DynamicType * type)
    {
        AssertOrFailFast(!sharedContents || !sharedContents->IsWebAssembly());
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptSharedArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptSharedArrayBuffer, sharedContents, type);
        return result;
    }

    bool SharedArrayBuffer::IsValidVirtualBufferLength(uint length) const
    {
        // Growable JS buffers use a heap allocation sized to maxByteLength and
        // rely on length-tracking views; they must not be classified as the
        // asm.js virtual-buffer fast path (same rule as resizable ArrayBuffer
        // call sites: !isResizable && IsValidVirtualBufferLength).
        if (IsGrowable())
        {
            return false;
        }
#if ENABLE_FAST_ARRAYBUFFER
        /*
        1. length >= 2^16
        2. length is power of 2 or (length > 2^24 and length is multiple of 2^24)
        3. length is a multiple of 4K
        */
        return !PHASE_OFF1(Js::TypedArrayVirtualPhase) &&
            JavascriptArrayBuffer::IsValidAsmJsBufferLengthAlgo(length, true);
#else
        return false;
#endif
    }

    void JavascriptSharedArrayBuffer::CancelAsyncWaiters()
    {
        AtomicsAsyncWaiterRegistration* registration = asyncRegistrations;
        while (registration != nullptr)
        {
            AtomicsAsyncWaiterRegistration* next = registration->nextWrapper;
            if (registration->waiterList != nullptr)
            {
                // Read `linked` under the waiter-list CS (it is written by a
                // foreign notifier under the same CS). While linked == true
                // the unlink happens under the CS too (RemoveAsyncWaiter
                // re-enters the recursive CS); once linked == false is
                // observed here, the notifier has finished its final token
                // write and will not touch the token again, so HeapDelete is
                // safe. This closes the unlinked-token/UAF window of the
                // earlier revision, where `linked` was read without the CS
                // and the notifier still had a latch write outstanding.
                AutoCriticalSection autoCS(registration->waiterList->GetCriticalSectionForAccess());
                if (registration->linked)
                {
                    registration->waiterList->RemoveAsyncWaiter(registration);
                }
            }
            HeapDelete(registration);
            registration = next;
        }
        asyncRegistrations = nullptr;
    }

    void JavascriptSharedArrayBuffer::Finalize(bool isShutdown)
    {
        // Drop pending waitAsync registrations before the shared waiter lists
        // (which hold native registration tokens) can be torn down. Promise code
        // must not run from a finalizer; these promises stay pending. The tokens
        // intentionally do not dereference AtomicsAsyncWaiter: an unmarked
        // wrapper is not scanned, so its records may already have been swept.
        CancelAsyncWaiters();

        if (sharedContents == nullptr)
        {
            return;
        }

        uint ref = sharedContents->Release();
        if (ref == 0)
        {
            this->FreeBuffer(sharedContents->buffer, sharedContents->bufferLength, sharedContents->maxBufferLength, sharedContents->isGrowable);

            Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
            // Growable JS buffers are charged for their whole capacity; fixed
            // and WebAssembly buffers keep the current-length accounting.
            recycler->ReportExternalMemoryFree(sharedContents->isGrowable ? sharedContents->maxBufferLength : sharedContents->bufferLength);

            sharedContents->Cleanup();
            HeapDelete(sharedContents);
        }

        sharedContents = nullptr;
    }

    void JavascriptSharedArrayBuffer::Dispose(bool isShutdown)
    {
        /* See JavascriptArrayBuffer::Finalize */
    }

#ifdef ENABLE_WASM_THREADS
    WebAssemblySharedArrayBuffer::WebAssemblySharedArrayBuffer(DynamicType * type):
        JavascriptSharedArrayBuffer(type)
    {
        AssertOrFailFast(Wasm::Threads::IsEnabled());
    }

    WebAssemblySharedArrayBuffer::WebAssemblySharedArrayBuffer(SharedContents *sharedContents, DynamicType * type) :
        JavascriptSharedArrayBuffer(sharedContents, type)
    {
        AssertOrFailFast(Wasm::Threads::IsEnabled());
        ValidateBuffer();
    }

    void WebAssemblySharedArrayBuffer::ValidateBuffer()
    {
#if DBG && _WIN32
        if (CONFIG_FLAG(WasmSharedArrayVirtualBuffer))
        {
            MEMORY_BASIC_INFORMATION info = { 0 };
            size_t size = 0;
            size_t allocationSize = 0;
            // Make sure the beggining of the buffer is committed memory to the expected size
            if (sharedContents->bufferLength > 0)
            {
                size = VirtualQuery((LPCVOID)sharedContents->buffer, &info, sizeof(info));
                Assert(size > 0);
                allocationSize = info.RegionSize + ((uintptr_t)info.BaseAddress - (uintptr_t)info.AllocationBase);
                Assert(allocationSize == sharedContents->bufferLength && info.State == MEM_COMMIT && info.Type == MEM_PRIVATE);
            }

            // Make sure the end of the buffer is reserved memory to the expected size
            size_t expectedAllocationSize = sharedContents->maxBufferLength;
#if ENABLE_FAST_ARRAYBUFFER
            if (CONFIG_FLAG(WasmFastArray))
            {
                expectedAllocationSize = MAX_WASM__ARRAYBUFFER_LENGTH;
            }
#endif
            // If the whole buffer has been committed, no need to verify this
            if (expectedAllocationSize > sharedContents->bufferLength)
            {
                size = VirtualQuery((LPCVOID)(sharedContents->buffer + sharedContents->bufferLength), &info, sizeof(info));
                Assert(size > 0);
                allocationSize = info.RegionSize + ((uintptr_t)info.BaseAddress - (uintptr_t)info.AllocationBase);
                Assert(allocationSize == expectedAllocationSize && info.State == MEM_RESERVE && info.Type == MEM_PRIVATE);
            }
        }
#endif
    }

    WebAssemblySharedArrayBuffer* WebAssemblySharedArrayBuffer::Create(uint32 length, uint32 maxLength, DynamicType * type)
    {
        AssertOrFailFast(Wasm::Threads::IsEnabled());
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        WebAssemblySharedArrayBuffer* result = RecyclerNewFinalized(recycler, WebAssemblySharedArrayBuffer, type);
        result->Init(length, maxLength);
        result->sharedContents->SetIsWebAssembly();
        result->ValidateBuffer();
        recycler->AddExternalMemoryUsage(length);
        return result;
    }

    WebAssemblySharedArrayBuffer* WebAssemblySharedArrayBuffer::Create(SharedContents *sharedContents, DynamicType * type)
    {
        AssertOrFailFast(Wasm::Threads::IsEnabled());
        AssertOrFailFast(sharedContents && sharedContents->IsWebAssembly());
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        WebAssemblySharedArrayBuffer* result = RecyclerNewFinalized(recycler, WebAssemblySharedArrayBuffer, sharedContents, type);
        return result;
    }

    bool WebAssemblySharedArrayBuffer::IsValidVirtualBufferLength(uint length) const
    {
#if ENABLE_FAST_ARRAYBUFFER
        if (CONFIG_FLAG(WasmFastArray))
        {
            return true;
        }
#endif
#ifdef _WIN32
        if (CONFIG_FLAG(WasmSharedArrayVirtualBuffer))
        {
            return true;
        }
#endif
        return false;
    }

    _Must_inspect_result_ bool WebAssemblySharedArrayBuffer::GrowMemory(uint32 newBufferLength)
    {
        uint32 bufferLength = sharedContents->bufferLength;
        BYTE* buffer = sharedContents->buffer;
        if (newBufferLength < bufferLength || newBufferLength > sharedContents->maxBufferLength)
        {
            AssertMsg(newBufferLength <= sharedContents->maxBufferLength, "This shouldn't happen");
            Assert(UNREACHED);
            JavascriptError::ThrowTypeError(GetScriptContext(), WASMERR_BufferGrowOnly);
        }

        uint32 growSize = newBufferLength - bufferLength;

        // We're not growing the buffer, do nothing
        if (growSize == 0)
        {
            return true;
        }

        AssertOrFailFast(buffer);
        if (IsValidVirtualBufferLength(newBufferLength))
        {
            auto virtualAllocFunc = [=]
            {
                return !!VirtualAlloc(buffer + bufferLength, growSize, MEM_COMMIT, PAGE_READWRITE);
            };
            if (!this->GetRecycler()->DoExternalAllocation(growSize, virtualAllocFunc))
            {
                return false;
            }

            this->GetRecycler()->AddExternalMemoryUsage(growSize);
        }
        else
        {
            // We have already allocated maxLength in the heap if we're not using virtual alloc
        }
        ZeroMemory(buffer + bufferLength, growSize);
        sharedContents->bufferLength = newBufferLength;
        ValidateBuffer();
        return true;
    }

    BYTE* WebAssemblySharedArrayBuffer::AllocBuffer(uint32 length, uint32 maxLength, bool isGrowable)
    {
        Unused(isGrowable); // WebAssembly buffers are grown by GrowMemory, never by SAB.grow
#if ENABLE_FAST_ARRAYBUFFER
        if (CONFIG_FLAG(WasmFastArray))
        {
            return (BYTE*)WasmVirtualAllocator(length);
        }
#endif
#ifdef _WIN32
        if (CONFIG_FLAG(WasmSharedArrayVirtualBuffer))
        {
            return (BYTE*)AllocWrapper(length, maxLength);
        }
#endif
        AssertOrFailFast(maxLength >= length);
        uint32 additionalSize = maxLength - length;
        if (additionalSize > 0)
        {
            // SharedArrayBuffer::Init already requested External Memory for `length`, we need to request the balance
            if (!this->GetRecycler()->RequestExternalMemoryAllocation(additionalSize))
            {
                // Failed to request for more memory
                return nullptr;
            }
        }
        // Allocate the full size of the buffer if we can't do VirtualAlloc
        return HeapNewNoThrowArray(BYTE, maxLength);
    }

    void WebAssemblySharedArrayBuffer::FreeBuffer(BYTE* buffer, uint32 length, uint32 maxLength, bool isGrowable)
    {
        Unused(isGrowable);
        if (IsValidVirtualBufferLength(length))
        {
            FreeMemAlloc(buffer);
        }
        else
        {
            HeapDeleteArray(maxLength, buffer);

            AssertOrFailFast(maxLength >= length);
            // JavascriptSharedArrayBuffer::Finalize will only report freeing `length`, we have to take care of the balance
            uint32 additionalSize = maxLength - length;
            if (additionalSize > 0)
            {
                Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
                recycler->ReportExternalMemoryFree(additionalSize);
            }
        }
    }
#endif

    WaiterList::WaiterList()
        : m_waiters(nullptr)
    {
        InitWaiterList();
    }

    void WaiterList::InitWaiterList()
    {
        Assert(m_waiters == nullptr);
        m_waiters = HeapNew(Waiters, &HeapAllocator::Instance);
    }

    void WaiterList::Cleanup()
    {
        if (m_waiters != nullptr)
        {
            HeapDelete(m_waiters);
            m_waiters = nullptr;
        }
    }

    bool WaiterList::Contains(DWORD_PTR agent)
    {
        Assert(m_waiters != nullptr);
        for (int i = 0; i < m_waiters->Count(); i++)
        {
            if (m_waiters->Item(i).identity == agent)
            {
                return true;
            }
        }

        return false;
    }

    bool _Requires_lock_held_(csForAccess.cs) WaiterList::AddAndSuspendWaiter(DWORD_PTR waiter, uint32 timeout)
    {
#ifdef _WIN32
        Assert(m_waiters != nullptr);
        Assert(waiter != NULL);
        Assert(!Contains(waiter));

        AgentOfBuffer agent(waiter, CreateEvent(NULL, TRUE, FALSE, NULL));
        m_waiters->Add(agent);

        csForAccess.Leave();
        DWORD result = WaitForSingleObject(agent.event, timeout);
        csForAccess.Enter();
        return result == WAIT_OBJECT_0;
#else
        // TODO for xplat
        return false;
#endif
    }

    void WaiterList::RemoveWaiter(DWORD_PTR waiter)
    {
#ifdef _WIN32
        Assert(m_waiters != nullptr);
        for (int i = m_waiters->Count() - 1; i >= 0; i--)
        {
            if (m_waiters->Item(i).identity == waiter)
            {
                CloseHandle(m_waiters->Item(i).event);
                m_waiters->RemoveAt(i);
                return;
            }
        }

        Assert(false);
#endif
        // TODO for xplat
    }

    uint32 WaiterList::RemoveAndWakeWaiters(int32 count, void* threadContext, AtomicsAsyncWaiterRegistration** asyncAwoken)
    {
        Assert(m_waiters != nullptr);
        Assert(count >= 0);
        uint32 removed = 0;
        for (int i = 0; i < m_waiters->Count() && count > 0; )
        {
            AgentOfBuffer agent = m_waiters->Item(i);
            if (agent.asyncRegistration != nullptr &&
                agent.asyncRegistration->threadContext != threadContext)
            {
                if (!agent.asyncRegistration->monitored)
                {
                    // Foreign async waiter whose host has no monitor task for
                    // it (no timer callback, not cross-runtime content, or
                    // TTD): nobody can resolve its record from this thread or
                    // wake the registrant, so keep the S6b contract — leave
                    // the waiter registered and do not count it. The check
                    // uses only native token fields under this lock; it never
                    // dereferences the record.
                    i++;
                    continue;
                }

                // Foreign async waiter with a live monitor task (S6e). Hand
                // off through the native token only, in this exact order:
                //   1. set the wake-request latch,
                //   2. remove the token from this shared list,
                //   3. linked = false — the notifier's FINAL token touch.
                // Ordering rationale: while linked == true, the only paths
                // that may delete the token (wrapper finalization via
                // CancelAsyncWaiters, the registrant's claim via
                // JavascriptSharedArrayBuffer::RemoveAsyncWaiter) must first
                // take this waiter-list critical section (finalization calls
                // WaiterList::RemoveAsyncWaiter for a linked token), so they
                // cannot free it while this thread holds the lock. No monitor
                // can claim between the latch and the unlink either: the
                // claim (TryClaimForWake) runs on the registrant's thread and
                // needs this same critical section; observing the latch early
                // only makes it block here until the unlink completes. After
                // step 3 the notifier never dereferences the token again, so
                // the finalizer's fast path (linked == false -> HeapDelete
                // without the CS) is safe. Setting the latch LAST, as an
                // earlier revision did, left a window where the finalizer
                // deleted the unlinked token between steps 2 and 3 and the
                // latch write touched freed memory.
                // The registrant's monitor resolves "ok" on its own thread;
                // this thread must not touch the record, the wrapper or any
                // GC-visible field, and never frees the token. The waiter
                // counts toward notify's return value. (A count of 1 can
                // still race an already-due timeout task on the registrant:
                // whoever claims first through `settled` wins exactly once,
                // and the promise may resolve "timed-out" after this notify
                // returned 1 — the spec's own concurrent-timeout ambiguity.)
                AtomicsAsyncWaiterRegistration* registration = agent.asyncRegistration;
                InterlockedExchangeAdd(&registration->wakeRequested, 1L);
                m_waiters->RemoveAt(i);
                registration->linked = false;
                count--; removed++;
                continue;
            }

            m_waiters->RemoveAt(i);
            count--; removed++;
            if (agent.asyncRegistration != nullptr)
            {
                // Single-thread async wait: hand the token to the caller, which
                // resolves its record after releasing this critical section.
                // The SAB wrapper keeps the record rooted until then. The
                // handoff list link is written BEFORE linked = false: the
                // finalizer's fast path may HeapDelete an unlinked token
                // without this CS, so linked = false must be this thread's
                // final token write (same discipline as the foreign branch).
                Assert(asyncAwoken != nullptr);
                AtomicsAsyncWaiterRegistration* registration = agent.asyncRegistration;
                registration->nextAwoken = *asyncAwoken;
                *asyncAwoken = registration;
                registration->linked = false;
            }
#ifdef _WIN32
            else
            {
                SetEvent(agent.event);
                // This agent will be closed when their respective call to wait has returned
            }
#endif
        }
        return removed;
    }

    void WaiterList::AddAsyncWaiter(AtomicsAsyncWaiterRegistration* registration)
    {
        Assert(m_waiters != nullptr);
        Assert(registration != nullptr);
        Assert(registration->record != nullptr);
        Assert(!registration->linked);
        registration->waiterList = this;
        m_waiters->Add(AgentOfBuffer(registration));
        registration->linked = true;
    }

    void WaiterList::RemoveAsyncWaiter(AtomicsAsyncWaiterRegistration* registration)
    {
        AutoCriticalSection autoCS(&csForAccess);
        if (m_waiters == nullptr)
        {
            return;
        }
        for (int i = m_waiters->Count() - 1; i >= 0; i--)
        {
            if (m_waiters->Item(i).asyncRegistration == registration)
            {
                m_waiters->RemoveAt(i);
                registration->linked = false;
                return;
            }
        }
    }

    void AtomicsAsyncWaiter::ResolveOk()
    {
        if (settled)
        {
            return;
        }
        settled = true;

        if (buffer != nullptr)
        {
            buffer->RemoveAsyncWaiter(this);
        }

        ResolveWith(_u("ok"));
    }

    void AtomicsAsyncWaiter::Abandon()
    {
        // The notifying thread is not the registrant's ScriptContext. Mutating
        // the owner wrapper's GC-visible list from here would be a cross-thread
        // write, so only mark the record settled; the wrapper keeps it rooted
        // until finalization and the promise intentionally stays pending.
        settled = true;
    }

    void AtomicsAsyncWaiter::SetRegistration(AtomicsAsyncWaiterRegistration* registration)
    {
        // Called under the waiter-list lock, after WaiterList::AddAsyncWaiter
        // set registration->waiterList. Plain non-traced links: the token is
        // native-heap owned by the wrapper; the task roots the whole chain
        // (task -> record -> wrapper -> token/SharedContents), so the links
        // stay valid while the record is live and unsettled.
        Assert(registration != nullptr);
        Assert(registration->waiterList != nullptr);
        this->registration = registration;
    }

    bool AtomicsAsyncWaiter::TryClaimForTimeout()
    {
        // Runs on the registrant's script thread from the host-delayed timeout
        // task. `settled` is checked before any dereference of the cached
        // native links: the paths that release the token while this task can
        // still run are notify resolution and cross-agent Abandon, and both
        // set settled first. Wrapper finalization deletes tokens WITHOUT
        // setting settled, but it cannot race a live task: the host-held task
        // roots the record, which roots the wrapper, so an unlinked task's
        // wrapper is never finalized (the stale task is simply a no-op).
        if (settled || registration == nullptr)
        {
            return false;
        }

        WaiterList* waiterList = registration->waiterList;
        Assert(waiterList != nullptr);
        AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());

        // Re-check under the lock: the notify path removes the token and
        // settles the record inside this same critical section.
        if (settled)
        {
            return false;
        }

        settled = true;

        // Unlink while holding the lock so a later notify counts 0 and cannot
        // resolve. csForAccess is recursive, so WaiterList::RemoveAsyncWaiter
        // re-entry below is safe. JavascriptSharedArrayBuffer::RemoveAsyncWaiter
        // deletes the token; nothing may touch `registration` afterwards.
        waiterList->RemoveAsyncWaiter(registration);
        if (buffer != nullptr)
        {
            buffer->RemoveAsyncWaiter(this);
        }
        registration = nullptr;

        return true;
    }

    void AtomicsAsyncWaiter::ResolveTimedOutAfterClaim()
    {
        // Claim (settle + unlink) already happened under the waiter-list lock
        // in TryClaimForTimeout; resolution runs strictly outside it.
        Assert(settled);
        ResolveWith(_u("timed-out"));
    }

    Var AtomicsAsyncWaiter::EntryTimeoutTask(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        AtomicsWaitAsyncTimeoutTaskFunction* task = VarTo<AtomicsWaitAsyncTimeoutTaskFunction>(function);
        AtomicsAsyncWaiter* waiter = task->GetWaiter();
        if (waiter->TryClaimForTimeout())
        {
            waiter->ResolveTimedOutAfterClaim();
        }
        // else: notify (or cross-agent Abandon) already settled the waiter;
        // the late timeout task no-ops without touching the released token.
        return function->GetScriptContext()->GetLibrary()->GetUndefined();
    }

    bool AtomicsAsyncWaiter::IsWakeRequested() const
    {
        // Registrant thread only, and only while `registration` is live (the
        // monitor task roots record -> wrapper -> token). Interlocked RMW as
        // an atomic read with a full barrier, mirroring the writer side.
        Assert(registration != nullptr);
        return InterlockedExchangeAdd(&registration->wakeRequested, 0L) != 0;
    }

    bool AtomicsAsyncWaiter::TryClaimForWake()
    {
        // Runs on the registrant's script thread from the host-delayed monitor
        // task after the token's wake-request latch was observed set by a
        // foreign notify. Same arbitration as TryClaimForTimeout: `settled` is
        // checked before any dereference of the cached native links, so a
        // waiter already settled by a timeout claim (or a same-thread notify)
        // no-ops without touching the released token. On success the settle
        // and both unlinks happen under the waiter-list lock; the token is
        // released by JavascriptSharedArrayBuffer::RemoveAsyncWaiter on this
        // (the registrant's) thread — the foreign notifying thread never
        // frees it, so there is no double delete.
        if (settled || registration == nullptr)
        {
            return false;
        }

        WaiterList* waiterList = registration->waiterList;
        Assert(waiterList != nullptr);
        AutoCriticalSection autoCS(waiterList->GetCriticalSectionForAccess());

        if (settled)
        {
            return false;
        }

        settled = true;

        // The foreign notify already unlinked the token from the shared list
        // (that is what the latch means), but a same-thread notify could not
        // have: it would have handed the token to its own resolution path.
        // RemoveAsyncWaiter is idempotent — if the token is somehow no longer
        // linked, it returns without touching it.
        waiterList->RemoveAsyncWaiter(registration);
        if (buffer != nullptr)
        {
            buffer->RemoveAsyncWaiter(this);
        }
        registration = nullptr;

        return true;
    }

    void AtomicsAsyncWaiter::ResolveOkAfterClaim()
    {
        // Claim (settle + unlink) already happened under the waiter-list lock
        // in TryClaimForWake; resolution runs strictly outside it.
        Assert(settled);
        ResolveWith(_u("ok"));
    }

    Var AtomicsAsyncWaiter::EntryMonitorTask(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));

        AtomicsWaitAsyncMonitorTaskFunction* task = VarTo<AtomicsWaitAsyncMonitorTaskFunction>(function);
        AtomicsAsyncWaiter* waiter = task->GetWaiter();
        JavascriptLibrary* library = function->GetScriptContext()->GetLibrary();

        // Stop immediately when the waiter settled (same-thread notify or a
        // timeout claim won) or the registration is gone: the token may
        // already be released, so `settled`/`registration` are checked before
        // any use of the cached links.
        if (waiter->IsSettled() || !waiter->HasRegistration())
        {
            return library->GetUndefined();
        }

        if (waiter->IsWakeRequested())
        {
            // A foreign notify requested the wake. Claim on this (the
            // registrant's) thread: settle + unlink + release the token under
            // the waiter-list lock, then resolve "ok" outside it. A failed
            // claim means a timeout won in between — stop either way; the
            // woken path never reschedules.
            if (waiter->TryClaimForWake())
            {
                waiter->ResolveOkAfterClaim();
            }
            return library->GetUndefined();
        }

        // Still pending and not woken: reschedule this task at the bounded
        // poll interval. Re-enqueueing the running task hands it to the host
        // timer callback again (one more AddRef/Release cycle on the host
        // side), exactly like a fresh task, and keeps the
        // task -> record -> wrapper -> token root chain alive.
        library->EnqueueDelayedTask(function, MonitorPollIntervalMs);
        return library->GetUndefined();
    }

    template <size_t N>
    void AtomicsAsyncWaiter::ResolveWith(const char16 (&outcome)[N])
    {
        // Shared settle tail. Callers have settled and unlinked the waiter;
        // resolution happens outside the waiter-list critical section because
        // it allocates and enqueues promise reactions.
        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptPromise::TryCallResolveOrRejectHandler(
            capability->GetResolve(),
            library->CreateStringFromCppLiteral(outcome),
            scriptContext);
    }

    bool AgentOfBuffer::AgentCanSuspend(ScriptContext *scriptContext)
    {
        // Currently we are allowing every agent (including main page) to suspend. If we want only web-worker to suspend un-comment below code.
        // return scriptContext != nullptr && scriptContext->webWorkerId != Constants::NonWebWorkerContextId;

        return true;
    }
}
