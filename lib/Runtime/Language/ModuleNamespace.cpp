//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "RuntimeLanguagePch.h"
#include "Types/PropertyIndexRanges.h"
#include "Types/SimpleDictionaryPropertyDescriptor.h"
#include "Types/SimpleDictionaryTypeHandler.h"
#include "ModuleNamespace.h"
#include "ModuleNamespaceEnumerator.h"

namespace Js
{
    ModuleNamespace::ModuleNamespace(ModuleRecordBase* moduleRecord, DynamicType* type) :
        moduleRecord(moduleRecord), DynamicObject(type), unambiguousNonLocalExports(nullptr),
        sortedExportedNames(nullptr), nsSlots(nullptr)
    {

    }

    ModuleNamespace* ModuleNamespace::GetModuleNamespace(ModuleRecordBase* requestModule)
    {
        Assert(requestModule->IsSourceTextModuleRecord());
        SourceTextModuleRecord* moduleRecord = SourceTextModuleRecord::FromHost(requestModule);
        ModuleNamespace* nsObject = moduleRecord->GetNamespace();
        if (nsObject != nullptr)
        {
            return nsObject;
        }
        ScriptContext* scriptContext = moduleRecord->GetRealm()->GetScriptContext();
        Recycler* recycler = scriptContext->GetRecycler();

        nsObject = RecyclerNew(recycler, ModuleNamespace, moduleRecord, scriptContext->GetLibrary()->GetModuleNamespaceType());
        nsObject->Initialize();

        moduleRecord->SetNamespace(nsObject);
        return nsObject;
    }

    void ModuleNamespace::Initialize()
    {
        ScriptContext* scriptContext = moduleRecord->GetRealm()->GetScriptContext();
        Recycler* recycler = scriptContext->GetRecycler();
        SourceTextModuleRecord* sourceTextModuleRecord = static_cast<SourceTextModuleRecord*>(
            static_cast<ModuleRecordBase*>(moduleRecord));
        JavascriptLibrary* library = GetLibrary();

        if (scriptContext->GetConfig()->IsES6ToStringTagEnabled())
        {
            DynamicObject::SetPropertyWithAttributes(PropertyIds::_symbolToStringTag, library->GetModuleTypeDisplayString(),
                PropertyNone, nullptr);
        }

        ModuleImportOrExportEntryList* localExportList = sourceTextModuleRecord->GetLocalExportEntryList();
        // We don't have a type handler that can handle ModuleNamespace object. We have properties that could be aliased
        // like {export foo as foo1, foo2, foo3}, and external properties as reexport from current module. The problem with aliasing
        // is that multiple propertyId can be associated with one slotIndex. We need to build from PropertyMap directly here.
        // there is one instance of ModuleNamespace per module file; we can always use the BigPropertyIndex for security.
        propertyMap = RecyclerNew(recycler, SimplePropertyDescriptorMap, recycler, sourceTextModuleRecord->GetLocalExportCount());
        if (localExportList != nullptr)
        {
            localExportList->Map([=](ModuleImportOrExportEntry exportEntry) {
                PropertyId exportNameId = exportEntry.exportName->GetPropertyId();
                PropertyId localNameId = exportEntry.localName->GetPropertyId();
                const Js::PropertyRecord* exportNameRecord = scriptContext->GetThreadContext()->GetPropertyName(exportNameId);
                ModuleNameRecord* importRecord = nullptr;
                AssertMsg(exportNameId != Js::Constants::NoProperty, "should have been initialized already");
                // ignore local exports that are actually indirect exports.
                if (sourceTextModuleRecord->GetImportEntryList() == nullptr ||
                    !sourceTextModuleRecord->ResolveImport(localNameId, &importRecord))
                {
                    BigPropertyIndex index = sourceTextModuleRecord->GetLocalExportSlotIndexByExportName(exportNameId);
                    Assert((uint)index < sourceTextModuleRecord->GetLocalExportCount());
                    SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor = { index, PropertyModuleNamespaceDefault };
                    propertyMap->Add(exportNameRecord, propertyDescriptor);
                }
            });
        }
        // update the local slot to use the storage for local exports.
        SetNSSlotsForModuleNS(sourceTextModuleRecord->GetLocalExportSlots());

        // For items that are not in the local export list, we need to resolve them to get it
        ExportedNames* exportedNames = sourceTextModuleRecord->GetExportedNames(nullptr);
        ModuleNameRecord* moduleNameRecord = nullptr;
        sortedExportedNames = ListForListIterator::New(scriptContext->GetRecycler());
#if DBG
        uint unresolvableExportsCount = 0;
        uint localExportCount = 0;
#endif
        if (exportedNames != nullptr)
        {
            exportedNames->Map([&](PropertyId propertyId) {
                JavascriptString* propertyString = scriptContext->GetPropertyString(propertyId);
                sortedExportedNames->Add(propertyString);
                if (!moduleRecord->ResolveExport(propertyId, nullptr, &moduleNameRecord))
                {
                    // ignore ambiguous resolution.
#if DBG
                    unresolvableExportsCount++;
#endif
                    return;
                }
                // non-ambiguous resolution.
                if (moduleNameRecord == nullptr)
                {
                    JavascriptError::ThrowSyntaxError(scriptContext, JSERR_ResolveExportFailed, scriptContext->GetPropertyName(propertyId)->GetBuffer());
                }
                if (moduleNameRecord->module == moduleRecord)
                {
                    // skip local exports as they are covered in the localExportSlots.
                    // have to check the property map to avoid filtering out aliased local re-exports
                    // which are not covered in localExportSlots
                    if (propertyMap->ContainsKey(scriptContext->GetThreadContext()->GetPropertyName(propertyId)))
                    {
#if DBG
                        localExportCount++;
#endif
                        return;
                    }
                }
                this->AddUnambiguousNonLocalExport(propertyId, moduleNameRecord);
            });
        }

        sortedExportedNames->Sort([](void* context, const void* left, const void* right) ->int {
            JavascriptString** leftString = (JavascriptString**)(left);
            JavascriptString** rightString = (JavascriptString**)(right);
            return JavascriptString::strcmp(*leftString, *rightString);
        }, nullptr);

#if DBG
        uint totalExportCount = exportedNames != nullptr ? exportedNames->Count() : 0;
        uint unambiguousNonLocalCount = (this->GetUnambiguousNonLocalExports() != nullptr) ? this->GetUnambiguousNonLocalExports()->Count() : 0;
        Assert(totalExportCount == localExportCount + unambiguousNonLocalCount + unresolvableExportsCount);
#endif
        BOOL result = this->PreventExtensions();
        Assert(result);
    }

