//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    enum class DisposeHint : uint8
    {
        SyncDispose = 0,
        AsyncDispose = 1,
    };

    struct DisposableResourceRecord
    {
        DisposableResourceRecord(
            Var resource,
            Var disposeMethod,
            DisposeHint hint,
            bool needsSyncToAsyncWrap,
            bool passResourceAsArgument,
            DisposableResourceRecord* next)
            : next(next)
            , resource(resource)
            , disposeMethod(disposeMethod)
            , hint(hint)
            , needsSyncToAsyncWrap(needsSyncToAsyncWrap)
            , passResourceAsArgument(passResourceAsArgument)
        {
        }

        Field(DisposableResourceRecord*) next;
        Field(Var) resource;
        Field(Var) disposeMethod;
        DisposeHint hint;
        bool needsSyncToAsyncWrap;
        bool passResourceAsArgument;
    };

    // Ownership & GC lifetime record (see doc/memory-management.md section 3):
    // - Resource / creation: DisposeCapability + DisposableResourceRecord list allocated
    //   via RecyclerNew in DisposeCapability::New / PushRecord (JavascriptDisposableStack.cpp).
    // - Domain & allocator: Recycler heap (DynamicObject subclass; all pointer/Var/state
    //   fields use Field(...) write barriers, traced by Recycler).
    // - Physical layout / views: singly-linked LIFO stack of DisposableResourceRecord nodes
    //   (no borrowed native buffers or unmanaged pointers).
    // - Owner & transfer: active JS frame register (`using` / `await using` opcodes),
    //   JavascriptDisposableStack, JavascriptAsyncDisposableStack, or an active
    //   JavascriptAsyncDisposableStackStepFunction closure stored in Promise reactions.
    //   MoveToNew transfers `head` ownership to a fresh DisposeCapability and clears `head`
    //   on the source capability; `disposed` transitions to true before disposal/move.
    // - Invalidation & reentrancy: `InvokeDisposeMethod` and promise resolution can reenter
    //   arbitrary JS and trigger GC; `disposed` is already true before disposal starts, and
    //   each step pops `head` before calling user code, keeping all remaining records,
    //   `pendingError`, and `promiseCapability` rooted via stack/step closure.
    // - Thread / context: bound to a single ScriptContext/ThreadContext; cross-realm values
    //   are marshaled with CrossSite::MarshalVar at call/return boundaries.
    // - Release / teardown: GC-collected once frame/stack/promise chain completes; no
    //   finalizers, native locks, or manual free paths exist.
    class DisposeCapability : public DynamicObject
    {
    private:
        DEFINE_VTABLE_CTOR(DisposeCapability, DynamicObject);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(DisposeCapability);

        Field(DisposableResourceRecord*) head;
        Field(Var) pendingError;
        Field(Var) currentAwaitValue;
        Field(bool) hasPendingError;
        Field(bool) needsAwait;
        Field(bool) hasAwaited;

        void PushRecord(
            Var resource,
            Var disposeMethod,
            DisposeHint hint,
            bool needsSyncToAsyncWrap,
            bool passResourceAsArgument,
            ScriptContext* scriptContext);

        static Var InvokeDisposeMethod(const DisposableResourceRecord* record, ScriptContext* scriptContext);

    public:
        explicit DisposeCapability(DynamicType* type);

        static DisposeCapability* New(ScriptContext* scriptContext);
        static DisposeCapability* FromVarOrNew(Var curStack, ScriptContext* scriptContext);

        void AddResource(Var value, DisposeHint hint, ScriptContext* scriptContext);
        void AddAdoptResource(Var value, Var onDispose, DisposeHint hint, ScriptContext* scriptContext);
        void AddDeferResource(Var onDispose, DisposeHint hint, ScriptContext* scriptContext);

        void RecordError(Var caughtError, ScriptContext* scriptContext);
        void DisposeSync(ScriptContext* scriptContext);

        // Returns:
        //   0 -> stack empty (disposal loop complete)
        //   1 -> sync step completed without await (or sync throw recorded)
        //   2 -> async value produced in currentAwaitValue (ready for TakeAwaitValue -> Await)
        int32 StepAsyncOrSync(ScriptContext* scriptContext);
        Var TakeAwaitValue(ScriptContext* scriptContext);
        void Finalize(ScriptContext* scriptContext);

        bool HasPendingError() const { return hasPendingError; }
        Var TakePendingError(ScriptContext* scriptContext);
        DisposeCapability* MoveToNew(ScriptContext* scriptContext);
    };

    template <> bool VarIsImpl<DisposeCapability>(RecyclableObject* obj);

    class JavascriptAsyncDisposableStackStepFunction : public RuntimeFunction
    {
    protected:
        DEFINE_VTABLE_CTOR(JavascriptAsyncDisposableStackStepFunction, RuntimeFunction);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptAsyncDisposableStackStepFunction);

    public:
        JavascriptAsyncDisposableStackStepFunction(
            DynamicType* type,
            FunctionInfo* functionInfo,
            DisposeCapability* capability,
            JavascriptPromiseCapability* promiseCapability,
            bool isReject)
            : RuntimeFunction(type, functionInfo)
            , capability(capability)
            , promiseCapability(promiseCapability)
            , isReject(isReject)
        {
        }

        DisposeCapability* GetCapability() const { return capability; }
        JavascriptPromiseCapability* GetPromiseCapability() const { return promiseCapability; }
        bool IsReject() const { return isReject; }

        static Var EntryStep(RecyclableObject* function, CallInfo callInfo, ...);

    private:
        Field(DisposeCapability*) capability;
        Field(JavascriptPromiseCapability*) promiseCapability;
        bool isReject;
    };

    template <> bool VarIsImpl<JavascriptAsyncDisposableStackStepFunction>(RecyclableObject* obj);

    class JavascriptDisposableStack : public DynamicObject
    {
    private:
        DEFINE_VTABLE_CTOR(JavascriptDisposableStack, DynamicObject);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptDisposableStack);

        Field(DisposeCapability*) capability;
        Field(bool) disposed;

    public:
        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Use;
            static FunctionInfo Adopt;
            static FunctionInfo Defer;
            static FunctionInfo Dispose;
            static FunctionInfo Move;
            static FunctionInfo GetterDisposed;
        };

        explicit JavascriptDisposableStack(DynamicType* type, DisposeCapability* capability);

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryUse(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAdopt(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDefer(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDispose(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryMove(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterDisposed(RecyclableObject* function, CallInfo callInfo, ...);

        bool IsDisposed() const { return disposed; }
        DisposeCapability* GetCapability() const { return capability; }

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;
    };

    template <> bool VarIsImpl<JavascriptDisposableStack>(RecyclableObject* obj);

    class JavascriptAsyncDisposableStack : public DynamicObject
    {
    private:
        DEFINE_VTABLE_CTOR(JavascriptAsyncDisposableStack, DynamicObject);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptAsyncDisposableStack);

        Field(DisposeCapability*) capability;
        Field(bool) disposed;

    public:
        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Use;
            static FunctionInfo Adopt;
            static FunctionInfo Defer;
            static FunctionInfo DisposeAsync;
            static FunctionInfo Move;
            static FunctionInfo GetterDisposed;
        };

        explicit JavascriptAsyncDisposableStack(DynamicType* type, DisposeCapability* capability);

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryUse(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAdopt(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDefer(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDisposeAsync(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryMove(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterDisposed(RecyclableObject* function, CallInfo callInfo, ...);

        bool IsDisposed() const { return disposed; }
        DisposeCapability* GetCapability() const { return capability; }

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;
    };

    template <> bool VarIsImpl<JavascriptAsyncDisposableStack>(RecyclableObject* obj);
}
