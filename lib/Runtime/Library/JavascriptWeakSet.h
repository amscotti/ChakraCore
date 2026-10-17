//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptWeakSet : public DynamicObject
    {
    private:
#if ENABLE_WEAK_REFERENCE_REGIONS
        typedef JsUtil::WeakReferenceRegionKeyDictionary<RecyclableObject*, bool, RecyclerPointerComparer> KeySet;
        typedef const RecyclerWeakReferenceRegionItem<RecyclableObject*>& WeakType;
#else
        typedef JsUtil::WeaklyReferencedKeyDictionary<RecyclableObject, bool, RecyclerPointerComparer<const RecyclableObject*>> KeySet;
        typedef const RecyclerWeakReference<RecyclableObject>* WeakType;
#endif

        // Symbols are primitives; they cannot be weakly held and stay strongly
        // reachable here until delete/clear/finalize. Object keys keep using the
        // weak keySet.
        typedef JsUtil::BaseDictionary<JavascriptSymbol*, bool, RecyclerNonLeafAllocator, PowerOf2SizePolicy, RecyclerPointerComparer> SymbolMap;

        Field(KeySet) keySet;
        Field(SymbolMap*) symbolMap;
        void EnsureSymbolMap();

        friend class VirtualTableInfo<JavascriptWeakSet>;
        JavascriptWeakSet(VirtualTableInfoCtorEnum v) : DynamicObject(v), keySet(v), symbolMap(nullptr) {}
        DEFINE_VALIDATE_VTABLE_REGISTERED(JavascriptWeakSet);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptWeakSet);

    public:
        JavascriptWeakSet(DynamicType* type);

        void Add(RecyclableObject* key);
        void Add(JavascriptSymbol* key);
        bool Delete(RecyclableObject* key);
        bool Delete(JavascriptSymbol* key);
        bool Has(RecyclableObject* key);
        bool Has(JavascriptSymbol* key);

        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo Add;
            static FunctionInfo Delete;
            static FunctionInfo Has;

        };
        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAdd(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDelete(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryHas(RecyclableObject* function, CallInfo callInfo, ...);

    public:
        // For diagnostics and heap enum provide size and allow enumeration of key value pairs
        int Size() { keySet.Clean(); return keySet.Count() + (symbolMap != nullptr ? symbolMap->Count() : 0); }
        template <typename Fn>
        void Map(Fn fn)
        {
            keySet.Map([&](RecyclableObject* key, bool, WeakType)
            {
                fn(key);
            });

            if (symbolMap != nullptr)
            {
                symbolMap->Map([&](JavascriptSymbol* key, bool)
                {
                    fn(key);
                });
            }
        }

#if ENABLE_TTD
    public:
        virtual void MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor) override;

        virtual TTD::NSSnapObjects::SnapObjectType GetSnapTag_TTD() const override;
        virtual void ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc) override;
#endif
    };

    template <> inline bool VarIsImpl<JavascriptWeakSet>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_WeakSet;
    }
}