    void ModuleNamespace::AddUnambiguousNonLocalExport(PropertyId propertyId, ModuleNameRecord* nonLocalExportNameRecord)
    {
        Recycler* recycler = GetScriptContext()->GetRecycler();
        if (unambiguousNonLocalExports == nullptr)
        {
            unambiguousNonLocalExports = RecyclerNew(recycler, UnambiguousExportMap, recycler, 4);
        }
        // keep a local copy of the module/
        unambiguousNonLocalExports->AddNew(propertyId, *nonLocalExportNameRecord);
    }

    PropertyQueryFlags ModuleNamespace::HasPropertyQuery(PropertyId propertyId, _Inout_opt_ PropertyValueInfo* info)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        const Js::PropertyRecord* propertyRecord = GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::HasPropertyQuery(propertyId, info);
        }
        if (propertyMap != nullptr && propertyMap->TryGetValue(propertyRecord, &propertyDescriptor))
        {
            return PropertyQueryFlags::Property_Found;
        }
        if (unambiguousNonLocalExports != nullptr)
        {
            return JavascriptConversion::BooleanToPropertyQueryFlags(unambiguousNonLocalExports->ContainsKey(propertyId));
        }
        return PropertyQueryFlags::Property_NotFound;
    }

    BOOL ModuleNamespace::HasOwnProperty(PropertyId propertyId)
    {
        const Js::PropertyRecord* propertyRecord = GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::HasOwnProperty(propertyId);
        }
        if (!HasProperty(propertyId))
        {
            return FALSE;
        }
        Var value = nullptr;
        return GetProperty(this, propertyId, &value, nullptr, GetScriptContext());
    }

    static BOOL RejectDefineProperty(bool throwOnError, ScriptContext* scriptContext, int32 errorCode, const PropertyRecord* propertyRecord)
    {
        Assert(scriptContext);
        Assert(propertyRecord);
        if (throwOnError)
        {
            JavascriptError::ThrowTypeError(scriptContext, errorCode, propertyRecord->GetBuffer());
        }
        return FALSE;
    }

    BOOL ModuleNamespace::DefineOwnPropertyDescriptor(ModuleNamespace* obj, PropertyId propId, const PropertyDescriptor& descriptor, bool throwOnError, ScriptContext* scriptContext)
    {
        Assert(obj);
        Assert(scriptContext);

        // ES2025 10.4.6.6 [[DefineOwnProperty]] ( P, Desc ):
        // 1. If P is a Symbol, return ! OrdinaryDefineOwnProperty(O, P, Desc).
        const PropertyRecord* propertyRecord = scriptContext->GetThreadContext()->GetPropertyName(propId);
        if (propertyRecord->IsSymbol())
        {
            PropertyDescriptor currentDescriptor;
            BOOL isCurrentDescriptorDefined = JavascriptOperators::GetOwnPropertyDescriptor(obj, propId, scriptContext, &currentDescriptor);
            bool isExtensible = !!obj->IsExtensible();
            return JavascriptOperators::ValidateAndApplyPropertyDescriptor<true>(
                obj, propId, descriptor, isCurrentDescriptorDefined ? &currentDescriptor : nullptr, isExtensible, throwOnError, scriptContext);
        }

        // 2. Let current be ? O.[[GetOwnProperty]](P) (may throw ReferenceError in TDZ).
        // 3. If current is undefined, return false.
        PropertyDescriptor currentDescriptor;
        if (!JavascriptOperators::GetOwnPropertyDescriptor(obj, propId, scriptContext, &currentDescriptor))
        {
            return RejectDefineProperty(throwOnError, scriptContext, JSERR_DefineProperty_NotExtensible, propertyRecord);
        }

        // 4. If Desc.[[Configurable]] is true, return false.
        // 5. If Desc.[[Enumerable]] is false, return false.
        // 6. If IsAccessorDescriptor(Desc) is true, return false.
        // 7. If Desc.[[Writable]] is false, return false.
        if ((descriptor.ConfigurableSpecified() && descriptor.IsConfigurable()) ||
            (descriptor.EnumerableSpecified() && !descriptor.IsEnumerable()) ||
            descriptor.IsAccessorDescriptor() ||
            (descriptor.WritableSpecified() && !descriptor.IsWritable()))
        {
            return RejectDefineProperty(throwOnError, scriptContext, JSERR_DefineProperty_NotConfigurable, propertyRecord);
        }

        // 8. If Desc has a [[Value]] field, return SameValue(Desc.[[Value]], current.[[Value]]).
        if (descriptor.ValueSpecified() &&
            !JavascriptConversion::SameValue(descriptor.GetValue(), currentDescriptor.GetValue()))
        {
            return RejectDefineProperty(throwOnError, scriptContext, JSERR_DefineProperty_NotWritable, propertyRecord);
        }

        // 9. Return true.
        return TRUE;
    }

    BOOL ModuleNamespace::Seal()
    {
        // SetIntegrityLevel(ns, "sealed"): check [[GetOwnProperty]] (TDZ) across
        // exports; all exports are already configurable:false, and non-extensible.
        ScriptContext* scriptContext = GetScriptContext();
        if (sortedExportedNames != nullptr)
        {
            auto count = sortedExportedNames->Count();
            for (decltype(count) i = 0; i < count; i++)
            {
                Var value = nullptr;
                GetProperty(this, VarTo<JavascriptString>(sortedExportedNames->Item(i)), &value, nullptr, scriptContext);
            }
        }
        return TRUE;
    }

    BOOL ModuleNamespace::Freeze()
    {
        // SetIntegrityLevel(ns, "frozen"): succeeds only when no resolved exports
        // exist (Symbol.toStringTag is already non-writable, non-configurable).
        // Any initialized export has writable:true, so freezing rejects; TDZ throws first.
        ScriptContext* scriptContext = GetScriptContext();
        if (sortedExportedNames != nullptr)
        {
            auto count = sortedExportedNames->Count();
            for (decltype(count) i = 0; i < count; i++)
            {
                Var value = nullptr;
                if (GetProperty(this, VarTo<JavascriptString>(sortedExportedNames->Item(i)), &value, nullptr, scriptContext))
                {
                    return FALSE;
                }
            }
        }
        return TRUE;
    }

    BOOL ModuleNamespace::IsSealed()
    {
        return Seal();
    }

    BOOL ModuleNamespace::IsFrozen()
    {
        return Freeze();
    }

    BOOL ModuleNamespace::IsConfigurable(PropertyId propertyId)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        const Js::PropertyRecord* propertyRecord = GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::IsConfigurable(propertyId);
        }

        if (propertyMap != nullptr && propertyMap->TryGetValue(propertyRecord, &propertyDescriptor))
        {
            return !!(propertyDescriptor.Attributes & PropertyConfigurable);
        }

        if (unambiguousNonLocalExports != nullptr)
        {
            ModuleNameRecord moduleNameRecord;
            if (unambiguousNonLocalExports->TryGetValue(propertyId, &moduleNameRecord))
            {
                return !!(PropertyModuleNamespaceDefault & PropertyConfigurable);
            }
        }

        return DynamicObject::IsConfigurable(propertyId);
    }

    BOOL ModuleNamespace::IsEnumerable(PropertyId propertyId)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        const Js::PropertyRecord* propertyRecord = GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::IsEnumerable(propertyId);
        }

        if (propertyMap != nullptr && propertyMap->TryGetValue(propertyRecord, &propertyDescriptor))
        {
            return !!(propertyDescriptor.Attributes & PropertyEnumerable);
        }

        if (unambiguousNonLocalExports != nullptr)
        {
            ModuleNameRecord moduleNameRecord;
            if (unambiguousNonLocalExports->TryGetValue(propertyId, &moduleNameRecord))
            {
                return !!(PropertyModuleNamespaceDefault & PropertyEnumerable);
            }
        }

        return DynamicObject::IsEnumerable(propertyId);
    }

    BOOL ModuleNamespace::IsWritable(PropertyId propertyId)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        const Js::PropertyRecord* propertyRecord = GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::IsWritable(propertyId);
        }

        if (propertyMap != nullptr && propertyMap->TryGetValue(propertyRecord, &propertyDescriptor))
        {
            return !!(propertyDescriptor.Attributes & PropertyWritable);
        }

        if (unambiguousNonLocalExports != nullptr)
        {
            ModuleNameRecord moduleNameRecord;
            if (unambiguousNonLocalExports->TryGetValue(propertyId, &moduleNameRecord))
            {
                return !!(PropertyModuleNamespaceDefault & PropertyWritable);
            }
        }

        return DynamicObject::IsWritable(propertyId);
    }

    PropertyQueryFlags ModuleNamespace::GetPropertyQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        const Js::PropertyRecord* propertyRecord = requestContext->GetThreadContext()->GetPropertyName(propertyId);
        if (propertyRecord->IsSymbol())
        {
            return this->DynamicObject::GetPropertyQuery(originalInstance, propertyId, value, info, requestContext);
        }
        if (propertyMap != nullptr && propertyMap->TryGetValue(propertyRecord, &propertyDescriptor))
        {
            Assert((uint)propertyDescriptor.propertyIndex < ((SourceTextModuleRecord*)static_cast<ModuleRecordBase*>(moduleRecord))->GetLocalExportCount());
            PropertyValueInfo::SetNoCache(info, this); // Disable inlinecache for localexport slot for now.
            //if ((PropertyIndex)propertyDescriptor.propertyIndex == propertyDescriptor.propertyIndex)
            //{
            //    PropertyValueInfo::Set(info, this, (PropertyIndex)propertyDescriptor.propertyIndex, propertyDescriptor.Attributes);
            //}
            *value = this->GetNSSlot(propertyDescriptor.propertyIndex);
            return PropertyQueryFlags::Property_Found;
        }
        if (unambiguousNonLocalExports != nullptr)
        {
            ModuleNameRecord moduleNameRecord;
            // TODO: maybe we can cache the slot address & offset, instead of looking up everytime? We do need to look up the reference everytime.
            if (unambiguousNonLocalExports->TryGetValue(propertyId, &moduleNameRecord))
            {
                // special case for export * as ns
                if (moduleNameRecord.bindingName == ModuleRecordBase::NamespaceBindingPropertyId)
                {
                    ModuleNamespace* targetNs = ModuleNamespace::GetModuleNamespace(moduleNameRecord.module);
                    *value = static_cast<Var>(targetNs);
                    if (targetNs->GetScriptContext() != requestContext)
                    {
                        *value = CrossSite::MarshalVar(requestContext, *value, targetNs->GetScriptContext());
                    }
                    return PropertyQueryFlags::Property_Found;
                }
                Assert(moduleNameRecord.module->IsSourceTextModuleRecord());
                SourceTextModuleRecord* targetModule = SourceTextModuleRecord::FromHost(moduleNameRecord.module);
                uint slotIndex = targetModule->GetLocalExportSlotIndexByLocalName(moduleNameRecord.bindingName);
                ModuleNamespace* targetNs = ModuleNamespace::GetModuleNamespace(targetModule);
                *value = targetNs->GetNSSlot(slotIndex);
                if (targetNs->GetScriptContext() != requestContext)
                {
                    *value = CrossSite::MarshalVar(requestContext, *value, targetNs->GetScriptContext());
                }
                return PropertyQueryFlags::Property_Found;
            }
        }
        return PropertyQueryFlags::Property_NotFound;
    }

    PropertyQueryFlags ModuleNamespace::GetPropertyQuery(Var originalInstance, JavascriptString* propertyNameString, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        const PropertyRecord* propertyRecord = nullptr;
        GetScriptContext()->GetOrAddPropertyRecord(propertyNameString, &propertyRecord);
        return JavascriptConversion::BooleanToPropertyQueryFlags(GetProperty(originalInstance, propertyRecord->GetPropertyId(), value, info, requestContext));
    }

    BOOL ModuleNamespace::GetInternalProperty(Var instance, PropertyId internalPropertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        return FALSE;
    }

    PropertyQueryFlags ModuleNamespace::GetPropertyReferenceQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        return JavascriptConversion::BooleanToPropertyQueryFlags(GetProperty(originalInstance, propertyId, value, info, requestContext));
    }

    BOOL ModuleNamespace::GetEnumerator(JavascriptStaticEnumerator * enumerator, EnumeratorFlags flags, ScriptContext* requestContext, EnumeratorCache * enumeratorCache)
    {
        if (!(flags & EnumeratorFlags::EnumNonEnumerable) && sortedExportedNames != nullptr)
        {
            auto count = sortedExportedNames->Count();
            for (decltype(count) i = 0; i < count; i++)
            {
                Var value = nullptr;
                GetProperty(this, VarTo<JavascriptString>(sortedExportedNames->Item(i)), &value, nullptr, requestContext);
            }
        }
        ModuleNamespaceEnumerator* moduleEnumerator = ModuleNamespaceEnumerator::New(this, flags, requestContext, enumeratorCache);
        if (moduleEnumerator == nullptr)
        {
            return FALSE;
        }
        return enumerator->Initialize(moduleEnumerator, nullptr, nullptr, flags, requestContext, nullptr);
    }

    BOOL ModuleNamespace::SetProperty(PropertyId propertyId, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        JavascriptError::ThrowCantAssignIfStrictMode(flags, GetScriptContext());
        return FALSE;
    }

    BOOL ModuleNamespace::SetProperty(JavascriptString* propertyNameString, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        JavascriptError::ThrowCantAssignIfStrictMode(flags, GetScriptContext());
        return FALSE;
    }

    bool ModuleNamespace::TryFindPropertyRecordForIndex(uint32 index, const PropertyRecord** propertyRecord) const
    {
        char16 buffer[20];
        ::_ui64tow_s(static_cast<uint64>(index), buffer, _countof(buffer), 10);
        GetScriptContext()->FindPropertyRecord(buffer, JavascriptString::GetBufferLength(buffer), propertyRecord);
        return *propertyRecord != nullptr;
    }

    PropertyQueryFlags ModuleNamespace::HasItemQuery(uint32 index)
    {
        const PropertyRecord* propertyRecord = nullptr;
        if (TryFindPropertyRecordForIndex(index, &propertyRecord))
        {
            return HasPropertyQuery(propertyRecord->GetPropertyId(), nullptr);
        }
        return PropertyQueryFlags::Property_NotFound;
    }

    BOOL ModuleNamespace::HasOwnItem(uint32 index)
    {
        const PropertyRecord* propertyRecord = nullptr;
        if (TryFindPropertyRecordForIndex(index, &propertyRecord))
        {
            return HasOwnProperty(propertyRecord->GetPropertyId());
        }
        return FALSE;
    }

    PropertyQueryFlags ModuleNamespace::GetItemQuery(Var originalInstance, uint32 index, Var* value, ScriptContext* requestContext)
    {
        const PropertyRecord* propertyRecord = nullptr;
        if (TryFindPropertyRecordForIndex(index, &propertyRecord))
        {
            return GetPropertyQuery(originalInstance, propertyRecord->GetPropertyId(), value, nullptr, requestContext);
        }
        return PropertyQueryFlags::Property_NotFound;
    }

    PropertyQueryFlags ModuleNamespace::GetItemReferenceQuery(Var originalInstance, uint32 index, Var* value, ScriptContext* requestContext)
    {
        return GetItemQuery(originalInstance, index, value, requestContext);
    }

    BOOL ModuleNamespace::SetItem(uint32 index, Var value, PropertyOperationFlags flags)
    {
        JavascriptError::ThrowCantAssignIfStrictMode(flags, GetScriptContext());
        return FALSE;
    }

    BOOL ModuleNamespace::DeleteItem(uint32 index, PropertyOperationFlags flags)
    {
        const PropertyRecord* propertyRecord = nullptr;
        if (TryFindPropertyRecordForIndex(index, &propertyRecord))
        {
            return DeleteProperty(propertyRecord->GetPropertyId(), flags);
        }
        return TRUE;
    }

    BOOL ModuleNamespace::DeleteProperty(PropertyId propertyId, PropertyOperationFlags flags)
    {
        //Assert: IsPropertyKey(P) is true.
        //Let exports be O.[[Exports]].
        //If P is an element of exports, return false.
        //Return true.
        if (HasProperty(propertyId))
        {
            JavascriptError::ThrowCantDeleteIfStrictModeOrNonconfigurable(
                flags,
                GetScriptContext(),
                GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId)->GetBuffer());
            return FALSE;
        }
        return TRUE;
    }

    BOOL ModuleNamespace::DeleteProperty(JavascriptString *propertyNameString, PropertyOperationFlags flags)
    {
        //Assert: IsPropertyKey(P) is true.
        //Let exports be O.[[Exports]].
        //If P is an element of exports, return false.
        //Return true.
        PropertyRecord const *propertyRecord = nullptr;
        if (JavascriptOperators::ShouldTryDeleteProperty(this, propertyNameString, &propertyRecord))
        {
            Assert(propertyRecord);
            return DeleteProperty(propertyRecord->GetPropertyId(), flags);
        }

        return TRUE;
    }

    BOOL ModuleNamespace::GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("{ModuleNamespaceObject}"));
        return TRUE;
    }

    BOOL ModuleNamespace::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("Object, (ModuleNamespaceObject)"));
        return TRUE;
    }

    Var ModuleNamespace::GetNSSlot(BigPropertyIndex propertyIndex)
    {
        Assert((uint)propertyIndex < static_cast<SourceTextModuleRecord*>(static_cast<ModuleRecordBase*>(moduleRecord))->GetLocalExportCount());
        Var value = this->nsSlots[propertyIndex];
        // Local export slots are pre-filled with the undeclared-block-var
        // sentinel; reading an export whose declaration has not been evaluated
        // yet is in TDZ and must throw ReferenceError (matches the
        // LdModuleSlot/ChkUndecl path for direct import reads).
        ScriptContext* scriptContext = GetScriptContext();
        if (scriptContext->IsUndeclBlockVar(value))
        {
            JavascriptError::ThrowReferenceError(scriptContext, JSERR_UseBeforeDeclaration);
        }
        return value;
    }

    PropertyId ModuleNamespace::GetPropertyId(BigPropertyIndex index)
    {
        SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor;
        if (propertyMap->TryGetValueAt(index, &propertyDescriptor))
        {
            const PropertyRecord* propertyRecord = propertyMap->GetKeyAt(index);
            return propertyRecord->GetPropertyId();
        }
        return Constants::NoProperty;
    }

    BOOL ModuleNamespace::FindNextProperty(BigPropertyIndex& index, JavascriptString** propertyString, PropertyId* propertyId, PropertyAttributes* attributes, ScriptContext * requestContext) const
    {
        if (index < propertyMap->Count())
        {
            SimpleDictionaryPropertyDescriptor<BigPropertyIndex> propertyDescriptor(propertyMap->GetValueAt(index));
            Assert(propertyDescriptor.Attributes == PropertyModuleNamespaceDefault);
            const PropertyRecord* propertyRecord = propertyMap->GetKeyAt(index);
            *propertyId = propertyRecord->GetPropertyId();
            if (propertyString != nullptr)
            {
                *propertyString = requestContext->GetPropertyString(*propertyId);
            }
            if (attributes != nullptr)
            {
                *attributes = propertyDescriptor.Attributes;
            }
            return TRUE;
        }
        else
        {
            *propertyId = Constants::NoProperty;
            if (propertyString != nullptr)
            {
                *propertyString = nullptr;
            }
        }
        return FALSE;
    }
}
