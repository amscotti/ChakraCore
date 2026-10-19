//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeTypePch.h"

namespace Js
{
    bool DynamicObjectPropertyEnumerator::GetEnumNonEnumerable() const
    {
        return !!(flags & EnumeratorFlags::EnumNonEnumerable);
    }
    bool DynamicObjectPropertyEnumerator::GetEnumSymbols() const
    {
        return !!(flags & EnumeratorFlags::EnumSymbols);
    }
    bool DynamicObjectPropertyEnumerator::GetSnapShotSemantics() const
    {
        return !!(flags & EnumeratorFlags::SnapShotSemantics);
    }

    bool DynamicObjectPropertyEnumerator::GetUseCache() const
    {
#if ENABLE_TTD
        if(this->scriptContext->GetThreadContext()->IsRuntimeInTTDMode())
        {
            return false;
        }
#endif

        return ((flags & (EnumeratorFlags::SnapShotSemantics | EnumeratorFlags::UseCache)) == (EnumeratorFlags::SnapShotSemantics | EnumeratorFlags::UseCache));
    }

    void DynamicObjectPropertyEnumerator::Initialize(DynamicType * type, CachedData * data, Js::BigPropertyIndex initialPropertyCount)
    {
        this->initialType = type;
        this->cachedData = data;
        this->initialPropertyCount = initialPropertyCount;
    }

    bool DynamicObjectPropertyEnumerator::Initialize(DynamicObject * object, EnumeratorFlags flags, ScriptContext * requestContext, EnumeratorCache * enumeratorCache)
    {
        this->scriptContext = requestContext;
        this->object = object;
        this->flags = flags;

        if (!object)
        {
            this->cachedData = nullptr;
            return true;
        }

        this->objectIndex = Constants::NoBigSlot;
        this->enumeratedCount = 0;

        if (!GetUseCache())
        {
            if (!object->GetDynamicType()->GetTypeHandler()->EnsureObjectReady(object))
            {
                return false;
            }
            Initialize(object->GetDynamicType(), nullptr, GetSnapShotSemantics() ? this->object->GetPropertyCount() : Constants::NoBigSlot);
            return true;
        }

        DynamicType * type = object->GetDynamicType();

        CachedData * data;
        if (enumeratorCache && type == enumeratorCache->type)
        {
            data = (CachedData *)enumeratorCache->data;

            Assert(data != nullptr);
            Assert(data->scriptContext == this->scriptContext); // The cache data script context should be the same as request context

            if (data->enumNonEnumerable == GetEnumNonEnumerable() && data->enumSymbols == GetEnumSymbols())
            {
                Initialize(type, data, data->propertyCount);
                return true;
            }
        }

        data = (CachedData *)requestContext->GetThreadContext()->GetDynamicObjectEnumeratorCache(type);

        if (data != nullptr && data->scriptContext == this->scriptContext && data->enumNonEnumerable == GetEnumNonEnumerable() && data->enumSymbols == GetEnumSymbols())
        {
            Initialize(type, data, data->propertyCount);

            if (enumeratorCache)
            {
                enumeratorCache->type = type;
                enumeratorCache->data = data;
            }
            return true;
        }

        if (!object->GetDynamicType()->GetTypeHandler()->EnsureObjectReady(object))
        {
            return false;
        }

        // Reload the type after EnsureObjectReady
        type = object->GetDynamicType();
        auto fillCache = [this](CachedData* cachedData, uint propertyCount)
        {
            PropertyId propertyId;
            PropertyAttributes attributes;
            while (this->enumeratedCount < static_cast<int>(propertyCount))
            {
                JavascriptString* propertyString = this->MoveAndGetNextNoCache(propertyId, &attributes);
                if (propertyString == nullptr)
                {
                    break;
                }

                PropertyString* propertyStringTyped = JavascriptOperators::TryFromVar<PropertyString>(propertyString);
                if (propertyStringTyped != nullptr)
                {
                    cachedData->strings[enumeratedCount] = propertyStringTyped;
                    cachedData->indexes[enumeratedCount] = this->objectIndex;
                    cachedData->attributes[enumeratedCount] = attributes;
                    cachedData->cachedCount = ++this->enumeratedCount;
                }
            }
            cachedData->completed = true;
            this->enumeratedCount = 0;
            this->objectIndex = Constants::NoBigSlot;
        };
        if (!type->PrepareForTypeSnapshotEnumeration())
        {
            // Non-lockable handlers (the dictionary handlers) cannot freeze their type for the shared
            // enumerator cache. Snapshot their keys here instead: a setter can delete/re-add a property
            // while a for-in/JSON/Object.assign/spread enumeration is in flight, and a live map walk would
            // then skip or duplicate entries. The local cache is owned by this enumerator only; a null
            // scriptContext marks it as local and keeps the JIT fast path off.
            if (GetSnapShotSemantics() && !type->GetTypeHandler()->IsLockable())
            {
                uint propertyCount = this->object->GetPropertyCountForEnum();
                if (propertyCount == 0)
                {
                    Initialize(type, nullptr, 0);
                    return true;
                }

                data = RecyclerNewStructPlus(requestContext->GetRecycler(),
                    propertyCount * sizeof(Field(PropertyString*)) + propertyCount * sizeof(BigPropertyIndex) + propertyCount * sizeof(PropertyAttributes), CachedData);
                data->scriptContext = nullptr;
                data->cachedCount = 0;
                data->propertyCount = propertyCount;
                data->strings = reinterpret_cast<Field(PropertyString*)*>(data + 1);
                data->indexes = unsafe_write_barrier_cast<BigPropertyIndex *>(data->strings + propertyCount);
                data->attributes = (PropertyAttributes*)(data->indexes + propertyCount);
                data->completed = false;
                data->enumNonEnumerable = GetEnumNonEnumerable();
                data->enumSymbols = GetEnumSymbols();
                Initialize(type, data, propertyCount);
                fillCache(data, propertyCount);
                return true;
            }

            Initialize(type, nullptr, object->GetPropertyCount());
            return true;
        }

        uint propertyCount = this->object->GetPropertyCountForEnum();
        data = RecyclerNewStructPlus(requestContext->GetRecycler(),
            propertyCount * sizeof(Field(PropertyString*)) + propertyCount * sizeof(BigPropertyIndex) + propertyCount * sizeof(PropertyAttributes), CachedData);
        data->scriptContext = requestContext;
        data->cachedCount = 0;
        data->propertyCount = propertyCount;
        data->strings = reinterpret_cast<Field(PropertyString*)*>(data + 1);
        data->indexes = unsafe_write_barrier_cast<BigPropertyIndex *>(data->strings + propertyCount);
        data->attributes = (PropertyAttributes*)(data->indexes + propertyCount);
        data->completed = false;
        data->enumNonEnumerable = GetEnumNonEnumerable();
        data->enumSymbols = GetEnumSymbols();
        requestContext->GetThreadContext()->AddDynamicObjectEnumeratorCache(type, data);
        Initialize(type, data, propertyCount);

        // Fill the locked-type snapshot now rather than lazily. If user code mutates the object during
        // enumeration (delete/re-add, conversion to another handler), the in-flight enumerator must keep
        // walking the keys captured at the start; a lazy walk would observe the rebuilt map.
        fillCache(data, propertyCount);

        if (enumeratorCache)
        {
            enumeratorCache->type = type;
            enumeratorCache->data = data;
        }
        return true;
    }

