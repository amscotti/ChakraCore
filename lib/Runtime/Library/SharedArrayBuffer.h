//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
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
    struct AtomicsAsyncWaiterRegistration;
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
              nextWrapperWaiter(nullptr), settled(false), registration(nullptr)
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
        // Defensive tail only, and unreachable for foreign waiters since
        // S6e: a foreign notify of a monitored waiter hands off through the
        // token latch (the registrant's monitor task resolves it), and an
        // unmonitored foreign waiter is skipped outright. If it ever runs,
        // it marks the record settled WITHOUT releasing the native
        // registration token or touching the owner wrapper — the token is
        // released exclusively on the registrant's thread (claim paths) or
        // by wrapper finalization.
        void Abandon();

        // Finite-timeout/cross-agent support (S6d/S6e). Cache the native
        // registration links so the host-delayed timeout and monitor tasks
        // can claim the waiter. Called under the waiter-list lock, after
        // WaiterList::AddAsyncWaiter published the waiterList pointer on the
        // token.
        void SetRegistration(AtomicsAsyncWaiterRegistration* registration);
        // Timeout-task claim: settled -> false (late task no-ops without
        // dereferencing the released token); otherwise settle and unlink under
        // the waiter-list lock so a later notify counts 0 and cannot resolve.
        // All claim/settle mutations run on the registrant's script thread.
        bool TryClaimForTimeout();
        // Resolve the promise with "timed-out" after a successful claim; must
        // run outside the waiter-list critical section (resolution allocates
        // and enqueues promise reactions).
        void ResolveTimedOutAfterClaim();
        // Host-delayed timeout task entry point; sees the task function.
        static Var EntryTimeoutTask(RecyclableObject* function, CallInfo callInfo, ...);

        // Cross-agent wake support (S6e). A foreign notify (another
        // ThreadContext) removes the native token from the shared waiter list
        // and atomically sets the token's wake-request flag; the registrant's
        // host-delayed monitor task — scheduled when the shared content is
        // cross-runtime — polls that flag and performs the claim/unlink/
        // release and the "ok" resolution on the registrant's thread only.
        // Bounded poll interval for the rescheduling monitor, in milliseconds.
        static const unsigned int MonitorPollIntervalMs = 2;
        // Whether the cached native registration links are still live. Only
        // meaningful on the registrant thread; see the registration field.
        bool HasRegistration() const { return registration != nullptr; }
        // Atomic read of the token's wake-request flag. Only valid while the
        // registration is live (HasRegistration), on the registrant thread.
        bool IsWakeRequested() const;
        // Monitor claim: wake-request observed -> settle, unlink and release
        // the token under the waiter-list lock (registrant thread only).
        bool TryClaimForWake();
        // Wake-claim body for callers already holding the waiter-list CS
        // (it is a recursive CriticalSection, so re-entry from the pass-order
        // walk below is safe). Split from TryClaimForWake so the FIFO walk
        // can claim predecessors while it holds the CS.
        bool TryClaimForWakeUnderCS();
        // FIFO pass-order wake claim (S6e conformance repair): walk this
        // token's notify-pass chain from its head and claim the first
        // unsettled same-ThreadContext chain member — this waiter itself or
        // a FIFO predecessor. Returns true with *claimed set to the claimed
        // record; the caller resolves it outside the CS. See EntryMonitorTask
        // for the ordering contract.
        bool TryClaimWakeInPassOrder(AtomicsAsyncWaiter** claimed);
        // Resolve the promise with "ok" after a successful wake claim; must
        // run outside the waiter-list critical section.
        void ResolveOkAfterClaim();
        // Host-delayed monitor task entry point; sees the task function.
        static Var EntryMonitorTask(RecyclableObject* function, CallInfo callInfo, ...);

        // Shared settle tail: resolve the capability with the given outcome
        // literal. Callers have settled and unlinked; runs outside the
        // waiter list lock.
        template <size_t N> void ResolveWith(const char16 (&outcome)[N]);

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

        // Plain (non-traced) native links to the registration token, cached at
        // registration. Only read by TryClaimForTimeout/TryClaimForWake on the
        // registrant's script thread after checking settled: notify resolution
        // and cross-agent Abandon set settled before releasing the token, and
        // SAB finalization — which deletes tokens without setting settled —
        // cannot race a live task, because the task roots the record, which
        // roots the wrapper that owns the token and the SharedContents
        // (waiter list). The chain makes every cached pointer valid while the
        // record is live.
        AtomicsAsyncWaiterRegistration* registration;
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
        // S6e cross-agent handoff. `monitored` is written under the
        // waiter-list lock: false at registration, flipped to true only
        // after the monitor task was successfully created and enqueued
        // (task-creation OOM keeps it false, so a foreign notify then skips
        // the waiter — the S6b fallback — instead of counting a wake nobody
        // can resolve). Foreign notifiers read it under the same lock.
        // `wakeRequested` is an atomic latch: a foreign notify sets it
        // (under the waiter-list lock) BEFORE removing the token from the
        // shared list, and the registrant's monitor task polls it without
        // the lock and claims under the lock. A foreign thread never
        // dereferences `record` or any other GC-visible field through this
        // token, and never frees the token.
        bool monitored;
        volatile long wakeRequested;

        // FIFO resolution order (S6e conformance repair). A foreign notify
        // pass latches up to `count` foreign tokens in waiter-list (FIFO)
        // order and links exactly those tokens into a per-pass chain,
        // stamped at latch time under the waiter-list CS: passPrev points to
        // the previously latched token of the same pass, passNext to the next
        // one. The registrant's monitor tasks walk that chain (under the same
        // CS) and claim/resolve same-ThreadContext predecessors before
        // themselves, so promise resolutions happen in notify FIFO order
        // regardless of which monitor timer fires first. Other-ThreadContext
        // chain members are skipped by the walk: they resolve on their own
        // registrant threads, and relative order across agents is
        // unobservable (each agent drains its own microtask queue). All link
        // reads/writes happen under the waiter-list CS of the list the pass
        // ran on; a token leaves the chain — SpliceFromPassChain — under that
        // same CS on every path that releases it (wake/timeout claim, wrapper
        // finalization), so walkers only ever see live tokens. The notifier's
        // passNext stamp for the next sibling happens after the previous
        // sibling set linked = false, but inside the same single CS hold:
        // every path that can observe linked == false (finalizer) or free
        // the token (registrant claims) must first acquire this CS, so the
        // sibling cannot be released in between.
        AtomicsAsyncWaiterRegistration* passPrev;
        AtomicsAsyncWaiterRegistration* passNext;

        // Requires the waiter-list CS of the list this token's pass ran on.
        void SpliceFromPassChain()
        {
            if (passPrev == nullptr && passNext == nullptr)
            {
                return; // never latched into a pass, or already spliced
            }
            if (passPrev != nullptr)
            {
                passPrev->passNext = passNext;
            }
            if (passNext != nullptr)
            {
                passNext->passPrev = passPrev;
            }
            passPrev = nullptr;
            passNext = nullptr;
        }
    };

    // Host-delayed timeout task for a pending Atomics.waitAsync with a finite
    // positive timeout. Recycler-owned RuntimeFunction mirroring the
    // FinalizationRegistry cleanup task: the host queue AddRefs the task until
    // it has called or dropped it, which roots the record -> wrapper -> native
    // registration token (and the SharedContents waiter list), so the record's
    // cached native links stay valid for the task's whole lifetime. The only
    // caller is the host that received the task inside
    // JavascriptLibrary::EnqueueDelayedTask; the engine never invokes it and
    // spawns no thread.
    class AtomicsWaitAsyncTimeoutTaskFunction : public RuntimeFunction
    {
    protected:
        DEFINE_VTABLE_CTOR(AtomicsWaitAsyncTimeoutTaskFunction, RuntimeFunction);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(AtomicsWaitAsyncTimeoutTaskFunction);

    public:
        AtomicsWaitAsyncTimeoutTaskFunction(
            DynamicType* type,
            FunctionInfo* functionInfo,
            AtomicsAsyncWaiter* waiter)
            : RuntimeFunction(type, functionInfo), waiter(waiter)
        {
        }

        AtomicsAsyncWaiter* GetWaiter() const { return waiter; }

    private:
        Field(AtomicsAsyncWaiter*) waiter;
    };

    template <> bool VarIsImpl<AtomicsWaitAsyncTimeoutTaskFunction>(RecyclableObject* obj);

    // Host-delayed monitor task for a pending cross-agent Atomics.waitAsync
    // (S6e). Ownership/rooting model (mirrors the timeout task): the host
    // queue AddRefs the task, which is a GC root; the task's traced waiter
    // Field keeps the record alive, the record's traced buffer Field keeps
    // the wrapper alive, and the wrapper in turn OWNS the native
    // registration token and holds the SharedContents reference (native
    // ownership, not GC rooting — nothing traces through the token). Hence
    // the record's cached native links stay valid for the task's whole
    // lifetime. Re-enqueues itself at
    // AtomicsAsyncWaiter::MonitorPollIntervalMs while the waiter is pending
    // and not wake-requested; stops as soon as the waiter settles or the
    // registration is released. Only ever called by the host that received it
    // on the registrant's thread.
    class AtomicsWaitAsyncMonitorTaskFunction : public RuntimeFunction
    {
    protected:
        DEFINE_VTABLE_CTOR(AtomicsWaitAsyncMonitorTaskFunction, RuntimeFunction);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(AtomicsWaitAsyncMonitorTaskFunction);

    public:
        AtomicsWaitAsyncMonitorTaskFunction(
            DynamicType* type,
            FunctionInfo* functionInfo,
            AtomicsAsyncWaiter* waiter)
            : RuntimeFunction(type, functionInfo), waiter(waiter)
        {
        }

        AtomicsAsyncWaiter* GetWaiter() const { return waiter; }

    private:
        Field(AtomicsAsyncWaiter*) waiter;
    };

    template <> bool VarIsImpl<AtomicsWaitAsyncMonitorTaskFunction>(RecyclableObject* obj);

    class SharedContents
    {
    public:
        BYTE  *buffer;             // Points to a heap allocated RGBA buffer, can be null
        IndexToWaitersMap *indexToWaiterList;  // Map of agents waiting on a particular index.
        uint32 bufferLength;       // Current [[ArrayBufferByteLength]]; growable buffers update this
        uint32 maxBufferLength = 0; // Maximum number of bytes: growable limit or WebAssembly maximum
        bool isWebAssembly = false;
        bool isGrowable = false;   // Growable (ES2024) JS SharedArrayBuffer; capacity is maxBufferLength

        // S6e: this content was (or may be) shared with another runtime — a
        // host extracted a content handle (JsGetSharedArrayBufferContent) or
        // a wrapper was created from such a handle
        // (JsCreateSharedArrayBufferWithSharedContent / SCA deserialization).
        // Atomics.waitAsync on cross-runtime content may be notified from
        // another thread, so the registrant schedules a monitor task (see
        // AtomicsObject::EntryWaitAsync); ordinary single-runtime waitAsync
        // never gains polling. One-way false->true transition; the RMW keeps
        // the flag atomic, and the host's broadcast handshake (events /
        // semaphore) orders the write against the receiving runtime's reads.
        // Returns true when the contents were ALREADY marked, so a caller can
        // run first-share work (retro-monitoring pre-share waiters) exactly
        // once.
        bool MarkCrossRuntime();
        bool IsCrossRuntime();

    private:
        volatile long crossRuntime;
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
            : buffer(b), bufferLength(l), maxBufferLength(m), crossRuntime(0), refCount(1), indexToWaiterList(nullptr)
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

        // S6e pre-share support: called when this wrapper's contents are
        // marked cross-runtime for the FIRST time (content-handle
        // extraction). Overridden by JavascriptSharedArrayBuffer to
        // retroactively schedule monitor tasks for async waiters that were
        // registered while the buffer was still single-runtime — without
        // this they would stay deaf to foreign notifies forever. Runs on
        // the extracting thread, which for a first extraction is the
        // registrant's own runtime/thread.
        virtual void MonitorPendingAsyncWaiters() { }

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
        // owning references. `monitored` records whether a monitor task was
        // scheduled for this registration (cross-runtime content + host timer
        // callback): it decides whether a foreign notify may hand off through
        // the token's wake-request latch.
        AtomicsAsyncWaiterRegistration* AddAsyncWaiter(AtomicsAsyncWaiter* waiter, ScriptContext* scriptContext, void* threadContext, bool monitored)
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
            registration->monitored = monitored;
            registration->wakeRequested = 0;
            registration->passPrev = nullptr;
            registration->passNext = nullptr;
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

        // S6e pre-share support (see SharedArrayBuffer::MonitorPendingAsyncWaiters):
        // schedule monitor tasks for async waiters registered before the
        // contents were marked cross-runtime, then publish their tokens as
        // monitored. Must run on the registrant's thread (first extraction
        // guarantees this: only the extracting runtime owns wrappers with
        // waiters at that point).
        virtual void MonitorPendingAsyncWaiters() override;

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
        // Notify pops up to `count` entries in waiter-list (FIFO) order.
        // Same-ThreadContext async entries are linked into *asyncAwoken
        // through their nextAwoken field (no allocation while the critical
        // section is held); their promises are resolved by the caller after
        // the lock is released. A foreign-ThreadContext async entry whose
        // token has a live monitor task (monitored) is removed from the list,
        // counted, chained to the pass's previously latched token (passPrev/
        // passNext, so the registrant's monitors can resolve the pass in FIFO
        // order — see AtomicsAsyncWaiter::TryClaimWakeInPassOrder), and its
        // token's wake-request latch is set atomically — the registrant's
        // monitor task claims and resolves it on its own thread; this thread
        // never touches the record/wrapper. An unmonitored foreign entry is
        // left registered and uncounted (S6b behavior for hosts without the
        // timer callback / cross-runtime opt-in). Same-thread cross-realm
        // waiters are woken.
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

