//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptFinalizationRegistry;
    class JavascriptFinalizationRegistryCleanupTaskFunction;

    // One [[Cells]] record. The target and the optional unregister token are
    // weak recycler handles; heldValue is a strong Field(Var) so it survives
    // until CleanupDeadCells removes the cell or unregister drops it.
    class FinalizationRegistryCell
    {
    public:
        Field(RecyclerWeakReference<RecyclableObject>*) targetWeakRef;
        Field(Var) heldValue;
        Field(RecyclerWeakReference<RecyclableObject>*) unregisterTokenWeakRef;

        FinalizationRegistryCell(
            RecyclerWeakReference<RecyclableObject>* targetWeakRef,
            Var heldValue,
            RecyclerWeakReference<RecyclableObject>* unregisterTokenWeakRef)
            : targetWeakRef(targetWeakRef),
              heldValue(heldValue),
              unregisterTokenWeakRef(unregisterTokenWeakRef)
        {
        }
    };

    class JavascriptFinalizationRegistry : public DynamicObject
    {
    private:
        typedef JsUtil::List<FinalizationRegistryCell*, Recycler> CellList;

        Field(Var) cleanupCallback;
        Field(CellList*) cells;
        Field(BOOL) cleanupJobScheduled;
        Field(BOOL) cleanupJobActive;

        friend class VirtualTableInfo<JavascriptFinalizationRegistry>;
        friend class JavascriptFinalizationRegistryCleanupTaskFunction;
        JavascriptFinalizationRegistry(VirtualTableInfoCtorEnum v)
            : DynamicObject(v), cleanupCallback(nullptr), cells(nullptr),
              cleanupJobScheduled(FALSE), cleanupJobActive(FALSE) {}
        DEFINE_VALIDATE_VTABLE_REGISTERED(JavascriptFinalizationRegistry);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptFinalizationRegistry);

    public:
        JavascriptFinalizationRegistry(DynamicType* type, Var cleanupCallback)
            : DynamicObject(type),
              cleanupCallback(cleanupCallback),
              cells(nullptr),
              cleanupJobScheduled(FALSE),
              cleanupJobActive(FALSE)
        {
        }

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryRegister(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryUnregister(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryCleanupJob(RecyclableObject* function, CallInfo callInfo, ...);

        void Register(RecyclableObject* target, Var heldValue, RecyclableObject* unregisterToken);
        bool Unregister(RecyclableObject* unregisterToken);
        bool HasClearedTargets() const;
        void ScheduleCleanupJob();
        void CleanupDeadCells();

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;

        Var GetCleanupCallback() const { return cleanupCallback; }

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Register;
            static FunctionInfo Unregister;
        };
    };

    // Host job task that owns a strong reference to its registry until the
    // host drains it. Created only when the host installed a native promise
    // continuation/job callback.
    class JavascriptFinalizationRegistryCleanupTaskFunction : public RuntimeFunction
    {
    protected:
        DEFINE_VTABLE_CTOR(JavascriptFinalizationRegistryCleanupTaskFunction, RuntimeFunction);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptFinalizationRegistryCleanupTaskFunction);

    public:
        JavascriptFinalizationRegistryCleanupTaskFunction(
            DynamicType* type,
            FunctionInfo* functionInfo,
            JavascriptFinalizationRegistry* registry)
            : RuntimeFunction(type, functionInfo), registry(registry)
        {
        }

        JavascriptFinalizationRegistry* GetRegistry() const { return registry; }

    private:
        Field(JavascriptFinalizationRegistry*) registry;
    };

    template <> inline bool VarIsImpl<JavascriptFinalizationRegistry>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_FinalizationRegistry;
    }

    template <> bool VarIsImpl<JavascriptFinalizationRegistryCleanupTaskFunction>(RecyclableObject* obj);
}
