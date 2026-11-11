//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptWeakRef : public DynamicObject
    {
    private:
        // The WeakRef owns this recycler weak-reference handle; it must not
        // own the target. Recycler::SweepWeakReference clears the handle's
        // strongRef when the target is no longer marked, and the handle itself
        // is reclaimed once this WeakRef is no longer marked.
        Field(RecyclerWeakReference<RecyclableObject>*) targetWeakRef;

        friend class VirtualTableInfo<JavascriptWeakRef>;
        JavascriptWeakRef(VirtualTableInfoCtorEnum v) : DynamicObject(v), targetWeakRef(nullptr) {}
        DEFINE_VALIDATE_VTABLE_REGISTERED(JavascriptWeakRef);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptWeakRef);

    public:
        JavascriptWeakRef(DynamicType* type, RecyclerWeakReference<RecyclableObject>* targetWeakRef)
            : DynamicObject(type), targetWeakRef(targetWeakRef)
        {
        }

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDeref(RecyclableObject* function, CallInfo callInfo, ...);

        Var Deref(ScriptContext* requestContext) const;

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Deref;
        };
    };

    template <> inline bool VarIsImpl<JavascriptWeakRef>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_WeakRef;
    }
}
