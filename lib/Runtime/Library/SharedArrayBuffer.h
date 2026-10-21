//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//  Implements SharedArrayBuffer according to http://tc39.github.io/ecmascript_sharedmem/shmem.html
//----------------------------------------------------------------------------

#pragma once
namespace Js
{

    class WaiterList;
    class SharedArrayBuffer;
    class JavascriptSharedArrayBuffer;
    class JavascriptPromiseCapability;
    class AtomicsAsyncWaiter;
    typedef JsUtil::List<DWORD_PTR, HeapAllocator> SharableAgents;
    typedef JsUtil::BaseDictionary<uint, WaiterList *, HeapAllocator> IndexToWaitersMap;

    // One pending Atomics.waitAsync registration. Allocation domain: Recycler
    // (RecyclerNew, normal heap). Owner: the JavascriptSharedArrayBuffer that
    // roots it through asyncWaiters. The native registration node (below) is
    // the only token the SAB finalizer may touch; it never dereferences this
    // record, because an unmarked wrapper is not scanned and its record can be
    // swept before the wrapper's finalizer runs.
    class AtomicsAsyncWaiter : public FinalizableObject
    {
    public:
        AtomicsAsyncWaiter(JavascriptSharedArrayBuffer* buffer, JavascriptPromiseCapability* capability, ScriptContext* scriptContext)
            : buffer(buffer), capability(capability), scriptContext(scriptContext),
              nextWrapperWaiter(nullptr), settled(false)
        {
        }

        JavascriptSharedArrayBuffer* GetBuffer() const { return buffer; }
        JavascriptPromiseCapability* GetCapability() const { return capability; }
        ScriptContext* GetScriptContext() const { return scriptContext; }

        AtomicsAsyncWaiter* GetNextWrapperWaiter() const { return nextWrapperWaiter; }
        void SetNextWrapperWaiter(AtomicsAsyncWaiter* next) { nextWrapperWaiter = next; }

        bool IsSettled() const { return settled; }

        // Resolve the promise with "ok" and unlink from the owner/waiter list.
        void ResolveOk();
        // Mark a woken waiter whose promise cannot be resolved on this thread
        // (multi-agent; unsupported in this slice) without touching the owner
        // wrapper from the notifying thread.
        void Abandon();

        virtual void Finalize(bool isShutdown) override {}
        virtual void Dispose(bool isShutdown) override {}
        virtual void Mark(Recycler* recycler) override
        {
            AssertMsg(false, "AtomicsAsyncWaiter is allocated in the conservatively scanned normal heap");
        }

    private:
        Field(JavascriptSharedArrayBuffer*) buffer;
        Field(JavascriptPromiseCapability*) capability;
        Field(AtomicsAsyncWaiter*) nextWrapperWaiter;
        ScriptContext* scriptContext;
        bool settled;
    };

    // Native token owned by the wrapper and stored in the SharedContents waiter
    // list. HeapAllocator-owned; the wrapper's finalizer deletes it without
    // dereferencing the (possibly already swept) AtomicsAsyncWaiter record.
    struct AtomicsAsyncWaiterRegistration
    {
        AtomicsAsyncWaiter* record;
        WaiterList* waiterList;
        ScriptContext* scriptContext;   // cached, never read through the record
        void* threadContext;            // cached ThreadContext*, compared opaquely
        AtomicsAsyncWaiterRegistration* nextWrapper;
        AtomicsAsyncWaiterRegistration* nextAwoken;
        bool linked;
    };

    class SharedContents
    {
    public:
        BYTE  *buffer;             // Points to a heap allocated RGBA buffer, can be null
        IndexToWaitersMap *indexToWaiterList;  // Map of agents waiting on a particular index.
        uint32 bufferLength;       // Current [[ArrayBufferByteLength]]; growable buffers update this
        uint32 maxBufferLength = 0; // Maximum number of bytes: growable limit or WebAssembly maximum
        bool isWebAssembly = false;
        bool isGrowable = false;   // Growable (ES2024) JS SharedArrayBuffer; capacity is maxBufferLength
    private:
        // Addref/release counter for current buffer, this is needed as the current buffer will be shared among different workers
        long refCount;

