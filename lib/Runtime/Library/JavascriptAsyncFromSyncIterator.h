//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptAsyncFromSyncIterator : public DynamicObject
    {
    public:
        JavascriptAsyncFromSyncIterator(DynamicType* type, RecyclableObject* syncIterator) :
            DynamicObject(type), syncIterator(syncIterator), syncNextFunction(nullptr) { }

        RecyclableObject* GetSyncIterator() { return this->syncIterator; }
        RecyclableObject* EnsureSyncNextFunction(ScriptContext* scriptContext);

        static Var AsyncFromSyncIteratorContinuation(
            RecyclableObject* result,
            RecyclableObject* syncIteratorRecord,
            bool closeOnRejection,
            ScriptContext* scriptContext);
        static Var EntryAsyncFromSyncIteratorNext(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsyncFromSyncIteratorThrow(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsyncFromSyncIteratorReturn(RecyclableObject* function, CallInfo callInfo, ...);

        static Var EntryAsyncFromSyncIteratorValueUnwrapFalseFunction(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsyncFromSyncIteratorValueUnwrapTrueFunction(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsyncFromSyncCloseOnRejectFunction(RecyclableObject* function, CallInfo callInfo, ...);

        class EntryInfo
        {
        public:
            static FunctionInfo Next;
            static FunctionInfo Return;
            static FunctionInfo Throw;
        };

    private:
        static Var RejectPromiseWithCaughtException(const JavascriptException& err, ScriptContext* scriptContext);
        static void IteratorCloseNormalCompletion(RecyclableObject* syncIterator, ScriptContext* scriptContext);
        static RecyclableObject* CreateCloseOnRejectFunction(RecyclableObject* syncIterator, ScriptContext* scriptContext);

        Field(RecyclableObject*) syncIterator;
        Field(RecyclableObject*) syncNextFunction;
    protected:
        DEFINE_VTABLE_CTOR(JavascriptAsyncFromSyncIterator, DynamicObject);
    };

    template <> bool VarIsImpl<JavascriptAsyncFromSyncIterator>(RecyclableObject* obj);

    /*
     * Ownership record (doc/memory-management.md section 3) for AsyncFromSyncCloseOnRejectFunction:
     * - Resource / creation call: RuntimeFunction subclass allocated via
     *   RecyclerNewEnumClass(..., JavascriptLibrary::EnumFunctionClass, ...) in
     *   JavascriptAsyncFromSyncIterator::CreateCloseOnRejectFunction when
     *   AsyncFromSyncIteratorContinuation runs with !done && closeOnRejection.
     * - Domain / allocator: ScriptContext Recycler GC heap (lifetime bound to ScriptContext).
     * - Physical descriptor: Single Recycler-allocated EnumFunctionClass object whose
     *   DynamicType comes from JavascriptLibrary::CreateFunctionWithLengthAndNameType;
     *   no native heap memory, finalizer, or pool entry is held.
     * - Logical fields: Field(RecyclableObject*) syncIterator with write-barrier
     *   protection plus configurable `length` (1) and `name` ("") JS properties.
     * - Owner / publication / aliases: Created in AsyncFromSyncIteratorContinuation and
     *   published solely as the rejectReaction handler passed to PerformPromiseThen on
     *   valueWrapper; no off-stack native pointers or explicit roots are kept.
     * - Borrow invalidation: Recycler tracing keeps syncIterator alive across GC until
     *   the reaction settles or becomes unreachable; ScriptContext disposal destroys all.
     * - Reentrancy / user code: EntryAsyncFromSyncCloseOnRejectFunction calls
     *   JavascriptOperators::IteratorClose (GetProperty("return") and ExecuteImplicitCall)
     *   after reading syncIterator, then throws the marshaled original rejection reason.
     * - Thread/context: Single bound ScriptContext execution thread; supports cross-realm
     *   calls via DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT and CrossSite::MarshalVar.
     * - Failure / cleanup: Reclaimed by Recycler sweep; inner exceptions from `return`
     *   are cleared via err.GetAndClear() so the original rejection reason always wins.
     * - Accounting / bound: At most one callback allocated per !done && closeOnRejection
     *   continuation, bounded by Recycler heap policy (no persistent cache/queue).
     */
    class AsyncFromSyncCloseOnRejectFunction : public RuntimeFunction
    {
    protected:
        DEFINE_VTABLE_CTOR(AsyncFromSyncCloseOnRejectFunction, RuntimeFunction);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(AsyncFromSyncCloseOnRejectFunction);

    public:
        AsyncFromSyncCloseOnRejectFunction(
            DynamicType* type,
            FunctionInfo* functionInfo,
            RecyclableObject* syncIterator) :
            RuntimeFunction(type, functionInfo),
            syncIterator(syncIterator)
        {
        }

        RecyclableObject* GetSyncIterator() const { return this->syncIterator; }

    private:
        Field(RecyclableObject*) syncIterator;
    };

    template <> bool VarIsImpl<AsyncFromSyncCloseOnRejectFunction>(RecyclableObject* obj);

} // namespace Js