    bool DynamicObjectPropertyEnumerator::IsNullEnumerator() const
    {
        return this->object == nullptr;
    }

    bool DynamicObjectPropertyEnumerator::CanUseJITFastPath() const
    {
#if ENABLE_TTD
        TTDAssert(this->cachedData == nullptr || !this->scriptContext->GetThreadContext()->IsRuntimeInTTDMode(), "We should always have cachedData null if we are in record or replay mode");
#endif

        return !this->IsNullEnumerator() && !GetEnumNonEnumerable() && this->cachedData != nullptr &&
            this->cachedData->scriptContext != nullptr;
    }

    void DynamicObjectPropertyEnumerator::Clear(EnumeratorFlags flags, ScriptContext * requestContext)
    {
        Initialize(nullptr, flags, requestContext, nullptr);
    }

    void DynamicObjectPropertyEnumerator::Reset()
    {
        Initialize(object, flags, scriptContext, nullptr);
    }

    DynamicType * DynamicObjectPropertyEnumerator::GetTypeToEnumerate() const
    {
        return
            GetSnapShotSemantics() &&
            initialType->GetIsLocked() &&
            CONFIG_FLAG(TypeSnapshotEnumeration)
            ? PointerValue(initialType)
            : object->GetDynamicType();
    }

    JavascriptString * DynamicObjectPropertyEnumerator::MoveAndGetNextWithCache(PropertyId& propertyId, PropertyAttributes* attributes)
    {
#if ENABLE_TTD
        AssertMsg(!this->scriptContext->GetThreadContext()->IsRuntimeInTTDMode(), "We should always trap out to explicit enumeration in this case");
#endif

        Assert(enumeratedCount <= cachedData->cachedCount);
        JavascriptString* propertyStringName = nullptr;
        PropertyAttributes propertyAttributes = PropertyNone;
#if DBG
        bool skipped = false;
#endif
        if (enumeratedCount < cachedData->cachedCount)
        {
            while (enumeratedCount < cachedData->cachedCount)
            {
                PropertyString * propertyString = cachedData->strings[enumeratedCount];
                propertyId = propertyString->GetPropertyId();

                // Snapshot keys are not a guarantee that the property still exists: user code during enumeration
                // may delete it. Skip removed keys, and use the current enumerability. A deleted and re-added key
                // exists again and is returned once; it is not duplicated because its snapshot slot is consumed.
                bool exists = this->object->HasOwnProperty(propertyId) != FALSE;
                bool enumerable = exists && this->object->IsEnumerable(propertyId) != FALSE;
                if (!exists || (!enumerable && !GetEnumNonEnumerable()))
                {
                    enumeratedCount++;
#if DBG
                    skipped = true;
#endif
                    continue;
                }

                propertyStringName = propertyString;

#if DBG
                if (!skipped && cachedData->scriptContext != nullptr && this->initialType == this->object->GetDynamicType())
                {
                    // Shared type caches enumerate a locked snapshot. A local dictionary snapshot, a skipped key,
                    // or a type change during enumeration can outlive a live map reorder; the cross-check is not
                    // valid then.
                    PropertyId tempPropertyId;
                    /* JavascriptString * tempPropertyString = */ this->MoveAndGetNextNoCache(tempPropertyId, attributes);

                    Assert(tempPropertyId == propertyId);
                    Assert(this->objectIndex == cachedData->indexes[enumeratedCount]);
                }
#endif

                this->objectIndex = cachedData->indexes[enumeratedCount];
                propertyAttributes = cachedData->attributes[enumeratedCount];
                if (enumerable)
                {
                    propertyAttributes |= PropertyEnumerable;
                }
                else
                {
                    propertyAttributes &= ~PropertyEnumerable;
                }

                enumeratedCount++;
                if (attributes != nullptr)
                {
                    *attributes = propertyAttributes;
                }
                return propertyStringName;
            }
        }
        else if (!cachedData->completed)
        {
            propertyStringName = this->MoveAndGetNextNoCache(propertyId, &propertyAttributes);

            if (propertyStringName)
            {
                PropertyString* propertyString = JavascriptOperators::TryFromVar<PropertyString>(propertyStringName);
                if (propertyString != nullptr)
                {
                    Assert(enumeratedCount < this->initialPropertyCount);
                    cachedData->strings[enumeratedCount] = propertyString;
                    cachedData->indexes[enumeratedCount] = this->objectIndex;
                    cachedData->attributes[enumeratedCount] = propertyAttributes;
                    cachedData->cachedCount = ++enumeratedCount;
                }
            }
            else
            {
                cachedData->completed = true;
            }
        }
        else
        {
#if DBG
            if (cachedData->scriptContext != nullptr && this->initialType == this->object->GetDynamicType())
            {
                PropertyId tempPropertyId;
                Assert(this->MoveAndGetNextNoCache(tempPropertyId, attributes) == nullptr);
            }
#endif

            propertyStringName = nullptr;
        }

        if (attributes != nullptr)
        {
            *attributes = propertyAttributes;
        }

        return propertyStringName;
    }
    JavascriptString * DynamicObjectPropertyEnumerator::MoveAndGetNextNoCache(PropertyId& propertyId, PropertyAttributes * attributes)
    {
        JavascriptString* propertyString = nullptr;
        BigPropertyIndex newIndex = this->objectIndex;
        PropertyValueInfo info;
        RecyclableObject * startingObject = this->object;
        do
        {
            newIndex++;
            PropertyValueInfo::ClearCacheInfo(&info);
            if (!this->object->FindNextProperty(newIndex, &propertyString, &propertyId, attributes,
                GetTypeToEnumerate(), flags, this->scriptContext, &info)
                || (GetSnapShotSemantics() && PropertyIndexToPropertyEnumeration(newIndex) >= initialPropertyCount))
            {
                // No more properties
                newIndex--;
                propertyString = nullptr;
                PropertyValueInfo::ClearCacheInfo(&info);
                break;
            }
        } while (Js::IsInternalPropertyId(propertyId));

        if (info.GetPropertyRecordUsageCache() != nullptr && info.GetPropertyRecordUsageCache()->ShouldUseCache() && propertyString == info.GetProperty())
        {
            CacheOperators::CachePropertyRead(startingObject, this->object, false, propertyId, false, &info, scriptContext);
            if ((!(this->flags & EnumeratorFlags::EphemeralReference)) && info.IsStoreFieldCacheEnabled() && info.IsWritable() && ((info.GetFlags() & (InlineCacheGetterFlag | InlineCacheSetterFlag)) == 0))
            {
                PropertyValueInfo::SetCacheInfo(&info, info.GetPropertyRecordUsageCache()->GetStElemInlineCache(), info.AllowResizingPolymorphicInlineCache());
                CacheOperators::CachePropertyWrite(this->object, false, this->object->GetType(), propertyId, &info, scriptContext);
            }
        }
        this->objectIndex = newIndex;
        return propertyString;
    }

    JavascriptString * DynamicObjectPropertyEnumerator::MoveAndGetNext(PropertyId& propertyId, PropertyAttributes * attributes)
    {
        propertyId = Js::Constants::NoProperty;
        if (this->cachedData &&
            (this->cachedData->scriptContext == nullptr || (GetSnapShotSemantics() && this->initialType->GetIsLocked())))
        {
            // A shared cache holds the initial, locked type's keys. The object may have changed type since
            // enumeration began (for example because a delete/re-add converted its handler); the snapshot is
            // still the enumeration state, so do not fall back to the live map here.
            return MoveAndGetNextWithCache(propertyId, attributes);
        }
        if (this->object)
        {
            // Once enters NoCache path, ensure never switches to Cache path above.
            this->cachedData = nullptr;
            return MoveAndGetNextNoCache(propertyId, attributes);
        }
        return nullptr;
    }
}