    public:
        long AddRef();
        long Release();
        bool IsWebAssembly() const { return isWebAssembly; }
        void SetIsWebAssembly() { isWebAssembly = true; }
        static int GetBufferOffset() { return offsetof(SharedContents, buffer); }
        static int GetBufferLengthOffset() { return offsetof(SharedContents, bufferLength); }

#if DBG
        // This is mainly used for validation purpose as the wait/wake APIs should be used on the agents (Workers) among which this buffer is shared.
        SharableAgents *allowedAgents;
        CriticalSection csAgent;
        void AddAgent(DWORD_PTR agent);
        bool IsValidAgent(DWORD_PTR agent);
#endif

        void Cleanup();

        SharedContents(BYTE* b, uint32 l, uint32 m)
            : buffer(b), bufferLength(l), maxBufferLength(m), refCount(1), indexToWaiterList(nullptr)
#if DBG
            , allowedAgents(nullptr)
#endif
        {
        }
    };

    class SharedArrayBuffer : public ArrayBufferBase
    {
    public:
        DEFINE_VTABLE_CTOR_ABSTRACT(SharedArrayBuffer, ArrayBufferBase);
        SharedArrayBuffer(DynamicType * type);
        SharedArrayBuffer(SharedContents *contents, DynamicType * type);

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Slice;
            static FunctionInfo GetterByteLength;
            static FunctionInfo GetterMaxByteLength;
            static FunctionInfo GetterGrowable;
            static FunctionInfo Grow;
            static FunctionInfo GetterSymbolSpecies;
        };

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySlice(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterMaxByteLength(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterGrowable(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGrow(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterSymbolSpecies(RecyclableObject* function, CallInfo callInfo, ...);

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;
        virtual BOOL GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;

        virtual uint32 GetByteLength() const override;
        virtual BYTE* GetBuffer() const override;

        // ES2024 growable SharedArrayBuffer. A growable buffer owns a capacity of
        // maxBufferLength bytes and only changes bufferLength on grow; the base
        // pointer is never remapped, so views stay valid. Shared buffers are not
        // detachable, so there is no invalidation path here.
        bool IsGrowable() const { return sharedContents != nullptr && sharedContents->isGrowable; }
        uint32 GetMaxByteLength() const
        {
            if (sharedContents == nullptr)
            {
                return 0;
            }
            return sharedContents->isGrowable ? sharedContents->maxBufferLength : sharedContents->bufferLength;
        }
        void Grow(uint32 newByteLength);

        static int GetByteLengthOffset() { Assert(false); return 0; }
        static int GetBufferOffset() { Assert(false); return 0; }
        static int GetSharedContentsOffset() { return offsetof(SharedArrayBuffer, sharedContents); }

        virtual bool IsArrayBuffer() override { return false; }
        virtual bool IsSharedArrayBuffer() override { return true; }
        virtual ArrayBuffer * GetAsArrayBuffer() { return nullptr; }
        virtual SharedArrayBuffer * GetAsSharedArrayBuffer() override;

        WaiterList *GetWaiterList(uint index);
        SharedContents *GetSharedContents() { return sharedContents; }

#if defined(TARGET_64)
        //maximum 2G -1  for amd64
        static const uint32 MaxSharedArrayBufferLength = 0x7FFFFFFF;
#else
        // maximum 1G to avoid arithmetic overflow.
        static const uint32 MaxSharedArrayBufferLength = 1 << 30;
#endif
        virtual bool IsValidVirtualBufferLength(uint length) const;

    protected:
        // maxLength is the allocation extent for growable JS buffers and the
        // grow limit for WebAssemblySharedArrayBuffer.
        // Must call after constructor of child class is completed. Required to be able to make correct virtual calls
        void Init(uint32 length, uint32 maxLength, bool isGrowable = false);
        virtual BYTE* AllocBuffer(uint32 length, uint32 maxLength, bool isGrowable);
        virtual void FreeBuffer(BYTE* buffer, uint32 length, uint32 maxLength, bool isGrowable);

        FieldNoBarrier(SharedContents *) sharedContents;

        static CriticalSection csSharedArrayBuffer;
    };

    template <> inline bool VarIsImpl<SharedArrayBuffer>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_SharedArrayBuffer;
    }

    class JavascriptSharedArrayBuffer : public SharedArrayBuffer
    {
    protected:
        DEFINE_VTABLE_CTOR(JavascriptSharedArrayBuffer, SharedArrayBuffer);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptSharedArrayBuffer);

    public:
        static JavascriptSharedArrayBuffer* Create(uint32 length, DynamicType * type);
        static JavascriptSharedArrayBuffer* Create(uint32 length, uint32 maxByteLength, bool isGrowable, DynamicType * type);
        static JavascriptSharedArrayBuffer* Create(SharedContents *sharedContents, DynamicType * type);
        virtual void Dispose(bool isShutdown) override;
        virtual void Finalize(bool isShutdown) override;

        // Async waiters registered through this wrapper. The list is rooted here
        // (the wrapper's finalizable-heap object is conservatively scanned) so a
        // pending promise and its capability survive until notify or finalize.
        // SharedContents waiter lists store native registration tokens, not
        // owning references.
        AtomicsAsyncWaiterRegistration* AddAsyncWaiter(AtomicsAsyncWaiter* waiter, ScriptContext* scriptContext, void* threadContext)
        {
            Assert(waiter != nullptr);
            AtomicsAsyncWaiterRegistration* registration = HeapNew(AtomicsAsyncWaiterRegistration);
            registration->record = waiter;
            registration->waiterList = nullptr; // set by WaiterList::AddAsyncWaiter
            registration->scriptContext = scriptContext;
            registration->threadContext = threadContext;
            registration->nextWrapper = asyncRegistrations;
            registration->nextAwoken = nullptr;
            registration->linked = false;
            asyncRegistrations = registration;

            waiter->SetNextWrapperWaiter(asyncWaiters);
            asyncWaiters = waiter;
            return registration;
        }

        void RemoveAsyncWaiter(AtomicsAsyncWaiter* waiter)
        {
            AtomicsAsyncWaiter* current = asyncWaiters;
            AtomicsAsyncWaiter* previous = nullptr;
            while (current != nullptr)
            {
                if (current == waiter)
                {
                    if (previous == nullptr)
                    {
                        asyncWaiters = current->GetNextWrapperWaiter();
                    }
                    else
                    {
                        previous->SetNextWrapperWaiter(current->GetNextWrapperWaiter());
                    }
                    break;
                }
                previous = current;
                current = current->GetNextWrapperWaiter();
            }

            AtomicsAsyncWaiterRegistration* registration = asyncRegistrations;
            AtomicsAsyncWaiterRegistration* previousRegistration = nullptr;
            while (registration != nullptr)
            {
                if (registration->record == waiter)
                {
                    if (previousRegistration == nullptr)
                    {
                        asyncRegistrations = registration->nextWrapper;
                    }
                    else
                    {
                        previousRegistration->nextWrapper = registration->nextWrapper;
                    }
                    HeapDelete(registration);
                    return;
                }
                previousRegistration = registration;
                registration = registration->nextWrapper;
            }
            Assert(false);
        }

        // Finalizer path: unlink every native registration from its shared
        // waiter list and release the token. Never dereferences the GC record.
        void CancelAsyncWaiters();

    protected:
        JavascriptSharedArrayBuffer(DynamicType * type);
        JavascriptSharedArrayBuffer(SharedContents *sharedContents, DynamicType * type);

    private:
        // Root for Atomics.waitAsync records registered through this wrapper.
        Field(AtomicsAsyncWaiter*) asyncWaiters;
        AtomicsAsyncWaiterRegistration* asyncRegistrations;
    };

#ifdef ENABLE_WASM_THREADS
    class WebAssemblySharedArrayBuffer : public JavascriptSharedArrayBuffer
    {
    protected:
        DEFINE_VTABLE_CTOR(WebAssemblySharedArrayBuffer, JavascriptSharedArrayBuffer);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(WebAssemblySharedArrayBuffer);

    public:
        static WebAssemblySharedArrayBuffer* Create(uint32 length, uint32 maxLength, DynamicType * type);
        static WebAssemblySharedArrayBuffer* Create(SharedContents *sharedContents, DynamicType * type);

        virtual bool IsValidVirtualBufferLength(uint length) const override;
        virtual bool IsWebAssemblyArrayBuffer() override { return true; }
        _Must_inspect_result_ bool GrowMemory(uint32 newBufferLength);

    protected:
        virtual BYTE* AllocBuffer(uint32 length, uint32 maxLength, bool isGrowable) override;
        virtual void FreeBuffer(BYTE* buffer, uint32 length, uint32 maxLength, bool isGrowable) override;

    private:
        WebAssemblySharedArrayBuffer(DynamicType * type);
        WebAssemblySharedArrayBuffer(SharedContents *sharedContents, DynamicType * type);
        void ValidateBuffer();
    };

    template <> inline bool VarIsImpl<WebAssemblySharedArrayBuffer>(RecyclableObject* obj)
    {
        return VarIs<SharedArrayBuffer>(obj) && UnsafeVarTo<SharedArrayBuffer>(obj)->IsWebAssemblyArrayBuffer();
    }
#endif

    // An agent can be viewed as a worker
    struct AgentOfBuffer
    {
    public:
        AgentOfBuffer() :identity(NULL), event(NULL), asyncRegistration(nullptr) {}
        AgentOfBuffer(DWORD_PTR agent, HANDLE e) :identity(agent), event(e), asyncRegistration(nullptr) {}
        AgentOfBuffer(AtomicsAsyncWaiterRegistration* registration) :identity(NULL), event(NULL), asyncRegistration(registration) {}
        static bool AgentCanSuspend(ScriptContext *scriptContext);

        DWORD_PTR identity;
        HANDLE event;
        AtomicsAsyncWaiterRegistration* asyncRegistration; // non-owning; owner is JavascriptSharedArrayBuffer
    };

    typedef JsUtil::List<AgentOfBuffer, HeapAllocator> Waiters;
    class WaiterList
    {
    public:

        WaiterList();
        void Cleanup();

        bool _Requires_lock_held_(csForAccess.cs) AddAndSuspendWaiter(DWORD_PTR waiter, uint32 timeout);
        void RemoveWaiter(DWORD_PTR waiter);
        // Notify pops up to `count` same-ThreadContext entries. Async entries
        // are linked into *asyncAwoken through their nextAwoken field (no
        // allocation while the critical section is held); their promises are
        // resolved by the caller after the lock is released. Async waiters
        // registered on another thread are left in place. Same-thread
        // cross-realm waiters are woken.
        uint32 RemoveAndWakeWaiters(int32 count, void* threadContext, AtomicsAsyncWaiterRegistration** asyncAwoken = nullptr);
        void AddAsyncWaiter(AtomicsAsyncWaiterRegistration* registration);
        void RemoveAsyncWaiter(AtomicsAsyncWaiterRegistration* registration);

        CriticalSection * GetCriticalSectionForAccess() { return &csForAccess; }

    private:
        void InitWaiterList();
        bool Contains(DWORD_PTR agent);

        Waiters * m_waiters;

        // Below CS is used for synchronizing access in wait/wake API
        CriticalSection csForAccess;
    };
}

