//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Implementation for typed arrays based on ArrayBuffer.
// There is one nested ArrayBuffer for each typed array. Multiple typed array
// can share the same array buffer.
//----------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "AtomicsOperations.h"

#define INSTANTIATE_BUILT_IN_ENTRYPOINTS(typeName) \
    template Var typeName::NewInstance(RecyclableObject* function, CallInfo callInfo, ...); \
    template Var typeName::EntrySet(RecyclableObject* function, CallInfo callInfo, ...);

namespace Js
{
    namespace
    {
        // Reinterpret the low 64 bits as a signed integer without relying on
        // an implementation-defined uint64-to-int64 conversion.
        inline int64 BigInt64Bits(uint64 bits)
        {
            const uint64 int64Max = 0x7fffffffffffffffULL;
            if (bits <= int64Max)
            {
                return static_cast<int64>(bits);
            }
            const int64 int64Min = -0x7fffffffffffffffLL - 1;
            return static_cast<int64>(bits - (int64Max + 1)) + int64Min;
        }

        // ToBigInt64 / ToBigUint64 for typed-array element stores. ToBigInt
        // performs the observable conversion (including user code); the low
        // 64 bits of the two's-complement value are stored.
        int64 ToBigInt64Element(Var value, ScriptContext* scriptContext)
        {
            return BigInt64Bits(JavascriptConversion::ToBigInt(value, scriptContext)->ToUInt64Low());
        }

        uint64 ToBigUint64Element(Var value, ScriptContext* scriptContext)
        {
            return JavascriptConversion::ToBigInt(value, scriptContext)->ToUInt64Low();
        }

        inline Var BigInt64ElementAt(int64* buffer, uint32 index, ScriptContext* scriptContext)
        {
            return JavascriptBigInt::CreateFromInt64(buffer[index], scriptContext);
        }

        inline Var BigUint64ElementAt(uint64* buffer, uint32 index, ScriptContext* scriptContext)
        {
            return JavascriptBigInt::CreateFromUInt64(buffer[index], scriptContext);
        }

        // Float16Array element conversions. Store runs the observable
        // ToNumber (including user code), then rounds to binary16 with the
        // shared Float16 codec so stores agree with DataView.setFloat16;
        // load widens exactly with Float16::ToDouble.
        Float16Bits ToFloat16Element(Var value, ScriptContext* scriptContext)
        {
            return Float16Bits(Float16::ToBits(JavascriptConversion::ToNumber(value, scriptContext)));
        }

        inline Var Float16ElementAt(uint16* buffer, uint32 index, ScriptContext* scriptContext)
        {
            return JavascriptNumber::ToVarWithCheck(Float16::ToDouble(buffer[index]), scriptContext);
        }
    }

    // explicitly instantiate these function for the built in entry point FunctionInfo
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Int8Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Uint8Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Uint8ClampedArray)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Int16Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Uint16Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Int32Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Uint32Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Float32Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Float64Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Int64Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Uint64Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(Float16Array)
    INSTANTIATE_BUILT_IN_ENTRYPOINTS(BoolArray)

    template <> bool VarIsImpl<Uint8ClampedArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint8ClampedArray &&
               ( VirtualTableInfo<Uint8ClampedArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint8ClampedArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint8Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint8Array &&
              ( VirtualTableInfo<Uint8Array>::HasVirtualTable(obj) ||
                VirtualTableInfo<CrossSiteObject<Uint8Array>>::HasVirtualTable(obj)
              );
    }

    template <> bool VarIsImpl<Int8Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int8Array &&
               ( VirtualTableInfo<Int8Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int8Array>>::HasVirtualTable(obj)
               );
    }


    template <> bool VarIsImpl<Int16Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int16Array &&
               ( VirtualTableInfo<Int16Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int16Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint16Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint16Array &&
              ( VirtualTableInfo<Uint16Array>::HasVirtualTable(obj) ||
                VirtualTableInfo<CrossSiteObject<Uint16Array>>::HasVirtualTable(obj)
              );
    }

    template <> bool VarIsImpl<Int32Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int32Array &&
               ( VirtualTableInfo<Int32Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int32Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint32Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint32Array &&
               ( VirtualTableInfo<Uint32Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint32Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Float32Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Float32Array &&
               ( VirtualTableInfo<Float32Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Float32Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Float64Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Float64Array &&
               ( VirtualTableInfo<Float64Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Float64Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Int64Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int64Array &&
               ( VirtualTableInfo<Int64Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int64Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint64Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint64Array &&
               ( VirtualTableInfo<Uint64Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint64Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Float16Array>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Float16Array &&
               ( VirtualTableInfo<Float16Array>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Float16Array>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<BoolArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_BoolArray &&
               ( VirtualTableInfo<BoolArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<BoolArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint8ClampedVirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint8ClampedArray &&
               ( VirtualTableInfo<Uint8ClampedVirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint8ClampedVirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint8VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint8Array &&
               ( VirtualTableInfo<Uint8VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint8VirtualArray>>::HasVirtualTable(obj)
               );
    }


    template <> bool VarIsImpl<Int8VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int8Array &&
               ( VirtualTableInfo<Int8VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int8VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Int16VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int16Array &&
               ( VirtualTableInfo<Int16VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int16VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint16VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint16Array &&
               ( VirtualTableInfo<Uint16VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint16VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Int32VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Int32Array &&
               ( VirtualTableInfo<Int32VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Int32VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Uint32VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Uint32Array &&
               ( VirtualTableInfo<Uint32VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Uint32VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Float32VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Float32Array &&
               ( VirtualTableInfo<Float32VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Float32VirtualArray>>::HasVirtualTable(obj)
               );
    }

    template <> bool VarIsImpl<Float64VirtualArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_Float64Array &&
               ( VirtualTableInfo<Float64VirtualArray>::HasVirtualTable(obj) ||
                 VirtualTableInfo<CrossSiteObject<Float64VirtualArray>>::HasVirtualTable(obj)
               );
    }

    TypedArrayBase::TypedArrayBase(ArrayBufferBase* arrayBuffer, uint32 offSet, uint mappedLength, uint elementSize, DynamicType* type, bool lengthTracking) :
        ArrayBufferParent(type, mappedLength, arrayBuffer),
        byteOffset(offSet),
        BYTES_PER_ELEMENT(elementSize),
        lengthTracking(lengthTracking)
    {
    }

    inline JsUtil::List<Var, ArenaAllocator>* IteratorToList(RecyclableObject *iterator, ScriptContext *scriptContext, ArenaAllocator *alloc)
    {
        Assert(iterator != nullptr);

        Var nextValue;
        JsUtil::List<Var, ArenaAllocator>* retList = JsUtil::List<Var, ArenaAllocator>::New(alloc);

        RecyclableObject* nextFunc = JavascriptOperators::CacheIteratorNext(iterator, scriptContext);
        while (JavascriptOperators::IteratorStepAndValue(iterator, scriptContext, nextFunc, &nextValue))
        {
            retList->Add(nextValue);
        }

        return retList;
    }

    Var TypedArrayBase::CreateNewInstanceFromIterator(RecyclableObject *iterator, ScriptContext *scriptContext, uint32 elementSize, PFNCreateTypedArray pfnCreateTypedArray)
    {
        TypedArrayBase *newArr = nullptr;

        DECLARE_TEMP_GUEST_ALLOCATOR(tempAlloc);

        ACQUIRE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext, _u("Runtime"));
        {
            JsUtil::List<Var, ArenaAllocator>* tempList = IteratorToList(iterator, scriptContext, tempAlloc);

            uint32 len = tempList->Count();
            uint32 byteLen;

            if (UInt32Math::Mul(len, elementSize, &byteLen))
            {
                JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayLength);
            }

            ArrayBufferBase *arrayBuffer = scriptContext->GetLibrary()->CreateArrayBuffer(byteLen);
            newArr = static_cast<TypedArrayBase*>(pfnCreateTypedArray(arrayBuffer, 0, len, scriptContext->GetLibrary(), false));

            for (uint32 k = 0; k < len; k++)
            {
                Var kValue = tempList->Item(k);
                newArr->SetItem(k, kValue);
            }
        }
        RELEASE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext);

        return newArr;
    }

    Var TypedArrayBase::CreateNewInstance(Arguments& args, ScriptContext* scriptContext, uint32 elementSize, PFNCreateTypedArray pfnCreateTypedArray)
    {
        uint32 byteLength = 0;
        int32 offset = 0;
        int32 mappedLength = -1;
        uint32 elementCount = 0;
        ArrayBufferBase* arrayBuffer = nullptr;
        TypedArrayBase* typedArraySource = nullptr;
        RecyclableObject* jsArraySource = nullptr;
        bool fromExternalObject = false;
        bool lengthTracking = false;

        // Handle first argument - see if that is ArrayBuffer/SharedArrayBuffer
        if (args.Info.Count > 1)
        {
            Var firstArgument = args[1];
            if (!Js::JavascriptOperators::IsObject(firstArgument))
            {
                elementCount = ArrayBuffer::ToIndex(firstArgument, JSERR_InvalidTypedArrayLength, scriptContext, ArrayBuffer::MaxArrayBufferLength / elementSize);
            }
            else
            {
                if (VarIs<TypedArrayBase>(firstArgument))
                {
                    // Constructor(TypedArray array)
                    typedArraySource = static_cast<TypedArrayBase*>(firstArgument);
                    if (typedArraySource->IsDetachedBuffer())
                    {
                        JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray]"));
                    }

                    elementCount = typedArraySource->GetLength();
                    if (elementCount >= ArrayBuffer::MaxArrayBufferLength / elementSize)
                    {
                        JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayLength);
                    }
                }
                else if (VarIs<ArrayBufferBase>(firstArgument))
                {
                    // Constructor(ArrayBuffer buffer,
                    //  optional uint32 byteOffset, optional uint32 length)
                    arrayBuffer = VarTo<ArrayBufferBase>(firstArgument);
                    if (arrayBuffer->IsDetached())
                    {
                        JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray]"));
                    }
                }
                else
                {
                    // Use GetIteratorFunction instead of GetIterator to check if it is the built-in array iterator
                    RecyclableObject* iteratorFn = JavascriptOperators::GetIteratorFunction(firstArgument, scriptContext, true /* optional */);
                    if (iteratorFn != nullptr &&
                        (iteratorFn != scriptContext->GetLibrary()->EnsureArrayPrototypeValuesFunction() ||
                            !JavascriptArray::IsNonES5Array(firstArgument) || JavascriptLibrary::ArrayIteratorPrototypeHasUserDefinedNext(scriptContext)))
                    {
                        Var iterator = scriptContext->GetThreadContext()->ExecuteImplicitCall(iteratorFn, Js::ImplicitCall_Accessor, [=]()->Js::Var
                        {
                            return CALL_FUNCTION(scriptContext->GetThreadContext(), iteratorFn, CallInfo(Js::CallFlags_Value, 1), VarTo<RecyclableObject>(firstArgument));
                        });

                        if (!JavascriptOperators::IsObject(iterator))
                        {
                            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
                        }
                        return CreateNewInstanceFromIterator(VarTo<RecyclableObject>(iterator), scriptContext, elementSize, pfnCreateTypedArray);
                    }

                    if (!JavascriptConversion::ToObject(firstArgument, scriptContext, &jsArraySource))
                    {
                        // REVIEW: unclear why this JavascriptConversion::ToObject() call is being made.
                        // It ends up calling RecyclableObject::ToObject which at least Proxy objects can
                        // hit with non-trivial behavior.
                        Js::Throw::FatalInternalError();
                    }

                    Var lengthVar = JavascriptOperators::OP_GetLength(jsArraySource, scriptContext);
                    elementCount = ArrayBuffer::ToIndex(lengthVar, JSERR_InvalidTypedArrayLength, scriptContext, ArrayBuffer::MaxArrayBufferLength / elementSize);
                }
            }
        }

        // If we got an ArrayBuffer, we can continue to process arguments.
        if (arrayBuffer != nullptr)
        {
            // A view over a resizable ArrayBuffer or a growable SharedArrayBuffer
            // without an explicit length is length-tracking; only a non-undefined
            // fourth argument captures a fixed length (explicit undefined still
            // tracks). SharedArrayBuffer is never detachable, so tracking only
            // grows.
            bool resizableBuffer = VarIs<ArrayBuffer>(arrayBuffer) && VarTo<ArrayBuffer>(arrayBuffer)->IsResizable();
            bool growableSharedBuffer = VarIs<SharedArrayBuffer>(arrayBuffer) && VarTo<SharedArrayBuffer>(arrayBuffer)->IsGrowable();
            lengthTracking = (resizableBuffer || growableSharedBuffer) &&
                !(args.Info.Count > 3 && !JavascriptOperators::IsUndefinedObject(args[3]));

            // Both ToIndex calls below can run user code that resizes or
            // detaches the buffer. Coerce first, then re-read the current
            // byte length before computing the element range.
            if (args.Info.Count > 2)
            {
                offset = ArrayBuffer::ToIndex(args[2], JSERR_InvalidTypedArrayLength, scriptContext, ArrayBuffer::MaxArrayBufferLength, false);
            }

            if (arrayBuffer->IsDetached())
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray]"));
            }

            byteLength = arrayBuffer->GetByteLength();
            if ((uint32)offset > byteLength)
            {
                JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayLength);
            }

            // we can start the mapping from the end of the incoming buffer, but with a map of 0 length.
            // User can't really do anything useful with the typed array but apparently this is allowed.
            if ((offset % elementSize) != 0)
            {
                JavascriptError::ThrowRangeError(
                    scriptContext, JSERR_InvalidTypedArrayLength);
            }

            if (args.Info.Count > 3 && !JavascriptOperators::IsUndefinedObject(args[3]))
            {
                mappedLength = ArrayBuffer::ToIndex(args[3], JSERR_InvalidTypedArrayLength, scriptContext, ArrayBuffer::MaxArrayBufferLength / elementSize, false);
            }
            else
            {
                // A length-tracking view floors a partial trailing element;
                // a fixed view must fit exactly within the initial buffer.
                if (!lengthTracking && (byteLength - offset) % elementSize != 0)
                {
                    JavascriptError::ThrowRangeError(
                        scriptContext, JSERR_InvalidTypedArrayLength);
                }
                mappedLength = (byteLength - offset)/elementSize;
            }

            // Validation above may have been reordered by user code in the
            // length conversion; verify the view still fits.
            if (arrayBuffer->IsDetached())
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray]"));
            }

            byteLength = arrayBuffer->GetByteLength();
            if ((uint32)offset > byteLength ||
                (!lengthTracking && (uint32)mappedLength > (byteLength - (uint32)offset) / elementSize))
            {
                JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayLength);
            }

            if (lengthTracking)
            {
                // The stored length is only a construction snapshot; lookups
                // use the current buffer byte length.
                mappedLength = (byteLength - (uint32)offset) / elementSize;
            }
        }
        else
        {
            // Null arrayBuffer - could be new constructor or copy constructor.
            byteLength = elementCount * elementSize;
            arrayBuffer = scriptContext->GetLibrary()->CreateArrayBuffer(byteLength);
        }

        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray]"));
        }

        if (mappedLength == -1)
        {
            mappedLength = (byteLength - offset)/elementSize;
        }

        // Create and set the array based on the source.
        TypedArrayBase* newArray  = static_cast<TypedArrayBase*>(pfnCreateTypedArray(arrayBuffer, offset, mappedLength, scriptContext->GetLibrary(), lengthTracking));
        if (fromExternalObject)
        {
            // No need to copy externally provided buffer
            OUTPUT_TRACE(TypedArrayPhase, _u("Created a TypedArray from an external buffer source\n"));
        }
        else if (typedArraySource)
        {
            newArray->Set(typedArraySource, offset);
            OUTPUT_TRACE(TypedArrayPhase, _u("Created a TypedArray from a typed array source\n"));
        }
        else if (jsArraySource)
        {
            newArray->SetObjectNoDetachCheck(jsArraySource, newArray->GetLength(), offset);
            OUTPUT_TRACE(TypedArrayPhase, _u("Created a TypedArray from a JavaScript array source\n"));
        }

        return newArray;
    }

    Var TypedArrayBase::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        function->GetScriptContext()->GetThreadContext()->ProbeStack(Js::Constants::MinStackDefault, function->GetScriptContext());

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);

        JavascriptError::ThrowTypeError(scriptContext, JSERR_InvalidTypedArray_Constructor);
    }

    template <typename TypeName, bool clamped, bool virtualAllocated>
    Var TypedArray<TypeName, clamped, virtualAllocated>::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        function->GetScriptContext()->GetThreadContext()->ProbeStack(Js::Constants::MinStackDefault, function->GetScriptContext());

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::IsConstructorSuperCall(args);
        Assert(isCtorSuperCall || !(callInfo.Flags & CallFlags_New) || args[0] == nullptr);

        if (!(callInfo.Flags & CallFlags_New) || (newTarget && JavascriptOperators::IsUndefinedObject(newTarget)))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("[TypedArray]"));
        }

        Var object = nullptr;
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            object = TypedArray::CreateNewInstance(args, scriptContext, sizeof(TypeName), TypedArray<TypeName, clamped, virtualAllocated>::Create);
        }
        END_SAFE_REENTRANT_CALL

#if ENABLE_DEBUG_CONFIG_OPTIONS
        if (Js::Configuration::Global.flags.IsEnabled(Js::autoProxyFlag))
        {
            object = Js::JavascriptProxy::AutoProxyWrapper(object);
        }
#endif
        return isCtorSuperCall ?
            JavascriptOperators::OrdinaryCreateFromConstructor(VarTo<RecyclableObject>(newTarget), VarTo<RecyclableObject>(object), nullptr, scriptContext) :
            object;
    };

    BOOL TypedArrayBase::HasOwnProperty(PropertyId propertyId)
    {
        return JavascriptConversion::PropertyQueryFlagsToBoolean(HasPropertyQuery(propertyId, nullptr /*info*/));
    }

    inline BOOL TypedArrayBase::CanonicalNumericIndexString(PropertyId propertyId, ScriptContext *scriptContext)
    {
        PropertyString *propertyString = scriptContext->GetPropertyString(propertyId);
        return CanonicalNumericIndexString(propertyString, scriptContext);
    }

    inline BOOL TypedArrayBase::CanonicalNumericIndexString(JavascriptString *propertyString, ScriptContext *scriptContext)
    {
        double result;
        return JavascriptConversion::CanonicalNumericIndexString(propertyString, &result, scriptContext);
    }

    PropertyQueryFlags TypedArrayBase::HasPropertyQuery(PropertyId propertyId, _Inout_opt_ PropertyValueInfo* info)
    {
        uint32 index = 0;
        ScriptContext *scriptContext = GetScriptContext();
        if (scriptContext->IsNumericPropertyId(propertyId, &index))
        {
            // All the slots within the length of the array are valid.
            return index < this->GetLength() ? PropertyQueryFlags::Property_Found : PropertyQueryFlags::Property_NotFound_NoProto;
        }

        if (!scriptContext->GetPropertyName(propertyId)->IsSymbol() && CanonicalNumericIndexString(propertyId, scriptContext))
        {
            return PropertyQueryFlags::Property_NotFound_NoProto;
        }

        return DynamicObject::HasPropertyQuery(propertyId, info);
    }

    BOOL TypedArrayBase::DeleteProperty(PropertyId propertyId, PropertyOperationFlags flags)
    {
        uint32 index = 0;
        if (GetScriptContext()->IsNumericPropertyId(propertyId, &index) && (index < this->GetLength()))
        {
            // All the slots within the length of the array are valid.
            return false;
        }
        return DynamicObject::DeleteProperty(propertyId, flags);
    }

    BOOL TypedArrayBase::DeleteProperty(JavascriptString *propertyNameString, PropertyOperationFlags flags)
    {
        PropertyRecord const *propertyRecord = nullptr;
        if (JavascriptOperators::ShouldTryDeleteProperty(this, propertyNameString, &propertyRecord))
        {
            Assert(propertyRecord);
            return DeleteProperty(propertyRecord->GetPropertyId(), flags);
        }

        return TRUE;
    }

    PropertyQueryFlags TypedArrayBase::GetPropertyReferenceQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        return TypedArrayBase::GetPropertyQuery(originalInstance, propertyId, value, info, requestContext);
    }

    PropertyQueryFlags TypedArrayBase::GetPropertyQuery(Var originalInstance, PropertyId propertyId, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        const Js::PropertyRecord* propertyRecord = requestContext->GetPropertyName(propertyId);
        if (propertyRecord->IsNumeric())
        {
            *value = this->DirectGetItem(propertyRecord->GetNumericValue());
            if (JavascriptOperators::GetTypeId(*value) == Js::TypeIds_Undefined)
            {
                return PropertyQueryFlags::Property_NotFound;
            }
            return PropertyQueryFlags::Property_Found;
        }
        if (!propertyRecord->IsSymbol() && CanonicalNumericIndexString(propertyId, requestContext))
        {
            *value = requestContext->GetLibrary()->GetUndefined();
            return PropertyQueryFlags::Property_NotFound_NoProto;
        }

        return DynamicObject::GetPropertyQuery(originalInstance, propertyId, value, info, requestContext);
    }

    PropertyQueryFlags TypedArrayBase::GetPropertyQuery(Var originalInstance, JavascriptString* propertyNameString, Var* value, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        AssertMsg(!PropertyRecord::IsPropertyNameNumeric(propertyNameString->GetString(), propertyNameString->GetLength()),
            "Numeric property names should have been converted to uint or PropertyRecord*");

        if (CanonicalNumericIndexString(propertyNameString, requestContext))
        {
            *value = requestContext->GetLibrary()->GetUndefined();
            return PropertyQueryFlags::Property_NotFound_NoProto;
        }

        return DynamicObject::GetPropertyQuery(originalInstance, propertyNameString, value, info, requestContext);
    }

    PropertyQueryFlags TypedArrayBase::HasItemQuery(uint32 index)
    {
        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }

        if (index < GetLength())
        {
            return PropertyQueryFlags::Property_Found;
        }

        return PropertyQueryFlags::Property_NotFound_NoProto;
    }

    BOOL TypedArrayBase::DeleteItem(uint32 index, Js::PropertyOperationFlags flags)
    {
        JavascriptError::ThrowCantDeleteIfStrictModeOrNonconfigurable(
            flags, GetScriptContext(), GetScriptContext()->GetIntegerString(index)->GetString());
        return FALSE;
    }

    PropertyQueryFlags TypedArrayBase::GetItemQuery(Var originalInstance, uint32 index, Var* value, ScriptContext* requestContext)
    {
        *value = DirectGetItem(index);
        return index < GetLength() ? PropertyQueryFlags::Property_Found : PropertyQueryFlags::Property_NotFound_NoProto;
    }

    PropertyQueryFlags TypedArrayBase::GetItemReferenceQuery(Var originalInstance, uint32 index, Var* value, ScriptContext* requestContext)
    {
        *value = DirectGetItem(index);
        return index < GetLength() ? PropertyQueryFlags::Property_Found : PropertyQueryFlags::Property_NotFound_NoProto;
    }

    BOOL TypedArrayBase::SetProperty(PropertyId propertyId, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        ScriptContext *scriptContext = GetScriptContext();

        const PropertyRecord* propertyRecord = scriptContext->GetPropertyName(propertyId);
        if (propertyRecord->IsNumeric())
        {
            this->DirectSetItem(propertyRecord->GetNumericValue(), value);
            return true;
        }

        if (!propertyRecord->IsSymbol() && CanonicalNumericIndexString(propertyId, scriptContext))
        {
            return FALSE;
        }
        else
        {
            return DynamicObject::SetProperty(propertyId, value, flags, info);
        }
    }

    BOOL TypedArrayBase::SetProperty(JavascriptString* propertyNameString, Var value, PropertyOperationFlags flags, PropertyValueInfo* info)
    {
        AssertMsg(!PropertyRecord::IsPropertyNameNumeric(propertyNameString->GetString(), propertyNameString->GetLength()),
            "Numeric property names should have been converted to uint or PropertyRecord*");

        ScriptContext *requestContext = GetScriptContext();
        if (CanonicalNumericIndexString(propertyNameString, requestContext))
        {
            return FALSE; // Integer-index exotic object should not set property that is canonical numeric index string but came to this overload
        }

        return DynamicObject::SetProperty(propertyNameString, value, flags, info);
    }

    DescriptorFlags TypedArrayBase::GetSetter(PropertyId propertyId, Var *setterValue, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        if (!requestContext->GetPropertyName(propertyId)->IsSymbol() && CanonicalNumericIndexString(propertyId, requestContext))
        {
            return None_NoProto;
        }

        return __super::GetSetter(propertyId, setterValue, info, requestContext);
    }

    DescriptorFlags TypedArrayBase::GetSetter(JavascriptString* propertyNameString, Var *setterValue, PropertyValueInfo* info, ScriptContext* requestContext)
    {
        if (CanonicalNumericIndexString(propertyNameString, requestContext))
        {
            return None_NoProto;
        }

        return __super::GetSetter(propertyNameString, setterValue, info, requestContext);
    }

    DescriptorFlags TypedArrayBase::GetItemSetter(uint32 index, Var* setterValue, ScriptContext* requestContext)
    {
#if ENABLE_COPYONACCESS_ARRAY
        JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Var>(this);
#endif
        if (index >= this->GetLength())
        {
            return None_NoProto;
        }

        return __super::GetItemSetter(index, setterValue, requestContext);
    }

    BOOL TypedArrayBase::GetEnumerator(JavascriptStaticEnumerator * enumerator, EnumeratorFlags flags, ScriptContext* requestContext, EnumeratorCache * enumeratorCache)
    {
        return enumerator->Initialize(nullptr, this, this, flags, requestContext, enumeratorCache);
    }

    JavascriptEnumerator * TypedArrayBase::GetIndexEnumerator(EnumeratorFlags flags, ScriptContext * requestContext)
    {
        return RecyclerNew(requestContext->GetRecycler(), TypedArrayIndexEnumerator, this, flags, requestContext);
    }

    BOOL TypedArrayBase::SetItem(uint32 index, Var value, PropertyOperationFlags flags)
    {
        // [[Set]] step 1.b.ii: any canonical numeric assignment to an
        // immutable view reports failure before value conversion.
        if (IsImmutableBuffer(GetArrayBuffer()))
        {
            return false;
        }

        // BigInt element stores perform ToBigInt even when the index is out
        // of bounds or the view is detached; the conversion and its side
        // effects are observable (IntegerIndexedElementSet). Other element
        // types keep the existing early-out optimization.
        if (flags == PropertyOperation_None && index >= GetLength() && !IsBigIntElementType())
        {
            return false;
        }

        DirectSetItem(index, value);
        return true;
    }

    BOOL TypedArrayBase::IsEnumerable(PropertyId propertyId)
    {
        uint32 index = 0;
        if (GetScriptContext()->IsNumericPropertyId(propertyId, &index))
        {
            return true;
        }
        return DynamicObject::IsEnumerable(propertyId);
    }

    BOOL TypedArrayBase::IsConfigurable(PropertyId propertyId)
    {
        uint32 index = 0;
        if (GetScriptContext()->IsNumericPropertyId(propertyId, &index))
        {
            return false;
        }
        return DynamicObject::IsConfigurable(propertyId);
    }


    BOOL TypedArrayBase::InitProperty(Js::PropertyId propertyId, Js::Var value, PropertyOperationFlags flags, Js::PropertyValueInfo* info)
    {
        return SetProperty(propertyId, value, flags, info);
    }

    BOOL TypedArrayBase::IsWritable(PropertyId propertyId)
    {
        uint32 index = 0;
        if (GetScriptContext()->IsNumericPropertyId(propertyId, &index))
        {
            // Integer-indexed properties of an immutable buffer are
            // non-writable (proposal 10.4.5.1).
            return !IsImmutableBuffer(GetArrayBuffer());
        }
        return DynamicObject::IsWritable(propertyId);
    }


    BOOL TypedArrayBase::SetEnumerable(PropertyId propertyId, BOOL value)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 index;

        // all numeric properties are enumerable
        if (scriptContext->IsNumericPropertyId(propertyId, &index) )
        {
            if (!value)
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotConfigurable,
                    scriptContext->GetThreadContext()->GetPropertyName(propertyId)->GetBuffer());
            }
            return true;
        }

        return __super::SetEnumerable(propertyId, value);
    }

    BOOL TypedArrayBase::SetWritable(PropertyId propertyId, BOOL value)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 index;

        // default is writable
        if (scriptContext->IsNumericPropertyId(propertyId, &index))
        {
            if (!value)
            {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotConfigurable,
                scriptContext->GetThreadContext()->GetPropertyName(propertyId)->GetBuffer());
            }
            return true;
        }

        return __super::SetWritable(propertyId, value);
    }

    BOOL TypedArrayBase::SetConfigurable(PropertyId propertyId, BOOL value)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 index;

        // default is not configurable
        if (scriptContext->IsNumericPropertyId(propertyId, &index))
        {
            if (value)
            {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotConfigurable,
                scriptContext->GetThreadContext()->GetPropertyName(propertyId)->GetBuffer());
            }
            return true;
        }

        return __super::SetConfigurable(propertyId, value);
    }

    BOOL TypedArrayBase::SetAttributes(PropertyId propertyId, PropertyAttributes attributes)
    {
        ScriptContext* scriptContext = this->GetScriptContext();
        uint32 index;

        if (scriptContext->IsNumericPropertyId(propertyId, &index))
        {
            VerifySetItemAttributes(propertyId, attributes);
            return true;
        }

        return __super::SetAttributes(propertyId, attributes);
    }

    BOOL TypedArrayBase::SetAccessors(PropertyId propertyId, Var getter, Var setter, PropertyOperationFlags flags)
    {
        ScriptContext* scriptContext = this->GetScriptContext();
        uint32 index;

        if (scriptContext->IsNumericPropertyId(propertyId, &index))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotConfigurable,
                GetScriptContext()->GetThreadContext()->GetPropertyName(propertyId)->GetBuffer());
        }

        return __super::SetAccessors(propertyId, getter, setter, flags);
    }

    BOOL TypedArrayBase::SetPropertyWithAttributes(PropertyId propertyId, Var value, PropertyAttributes attributes, PropertyValueInfo* info, PropertyOperationFlags flags, SideEffects possibleSideEffects)
    {
        ScriptContext* scriptContext = GetScriptContext();

        const PropertyRecord* propertyRecord = scriptContext->GetPropertyName(propertyId);
        if (propertyRecord->IsNumeric())
        {
            VerifySetItemAttributes(propertyId, attributes);
            return SetItem(propertyRecord->GetNumericValue(), value);
        }

        if (!propertyRecord->IsSymbol() && CanonicalNumericIndexString(propertyId, scriptContext))
        {
            return FALSE;
        }

        return __super::SetPropertyWithAttributes(propertyId, value, attributes, info, flags, possibleSideEffects);
    }

    BOOL TypedArrayBase::SetItemWithAttributes(uint32 index, Var value, PropertyAttributes attributes)
    {
        VerifySetItemAttributes(Constants::NoProperty, attributes);
        return SetItem(index, value);
    }

    BOOL TypedArrayBase::IsObjectArrayFrozen()
    {
        if (GetLength() > 0)
        {
            return false; // the backing buffer is always modifiable
        }
        return IsSealed();
    }

    BOOL TypedArrayBase::Is(TypeId typeId)
    {
        return typeId >= TypeIds_TypedArrayMin && typeId <= TypeIds_TypedArrayMax;
    }

    BOOL TypedArrayBase::IsDetachedTypedArray(Var aValue)
    {
        TypedArrayBase* arr = JavascriptOperators::TryFromVar<TypedArrayBase>(aValue);
        return arr && arr->IsDetachedBuffer();
    }

    void TypedArrayBase::Set(TypedArrayBase* source, uint32 offset)
    {
        uint32 sourceLength = source->GetLength();
        uint32 totalLength;

        if (UInt32Math::Add(offset, sourceLength, &totalLength) ||
            (totalLength > GetLength()))
        {
            JavascriptError::ThrowRangeError(
                GetScriptContext(), JSERR_InvalidTypedArrayLength);
        }

        if (source->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }

        // memmove buffer if views have same bit representation.
        // types of the same size are compatible, with the following exceptions:
        // - we cannot memmove between float and int arrays, due to different bit pattern
        // - we cannot memmove to a uint8 clamped array from an int8 array, due to negatives rounding to 0
        if (GetTypeId() == source->GetTypeId() ||
            (GetBytesPerElement() == source->GetBytesPerElement()
             && !((VarIs<Uint8ClampedArray>(this) || VarIs<Uint8ClampedVirtualArray>(this)) && (VarIs<Int8Array>(source) || VarIs<Int8VirtualArray>(source)))
             && !VarIs<Float32Array>(this) && !VarIs<Float32Array>(source)
             && !VarIs<Float32VirtualArray>(this) && !VarIs<Float32VirtualArray>(source)
             && !VarIs<Float64Array>(this) && !VarIs<Float64Array>(source)
             && !VarIs<Float64VirtualArray>(this) && !VarIs<Float64VirtualArray>(source)
             && !VarIs<Float16Array>(this) && !VarIs<Float16Array>(source)))
        {
            const size_t offsetInBytes = offset * BYTES_PER_ELEMENT;
            memmove_s(buffer + offsetInBytes,
                      GetByteLength() - offsetInBytes,
                      source->GetByteBuffer(),
                      source->GetByteLength());
        }
        else
        {
            if (source->GetArrayBuffer() != GetArrayBuffer())
            {
                for (uint32 i = 0; i < sourceLength; i++)
                {
                    DirectSetItemNoDetachCheck(offset + i, source->DirectGetItemNoDetachCheck(i));
                }
            }
            else
            {
                // We can have the source and destination coming from the same buffer. element size, start offset, and
                // length for source and dest typed array can be different. Use a separate tmp buffer to copy the elements.
                Js::JavascriptArray* tmpArray = GetScriptContext()->GetLibrary()->CreateArray(sourceLength);
                for (uint32 i = 0; i < sourceLength; i++)
                {
                    tmpArray->SetItem(i, source->DirectGetItem(i), PropertyOperation_None);
                }
                for (uint32 i = 0; i < sourceLength; i++)
                {
                    DirectSetItem(offset + i, tmpArray->DirectGetItem(i));
                }
            }
            if (source->IsDetachedBuffer() || this->IsDetachedBuffer())
            {
                Throw::FatalInternalError();
            }
        }
    }

    uint32 TypedArrayBase::GetSourceLength(RecyclableObject* arraySource, uint32 targetLength, uint32 offset)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 sourceLength = JavascriptConversion::ToUInt32(JavascriptOperators::OP_GetProperty(arraySource, PropertyIds::length, scriptContext), scriptContext);
        uint32 totalLength;
        if (UInt32Math::Add(offset, sourceLength, &totalLength) ||
            (totalLength > targetLength))
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_InvalidTypedArrayLength);
        }
        return sourceLength;
    }

    void TypedArrayBase::SetObjectNoDetachCheck(RecyclableObject* source, uint32 targetLength, uint32 offset)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 sourceLength = GetSourceLength(source, targetLength, offset);
        Assert(!this->IsDetachedBuffer());

        Var itemValue;
        Var undefinedValue = scriptContext->GetLibrary()->GetUndefined();
        for (uint32 i = 0; i < sourceLength; i++)
        {
            if (!source->GetItem(source, i, &itemValue, scriptContext))
            {
                itemValue = undefinedValue;
            }
            DirectSetItemNoDetachCheck(offset + i, itemValue);
        }

        if (this->IsDetachedBuffer())
        {
            // We cannot be detached when we are creating the typed array itself. Terminate if that happens.
            Throw::FatalInternalError();
        }
    }

    // Getting length from the source object can detach the typedarray, and thus set it's length as 0,
    // this is observable because RangeError will be thrown instead of a TypeError further down the line
    void TypedArrayBase::SetObject(RecyclableObject* source, uint32 targetLength, uint32 offset)
    {
        ScriptContext* scriptContext = GetScriptContext();
        uint32 sourceLength = GetSourceLength(source, targetLength, offset);

        Var itemValue;
        Var undefinedValue = scriptContext->GetLibrary()->GetUndefined();
        for (uint32 i = 0; i < sourceLength; i ++)
        {
            if (!source->GetItem(source, i, &itemValue, scriptContext))
            {
                itemValue = undefinedValue;
            }
            DirectSetItem(offset + i, itemValue);
        }
    }

    HRESULT TypedArrayBase::GetBuffer(Var instance, ArrayBuffer** outBuffer, uint32* outOffset, uint32* outLength)
    {
        HRESULT hr = NOERROR;
        if (Js::VarIs<Js::TypedArrayBase>(instance))
        {
            Js::TypedArrayBase* typedArrayBase = Js::VarTo<Js::TypedArrayBase>(instance);
            *outBuffer = typedArrayBase->GetArrayBuffer()->GetAsArrayBuffer();
            *outOffset = typedArrayBase->GetByteOffset();
            *outLength = typedArrayBase->GetByteLength();
        }
        else if (Js::VarIs<Js::ArrayBuffer>(instance))
        {
            Js::ArrayBuffer* buffer = Js::VarTo<Js::ArrayBuffer>(instance);
            *outBuffer = buffer;
            *outOffset = 0;
            *outLength = buffer->GetByteLength();
        }
        else if (Js::VarIs<Js::DataView>(instance))
        {
            Js::DataView* dView = Js::VarTo<Js::DataView>(instance);
            *outBuffer = dView->GetArrayBuffer()->GetAsArrayBuffer();
            *outOffset = dView->GetByteOffset();
            *outLength = dView->GetLength();
        }
        else
        {
            hr = E_INVALIDARG;
        }
        return hr;

    }

    void TypedArrayBase::ClearLengthAndBufferOnDetach()
    {
        AssertMsg(IsDetachedBuffer(), "Array buffer should be detached if we're calling this method");

        this->length = 0;
#if INT32VAR
        this->buffer = (BYTE*)TaggedInt::ToVarUnchecked(0);
#else
        this->buffer = nullptr;
#endif
    }

    Var TypedArrayBase::EntryGetterBuffer(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        TypedArrayBase* typedArray = UnsafeVarTo<TypedArrayBase>(args[0]);
        ArrayBufferBase* arrayBuffer = typedArray->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        return arrayBuffer;
    }

    Var TypedArrayBase::EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        TypedArrayBase* typedArray = UnsafeVarTo<TypedArrayBase>(args[0]);
        ArrayBufferBase* arrayBuffer = typedArray->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        else if (arrayBuffer->IsDetached())
        {
            return TaggedInt::ToVarUnchecked(0);
        }

        return JavascriptNumber::ToVar(typedArray->GetByteLength(), scriptContext);
    }

    Var TypedArrayBase::EntryGetterByteOffset(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        TypedArrayBase* typedArray = UnsafeVarTo<TypedArrayBase>(args[0]);
        ArrayBufferBase* arrayBuffer = typedArray->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        else if (arrayBuffer->IsDetached())
        {
            return TaggedInt::ToVarUnchecked(0);
        }

        return JavascriptNumber::ToVar(typedArray->GetEffectiveByteOffset(), scriptContext);
    }

    Var TypedArrayBase::EntryGetterLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        TypedArrayBase* typedArray = UnsafeVarTo<TypedArrayBase>(args[0]);
        ArrayBufferBase* arrayBuffer = typedArray->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        else if (arrayBuffer->IsDetached())
        {
            return TaggedInt::ToVarUnchecked(0);
        }

        return JavascriptNumber::ToVar(typedArray->GetLength(), scriptContext);
    }

    Var TypedArrayBase::EntryGetterSymbolSpecies(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ARGUMENTS(args, callInfo);

        Assert(args.Info.Count > 0);

        return args[0];
    }

    // ES2017 22.2.3.32 get %TypedArray%.prototype[@@toStringTag]
    Var TypedArrayBase::EntryGetterSymbolToStringTag(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        // 1. Let O be the this value.
        // 2. If Type(O) is not Object, return undefined.
        // 3. If O does not have a[[TypedArrayName]] internal slot, return undefined.
        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            if (scriptContext->GetConfig()->IsES6ToStringTagEnabled())
            {
                return scriptContext->GetLibrary()->GetUndefined();
            }
            else
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
            }
        }

        // 4. Let name be O.[[TypedArrayName]].
        // 5. Assert: name is a String value.
        // 6. Return name.
        Var name;
        switch (JavascriptOperators::GetTypeId(args[0]))
        {
        case TypeIds_Int8Array:
            name = scriptContext->GetPropertyString(PropertyIds::Int8Array);
            break;

        case TypeIds_Uint8Array:
            name = scriptContext->GetPropertyString(PropertyIds::Uint8Array);
            break;

        case TypeIds_Uint8ClampedArray:
            name = scriptContext->GetPropertyString(PropertyIds::Uint8ClampedArray);
            break;

        case TypeIds_Int16Array:
            name = scriptContext->GetPropertyString(PropertyIds::Int16Array);
            break;

        case TypeIds_Uint16Array:
            name = scriptContext->GetPropertyString(PropertyIds::Uint16Array);
            break;

        case TypeIds_Int32Array:
            name = scriptContext->GetPropertyString(PropertyIds::Int32Array);
            break;

        case TypeIds_Uint32Array:
            name = scriptContext->GetPropertyString(PropertyIds::Uint32Array);
            break;

        case TypeIds_Float32Array:
            name = scriptContext->GetPropertyString(PropertyIds::Float32Array);
            break;

        case TypeIds_Float64Array:
            name = scriptContext->GetPropertyString(PropertyIds::Float64Array);
            break;

        case TypeIds_Float16Array:
            name = scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("Float16Array"));
            break;

        case TypeIds_Int64Array:
            name = scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("BigInt64Array"));
            break;

        case TypeIds_Uint64Array:
            name = scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("BigUint64Array"));
            break;

        default:
            name = scriptContext->GetLibrary()->GetUndefinedDisplayString();
            break;
        }

        return name;
    }

    Var TypedArrayBase::EntrySet(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        return CommonSet(args);
    }

    template <typename TypeName, bool clamped, bool virtualAllocated>
    Var TypedArray<TypeName, clamped, virtualAllocated>::EntrySet(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        // This method is only called in pre-ES6 compat modes. In those modes, we need to throw an error
        // if the this argument is not the same type as our TypedArray template instance.
        if (args.Info.Count == 0 || !VarIs<TypedArray>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        return CommonSet(args);
    }

    Var TypedArrayBase::CommonSet(Arguments& args)
    {
        TypedArrayBase* typedArrayBase = VarTo<TypedArrayBase>(args[0]);
        ScriptContext* scriptContext = typedArrayBase->GetScriptContext();
        uint32 offset = 0;
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_TypedArray_NeedSource);
        }

        // set step 5: verify the target is mutable before reading offset or
        // any source element.
        if (IsImmutableBuffer(typedArrayBase->GetArrayBuffer()))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        if (args.Info.Count > 2)
        {
            offset = ArrayBuffer::ToIndex(args[2], JSERR_InvalidTypedArrayLength, scriptContext, ArrayBuffer::MaxArrayBufferLength, false);
        }

        // Out-of-bounds views behave like detached views for set: reject them
        // before reading the source (a proxy source must not observe a get).
        if (typedArrayBase->IsDetachedBuffer() || typedArrayBase->IsOutOfBounds())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray].prototype.set"));
        }

        TypedArrayBase* typedArraySource = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(args[1]);
        if (typedArraySource)
        {
            if (typedArraySource->IsDetachedBuffer() || typedArraySource->IsOutOfBounds())
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray].prototype.set"));
            }
            typedArrayBase->Set(typedArraySource, (uint32)offset);
        }
        else
        {
            RecyclableObject* sourceArray;
#if ENABLE_COPYONACCESS_ARRAY
            JavascriptLibrary::CheckAndConvertCopyOnAccessNativeIntArray<Var>(args[1]);
#endif
            if (!JavascriptConversion::ToObject(args[1], scriptContext, &sourceArray))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_TypedArray_NeedSource);
            }
            typedArrayBase->SetObject(sourceArray, typedArrayBase->GetLength(), (uint32)offset);
        }
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var TypedArrayBase::EntrySubarray(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_subarray);

        if (args.Info.Count == 0 || !VarIs<TypedArrayBase>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        return CommonSubarray(args);
    }

    uint32 TypedArrayBase::GetFromIndex(Var arg, uint32 length, ScriptContext *scriptContext)
    {
        uint32 index = JavascriptArray::GetFromIndex(arg, length, scriptContext);
        Assert(index <= length);
        return index;
    }

    Var TypedArrayBase::CommonSubarray(Arguments& args)
    {
        TypedArrayBase* typedArrayBase = VarTo<TypedArrayBase>(args[0]);
        uint32 length = typedArrayBase->GetLength();
        ScriptContext* scriptContext = typedArrayBase->GetScriptContext();
        int32 begin = 0;
        int end = length;
        if (args.Info.Count > 1)
        {
            begin = TypedArrayBase::GetFromIndex(args[1], length, scriptContext);

            if (args.Info.Count > 2 && !JavascriptOperators::IsUndefined(args[2]))
            {
                end = TypedArrayBase::GetFromIndex(args[2], length, scriptContext);
            }
        }

        if (end < begin)
        {
            end = begin;
        }

        // A subarray of an auto-length view stays auto-length when end is
        // omitted; with an explicit end it captures a fixed length.
        bool lengthTracking = typedArrayBase->IsLengthTracking() &&
            (args.Info.Count <= 2 || JavascriptOperators::IsUndefined(args[2]));

        return typedArrayBase->Subarray(begin, end, lengthTracking);
    }

    template <typename TypeName, bool clamped, bool virtualAllocated>
    Var TypedArray<TypeName, clamped, virtualAllocated>::Subarray(uint32 begin, uint32 end, bool lengthTracking)
    {
        Assert(end >= begin);

        Var newTypedArray;
        ScriptContext* scriptContext = this->GetScriptContext();
        ArrayBufferBase* buffer = this->GetArrayBuffer();
        uint32 srcByteOffset = this->GetByteOffset();
        uint32 beginByteOffset = srcByteOffset + begin * BYTES_PER_ELEMENT;
        uint32 newLength = end - begin;

        JavascriptFunction* defaultConstructor = TypedArrayBase::GetDefaultConstructor(this, scriptContext);
        RecyclableObject* constructor = JavascriptOperators::SpeciesConstructor(this, defaultConstructor, scriptContext);
        AssertOrFailFast(JavascriptOperators::IsConstructor(constructor));

        bool isDefaultConstructor = constructor == defaultConstructor;
        newTypedArray = VarTo<RecyclableObject>(JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
        {
            Js::Var constructorArgs[] = { constructor, buffer, JavascriptNumber::ToVar(beginByteOffset, scriptContext), JavascriptNumber::ToVar(newLength, scriptContext) };
            Js::CallInfo constructorCallInfo(Js::CallFlags_New, lengthTracking ? 3 : 4);
            return TypedArrayBase::TypedArrayCreate(constructor, &Js::Arguments(constructorCallInfo, constructorArgs), newLength, scriptContext);
        }));

        return newTypedArray;
    }

    Var TypedArrayBase::EntryToLocaleString(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.toLocaleString"));
        return JavascriptArray::ToLocaleString(static_cast<RecyclableObject*>(typedArrayBase), scriptContext);
    }

    // %TypedArray%.from as described in ES6.0 (draft 22) Section 22.2.2.1
    Var TypedArrayBase::EntryFrom(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].from"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_from);

        if (args.Info.Count < 1 || !JavascriptOperators::IsConstructor(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedFunction, _u("[TypedArray].from"));
        }

        RecyclableObject* constructor = VarTo<RecyclableObject>(args[0]);
        bool isDefaultConstructor = JavascriptLibrary::IsTypedArrayConstructor(constructor, scriptContext);
        RecyclableObject* items = nullptr;

        if (args.Info.Count < 2 || !JavascriptConversion::ToObject(args[1], scriptContext, &items))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("[TypedArray].from"));
        }

        bool mapping = false;
        RecyclableObject* mapFn = nullptr;
        Var mapFnThisArg = nullptr;

        if (args.Info.Count >= 3)
        {
            if (!JavascriptConversion::IsCallable(args[2]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("[TypedArray].from"));
            }

            mapFn = VarTo<RecyclableObject>(args[2]);

            if (args.Info.Count >= 4)
            {
                mapFnThisArg = args[3];
            }
            else
            {
                mapFnThisArg = library->GetUndefined();
            }

            mapping = true;
        }

        Var newObj;
        RecyclableObject* iterator = JavascriptOperators::GetIterator(items, scriptContext, true /* optional */);

        if (iterator != nullptr)
        {
            DECLARE_TEMP_GUEST_ALLOCATOR(tempAlloc);

            ACQUIRE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext, _u("Runtime"));
            {
                // Create a temporary list to hold the items returned from the iterator.
                // We will then iterate over this list and append those items into the object we will return.
                // We have to collect the items into this temporary list because we need to call the
                // new object constructor with a length of items and we don't know what length will be
                // until we iterate across all the items.
                // Consider: Could be possible to fast-path this in order to avoid creating the temporary list
                //       for types we know such as TypedArray. We know the length of a TypedArray but we still
                //       have to be careful in case there is a proxy which could return anything from [[Get]]
                //       or the built-in @@iterator has been replaced.
                JsUtil::List<Var, ArenaAllocator>* tempList = IteratorToList(iterator, scriptContext, tempAlloc);

                uint32 len = tempList->Count();
                newObj = JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
                {
                    Js::Var constructorArgs[] = { constructor, JavascriptNumber::ToVar(len, scriptContext) };
                    Js::CallInfo constructorCallInfo(Js::CallFlags_New, _countof(constructorArgs));
                    return TypedArrayCreate(constructor, &Js::Arguments(constructorCallInfo, constructorArgs), len, scriptContext, true);
                });

                TypedArrayBase* newTypedArrayBase = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(newObj);
                JavascriptArray* newArr = nullptr;

                if (!newTypedArrayBase)
                {
                    newArr = JavascriptArray::TryVarToNonES5Array(newObj);
                }

                for (uint32 k = 0; k < len; k++)
                {
                    Var kValue = tempList->Item(k);

                    if (mapping)
                    {
                        Assert(mapFn != nullptr);
                        Assert(mapFnThisArg != nullptr);

                        Var kVar = JavascriptNumber::ToVar(k, scriptContext);
                        kValue = scriptContext->GetThreadContext()->ExecuteImplicitCall(mapFn, Js::ImplicitCall_Accessor, [=]()->Js::Var
                        {
                            return CALL_FUNCTION(scriptContext->GetThreadContext(), mapFn, CallInfo(CallFlags_Value, 3), mapFnThisArg, kValue, kVar);
                        });
                    }

                    // We're likely to have constructed a new TypedArray, but the constructor could return any object
                    if (newTypedArrayBase)
                    {
                        newTypedArrayBase->DirectSetItem(k, kValue);
                    }
                    else if (newArr)
                    {
                        newArr->SetItem(k, kValue, Js::PropertyOperation_ThrowIfNotExtensible);
                    }
                    else
                    {
                        JavascriptOperators::OP_SetElementI_UInt32(newObj, k, kValue, scriptContext, Js::PropertyOperation_ThrowIfNotExtensible);
                    }
                }
            }
            RELEASE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext);
        }
        else
        {
            Var lenValue = JavascriptOperators::OP_GetLength(items, scriptContext);
            uint32 len = JavascriptConversion::ToUInt32(lenValue, scriptContext);

            TypedArrayBase* itemsTypedArrayBase = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(items);
            JavascriptArray* itemsArray = nullptr;

            if (!itemsTypedArrayBase)
            {
                itemsArray = JavascriptArray::TryVarToNonES5Array(items);
            }

            newObj = JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
            {
                Js::Var constructorArgs[] = { constructor, JavascriptNumber::ToVar(len, scriptContext) };
                Js::CallInfo constructorCallInfo(Js::CallFlags_New, _countof(constructorArgs));
                return TypedArrayBase::TypedArrayCreate(constructor, &Js::Arguments(constructorCallInfo, constructorArgs), len, scriptContext, true);
            });

            TypedArrayBase* newTypedArrayBase = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(newObj);
            JavascriptArray* newArr = nullptr;

            if (!newTypedArrayBase)
            {
                newArr = JavascriptArray::TryVarToNonES5Array(newObj);
            }

            for (uint32 k = 0; k < len; k++)
            {
                Var kValue;

                // The items source could be anything, but we already know if it's a TypedArray or Array
                if (itemsTypedArrayBase)
                {
                    kValue = itemsTypedArrayBase->DirectGetItem(k);
                }
                else if (itemsArray)
                {
                    kValue = itemsArray->DirectGetItem(k);
                }
                else
                {
                    kValue = JavascriptOperators::OP_GetElementI_UInt32(items, k, scriptContext);
                }

                if (mapping)
                {
                    Assert(mapFn != nullptr);
                    Assert(mapFnThisArg != nullptr);

                    Var kVar = JavascriptNumber::ToVar(k, scriptContext);
                    kValue = scriptContext->GetThreadContext()->ExecuteImplicitCall(mapFn, Js::ImplicitCall_Accessor, [=]()->Js::Var
                    {
                        return CALL_FUNCTION(scriptContext->GetThreadContext(), mapFn, CallInfo(CallFlags_Value, 3), mapFnThisArg, kValue, kVar);
                    });
                }

                // If constructor built a TypedArray (likely) or Array (maybe likely) we can do a more direct set operation
                if (newTypedArrayBase)
                {
                    newTypedArrayBase->DirectSetItem(k, kValue);
                }
                else if (newArr)
                {
                    newArr->SetItem(k, kValue, Js::PropertyOperation_ThrowIfNotExtensible);
                }
                else
                {
                    JavascriptOperators::OP_SetElementI_UInt32(newObj, k, kValue, scriptContext, Js::PropertyOperation_ThrowIfNotExtensible);
                }
            }
        }

        return newObj;
    }

    Var TypedArrayBase::EntryOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_of);

        if (args.Info.Count < 1)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedFunction, _u("[TypedArray].of"));
        }

        return JavascriptArray::OfHelper(true, args, scriptContext);
    }

    Var TypedArrayBase::EntryAt(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_at);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.at"));

        return JavascriptArray::AtHelper<uint32>(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryCopyWithin(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_copyWithin);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.copyWithin"), true);
        uint32 length = typedArrayBase->GetLength();

        return JavascriptArray::CopyWithinHelper(nullptr, typedArrayBase, typedArrayBase, length, args, scriptContext);
    }

    Var TypedArrayBase::GetKeysEntriesValuesHelper(Arguments& args, ScriptContext *scriptContext, LPCWSTR apiName, JavascriptArrayIteratorKind kind)
    {
        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, apiName);
#ifdef ENABLE_JS_BUILTINS
        JavascriptLibrary * library = scriptContext->GetLibrary();
        if (scriptContext->IsJsBuiltInEnabled())
        {
            JavascriptString* methodName = JavascriptString::NewWithSz(_u("CreateArrayIterator"), scriptContext);
            PropertyIds functionIdentifier = JavascriptOperators::GetPropertyId(methodName, scriptContext);
            Var scriptFunction = JavascriptOperators::OP_GetProperty(library->GetChakraLib(), functionIdentifier, scriptContext);

            Assert(!JavascriptOperators::IsUndefinedOrNull(scriptFunction));
            Assert(JavascriptConversion::IsCallable(scriptFunction));

            RecyclableObject* function = VarTo<RecyclableObject>(scriptFunction);

            Var chakraLibObj = JavascriptOperators::OP_GetProperty(library->GetGlobalObject(), PropertyIds::__chakraLibrary, scriptContext);
            Var argsIt[] = { chakraLibObj, args[0], TaggedInt::ToVarUnchecked((int)kind) };
            CallInfo callInfo(CallFlags_Value, 3);
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                return JavascriptFunction::CallFunction<true>(function, function->GetEntryPoint(), Js::Arguments(callInfo, argsIt));
            }
            END_SAFE_REENTRANT_CALL
        }
        else
#endif
        return scriptContext->GetLibrary()->CreateArrayIterator(typedArrayBase, kind);
    }

    Var TypedArrayBase::EntryEntries(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_entries);

        return GetKeysEntriesValuesHelper(args, scriptContext, _u("[TypedArray].prototype.entries"), JavascriptArrayIteratorKind::KeyAndValue);
    }

    Var TypedArrayBase::EntryEvery(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.every"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_every);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.every"));
        return JavascriptArray::EveryHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryFill(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_fill);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.fill"), true);
        return JavascriptArray::FillHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    // %TypedArray%.prototype.filter as described in ES6.0 (draft 22) Section 22.2.3.9
    Var TypedArrayBase::EntryFilter(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.filter"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_filter);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.filter"));
        uint32 length = typedArrayBase->GetLength();

        if (args.Info.Count < 2 || !JavascriptConversion::IsCallable(args[1]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("[TypedArray].prototype.filter"));
        }

        RecyclableObject* callBackFn = VarTo<RecyclableObject>(args[1]);
        Var thisArg = nullptr;

        if (args.Info.Count > 2)
        {
            thisArg = args[2];
        }
        else
        {
            thisArg = scriptContext->GetLibrary()->GetUndefined();
        }

        Var element = nullptr;
        Var selected = nullptr;
        RecyclableObject* newObj = nullptr;

        DECLARE_TEMP_GUEST_ALLOCATOR(tempAlloc);

        ACQUIRE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext, _u("Runtime"));
        {
            // Create a temporary list to hold the items selected by the callback function.
            // We will then iterate over this list and append those items into the object we will return.
            // We have to collect the items into this temporary list because we need to call the
            // new object constructor with a length of items and we don't know what length will be
            // until we know how many items we will collect.
            JsUtil::List<Var, ArenaAllocator>* tempList = JsUtil::List<Var, ArenaAllocator>::New(tempAlloc);

            for (uint32 k = 0; k < length; k++)
            {
                // We know that the TypedArray.HasItem will be true because k < length and we can skip the check in the TypedArray version of filter.
                element = typedArrayBase->DirectGetItem(k);

                BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
                {
                    selected = CALL_FUNCTION(scriptContext->GetThreadContext(),
                                    callBackFn, CallInfo(CallFlags_Value, 4), thisArg,
                                    element,
                                    JavascriptNumber::ToVar(k, scriptContext),
                                    typedArrayBase);
                }
                END_SAFE_REENTRANT_CALL

                if (JavascriptConversion::ToBoolean(selected, scriptContext))
                {
                    tempList->Add(element);
                }
            }

            uint32 captured = tempList->Count();

            JavascriptFunction* defaultConstructor = TypedArrayBase::GetDefaultConstructor(args[0], scriptContext);
            RecyclableObject* constructor = JavascriptOperators::SpeciesConstructor(typedArrayBase, defaultConstructor, scriptContext);
            AssertOrFailFast(JavascriptOperators::IsConstructor(constructor));

            bool isDefaultConstructor = constructor == defaultConstructor;
            newObj = VarTo<RecyclableObject>(JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
            {
                Js::Var constructorArgs[] = { constructor, JavascriptNumber::ToVar(captured, scriptContext) };
                Js::CallInfo constructorCallInfo(Js::CallFlags_New, _countof(constructorArgs));
                return TypedArrayBase::TypedArrayCreate(constructor, &Js::Arguments(constructorCallInfo, constructorArgs), captured, scriptContext, true);
            }));

            TypedArrayBase* newArr = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(newObj);
            if (newArr)
            {
                // We are much more likely to have a TypedArray here than anything else

                for (uint32 i = 0; i < captured; i++)
                {
                    newArr->DirectSetItem(i, tempList->Item(i));
                }
            }
            else
            {
                for (uint32 i = 0; i < captured; i++)
                {
                    JavascriptOperators::OP_SetElementI_UInt32(newObj, i, tempList->Item(i), scriptContext, PropertyOperation_ThrowIfNotExtensible);
                }
            }
        }
        RELEASE_TEMP_GUEST_ALLOCATOR(tempAlloc, scriptContext);

        return newObj;
    }

    Var TypedArrayBase::EntryFind(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.find"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_find);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.find"));

        return JavascriptArray::FindHelper<false, false>(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryFindIndex(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.findIndex"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_findIndex);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.findIndex"));

        return JavascriptArray::FindHelper<true, false>(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryFindLast(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.findLast"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_find);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.findLast"));

        return JavascriptArray::FindHelper<false, true>(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryFindLastIndex(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.findLastIndex"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_findIndex);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.findLastIndex"));

        return JavascriptArray::FindHelper<true, true>(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    // %TypedArray%.prototype.forEach as described in ES6.0 (draft 22) Section 22.2.3.12
    Var TypedArrayBase::EntryForEach(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.forEach"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_forEach);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.forEach"));
        uint32 length = typedArrayBase->GetLength();

        if (args.Info.Count < 2 || !JavascriptConversion::IsCallable(args[1]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("[TypedArray].prototype.forEach"));
        }

        RecyclableObject* callBackFn = VarTo<RecyclableObject>(args[1]);
        Var thisArg;

        if (args.Info.Count > 2)
        {
            thisArg = args[2];
        }
        else
        {
            thisArg = scriptContext->GetLibrary()->GetUndefined();
        }

        for (uint32 k = 0; k < length; k++)
        {
            // No need for HasItem, as we have already established that this API can be called only on the TypedArray object. So Proxy scenario cannot happen.

            Var element = typedArrayBase->DirectGetItem(k);

            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                CALL_FUNCTION(scriptContext->GetThreadContext(),
                    callBackFn, CallInfo(CallFlags_Value, 4),
                    thisArg,
                    element,
                    JavascriptNumber::ToVar(k, scriptContext),
                    typedArrayBase);
            }
            END_SAFE_REENTRANT_CALL
        }

        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var TypedArrayBase::EntryIndexOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_indexOf);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.indexOf"));
        uint32 length = typedArrayBase->GetLength();

        Var search = nullptr;
        uint32 fromIndex;
        if (!JavascriptArray::GetParamForIndexOf(length, args, search, fromIndex, scriptContext))
        {
            return TaggedInt::ToVarUnchecked(-1);
        }

        return JavascriptArray::TemplatedIndexOfHelper<false>(typedArrayBase, search, fromIndex, length, scriptContext);
    }

    Var TypedArrayBase::EntryIncludes(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_includes);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.includes"));
        uint32 length = typedArrayBase->GetLength();

        Var search = nullptr;
        uint32 fromIndex;
        if (!JavascriptArray::GetParamForIndexOf(length, args, search, fromIndex, scriptContext))
        {
            return scriptContext->GetLibrary()->GetFalse();
        }

        return JavascriptArray::TemplatedIndexOfHelper<true>(typedArrayBase, search, fromIndex, length, scriptContext);
    }


    // Join walks the captured length but reads only indices that are still
    // valid; OOB and detached positions stringify as the empty string.
    static JavascriptString* GetTypedArrayJoinElement(TypedArrayBase* typedArrayBase, uint32 index, ScriptContext* scriptContext)
    {
        if (typedArrayBase->IsDetachedBuffer() || index >= typedArrayBase->GetLength())
        {
            return scriptContext->GetLibrary()->GetEmptyString();
        }
        return JavascriptConversion::ToString(typedArrayBase->DirectGetItem(index), scriptContext);
    }

    Var TypedArrayBase::EntryJoin(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_join);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.join"));
        uint32 length = typedArrayBase->GetLength();

        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptString* separator = nullptr;

        if (args.Info.Count > 1 && !JavascriptOperators::IsUndefined(args[1]))
        {
            separator = JavascriptConversion::ToString(args[1], scriptContext);
        }
        else
        {
            separator = library->GetCommaDisplayString();
        }

        if (length == 0)
        {
            return library->GetEmptyString();
        }
        else if (length == 1)
        {
            return GetTypedArrayJoinElement(typedArrayBase, 0, scriptContext);
        }

        bool hasSeparator = (separator->GetLength() != 0);

        charcount_t estimatedAppendSize = min(
            static_cast<charcount_t>((64 << 20) / sizeof(void *)), // 64 MB worth of pointers
            static_cast<charcount_t>(length + (hasSeparator ? length - 1 : 0)));

        CompoundString* const cs = CompoundString::NewWithPointerCapacity(estimatedAppendSize, library);

        Assert(length >= 2);

        cs->Append(GetTypedArrayJoinElement(typedArrayBase, 0, scriptContext));

        for (uint32 i = 1; i < length; i++)
        {
            if (hasSeparator)
            {
                cs->Append(separator);
            }

            cs->Append(GetTypedArrayJoinElement(typedArrayBase, i, scriptContext));
        }

        return cs;
    }

    Var TypedArrayBase::EntryKeys(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_keys);

        return GetKeysEntriesValuesHelper(args, scriptContext, _u("[TypedArray].prototype.keys"), JavascriptArrayIteratorKind::Key);
    }

    Var TypedArrayBase::EntryLastIndexOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_lastIndexOf);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.lastIndexOf"));
        uint32 length = typedArrayBase->GetLength();

        Var search = nullptr;
        int64 fromIndex;
        if (!JavascriptArray::GetParamForLastIndexOf(length, args, search, fromIndex, scriptContext))
        {
            return TaggedInt::ToVarUnchecked(-1);
        }

        return JavascriptArray::LastIndexOfHelper(typedArrayBase, search, fromIndex, scriptContext);
    }

    Var TypedArrayBase::EntryMap(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.map"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_map);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.map"));

        return JavascriptArray::MapHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryReduce(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.reduce"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_reduce);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.reduce"));
        return JavascriptArray::ReduceHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryReduceRight(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.reduceRight"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_reduceRight);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.reduceRight"));
        return JavascriptArray::ReduceRightHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntryReverse(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_reverse);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.reverse"), true);
        return JavascriptArray::ReverseHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), scriptContext);
    }

    Var TypedArrayBase::EntrySlice(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.slice"));
        return JavascriptArray::SliceHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    Var TypedArrayBase::EntrySome(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.some"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_some);

        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.some"));

        return JavascriptArray::SomeHelper(nullptr, typedArrayBase, typedArrayBase, typedArrayBase->GetLength(), args, scriptContext);
    }

    // Comparison method used in TypedArray.prototype.sort
    template<typename T> bool TypedArrayCompareElementsHelper(JavascriptArray::CompareVarsInfo* cvInfo, const void* elem1, const void* elem2)
    {
        const T* element1 = static_cast<const T*>(elem1);
        const T* element2 = static_cast<const T*>(elem2);

        Assert(element1 != nullptr);
        Assert(element2 != nullptr);
        Assert(cvInfo != nullptr);

        const T x = *element1;
        const T y = *element2;

        // ECMA2023 spec requires that NaN values are sorted to the end
        if (NumberUtilities::IsNan((double)x))
        {
            return false;
        }
        else if (NumberUtilities::IsNan((double)y))
        {
            return true;
        }

        if (cvInfo->compFn != nullptr)
        {
            RecyclableObject* compFn = cvInfo->compFn;
            ScriptContext* scriptContext = compFn->GetScriptContext();
            Var undefined = scriptContext->GetLibrary()->GetUndefined();
            Var retVal = nullptr;
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                retVal = CALL_FUNCTION(scriptContext->GetThreadContext(),
                            compFn, CallInfo(CallFlags_Value, 3),
                            undefined,
                            JavascriptNumber::ToVarNoCheck((double)x, scriptContext),
                            JavascriptNumber::ToVarNoCheck((double)y, scriptContext));
            }
            END_SAFE_REENTRANT_CALL

            if (TaggedInt::Is(retVal))
            {
                return TaggedInt::ToInt32(retVal) < 0;
            }

            if (JavascriptNumber::Is_NoTaggedIntCheck(retVal))
            {
                return JavascriptNumber::GetValue(retVal) < 0;
            }

            return JavascriptConversion::ToNumber_Full(retVal, scriptContext) < 0;
        }
        else // simple comparison when no user method provided
        {
            // `<` treats -0 and +0 as equal. The spec's default comparator
            // orders -0 first. Shared with TypedArray.prototype.sort; see doc/c2-s10.md.
            return x < y;
        }
    }

    template<>
    bool TypedArrayCompareElementsHelper<int64>(JavascriptArray::CompareVarsInfo* cvInfo, const void* elem1, const void* elem2)
    {
        const int64 x = *static_cast<const int64*>(elem1);
        const int64 y = *static_cast<const int64*>(elem2);
        if (cvInfo->compFn == nullptr)
        {
            return x < y;
        }

        ScriptContext* scriptContext = cvInfo->compFn->GetScriptContext();
        RecyclableObject* compFn = cvInfo->compFn;
        Var retVal = nullptr;
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            retVal = CALL_FUNCTION(scriptContext->GetThreadContext(),
                compFn, CallInfo(CallFlags_Value, 3),
                scriptContext->GetLibrary()->GetUndefined(),
                JavascriptBigInt::CreateFromInt64(x, scriptContext),
                JavascriptBigInt::CreateFromInt64(y, scriptContext));
        }
        END_SAFE_REENTRANT_CALL

        if (TaggedInt::Is(retVal))
        {
            return TaggedInt::ToInt32(retVal) < 0;
        }
        if (JavascriptNumber::Is_NoTaggedIntCheck(retVal))
        {
            return JavascriptNumber::GetValue(retVal) < 0;
        }
        return JavascriptConversion::ToNumber_Full(retVal, scriptContext) < 0;
    }

    template<>
    bool TypedArrayCompareElementsHelper<uint64>(JavascriptArray::CompareVarsInfo* cvInfo, const void* elem1, const void* elem2)
    {
        const uint64 x = *static_cast<const uint64*>(elem1);
        const uint64 y = *static_cast<const uint64*>(elem2);
        if (cvInfo->compFn == nullptr)
        {
            return x < y;
        }

        ScriptContext* scriptContext = cvInfo->compFn->GetScriptContext();
        RecyclableObject* compFn = cvInfo->compFn;
        Var retVal = nullptr;
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            retVal = CALL_FUNCTION(scriptContext->GetThreadContext(),
                compFn, CallInfo(CallFlags_Value, 3),
                scriptContext->GetLibrary()->GetUndefined(),
                JavascriptBigInt::CreateFromUInt64(x, scriptContext),
                JavascriptBigInt::CreateFromUInt64(y, scriptContext));
        }
        END_SAFE_REENTRANT_CALL

        if (TaggedInt::Is(retVal))
        {
            return TaggedInt::ToInt32(retVal) < 0;
        }
        if (JavascriptNumber::Is_NoTaggedIntCheck(retVal))
        {
            return JavascriptNumber::GetValue(retVal) < 0;
        }
        return JavascriptConversion::ToNumber_Full(retVal, scriptContext) < 0;
    }

    // Float16Array stores raw binary16 patterns, so the generic helper
    // (which casts elements to double) does not apply. Widen both sides
    // exactly, then reuse the Number comparison semantics: NaN sorts to
    // the end and a user comparefn receives the widened Numbers.
    template<>
    bool TypedArrayCompareElementsHelper<Float16Bits>(JavascriptArray::CompareVarsInfo* cvInfo, const void* elem1, const void* elem2)
    {
        const double x = Float16::ToDouble(static_cast<const Float16Bits*>(elem1)->bits);
        const double y = Float16::ToDouble(static_cast<const Float16Bits*>(elem2)->bits);

        if (NumberUtilities::IsNan(x))
        {
            return false;
        }
        else if (NumberUtilities::IsNan(y))
        {
            return true;
        }

        if (cvInfo->compFn != nullptr)
        {
            RecyclableObject* compFn = cvInfo->compFn;
            ScriptContext* scriptContext = compFn->GetScriptContext();
            Var undefined = scriptContext->GetLibrary()->GetUndefined();
            Var retVal = nullptr;
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                retVal = CALL_FUNCTION(scriptContext->GetThreadContext(),
                            compFn, CallInfo(CallFlags_Value, 3),
                            undefined,
                            JavascriptNumber::ToVarNoCheck(x, scriptContext),
                            JavascriptNumber::ToVarNoCheck(y, scriptContext));
            }
            END_SAFE_REENTRANT_CALL

            if (TaggedInt::Is(retVal))
            {
                return TaggedInt::ToInt32(retVal) < 0;
            }

            if (JavascriptNumber::Is_NoTaggedIntCheck(retVal))
            {
                return JavascriptNumber::GetValue(retVal) < 0;
            }

            return JavascriptConversion::ToNumber_Full(retVal, scriptContext) < 0;
        }
        else // simple comparison when no user method is provided
        {
            // `<` treats -0 and +0 as equal, matching the other float kinds.
            return x < y;
        }
    }

    // TypedArray.prototype.sort entry point
    // Implements #sec-%typedarray%.prototype.sort from ECMA 2023 spec
    // 
    Var TypedArrayBase::EntrySort(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.sort"));

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_sort);

        RecyclableObject* compareFn = nullptr;
        // Spec requires us to throw if comparison function is neither undefined nor callable
        if (args.Info.Count > 1 && !JavascriptOperators::IsUndefined(args[1]))
        {
            if (!JavascriptConversion::IsCallable(args[1]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("[TypedArray].prototype.sort"));
            }

            compareFn = UnsafeVarTo<RecyclableObject>(args[1]);
        }

        // Throw if not a valid typed array
        // This step is per spec and allows us to optimise significantly below
        TypedArrayBase* typedArrayBase = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.sort"), true);
        uint32 length = typedArrayBase->GetLength();

        // Early return if length is 0 or 1
        if (length < 2)
        {
            return typedArrayBase;
        }

        BEGIN_TEMP_ALLOCATOR(tempAlloc, scriptContext, _u("Runtime"))
        {
            byte* buffer = typedArrayBase->GetByteBuffer();
            
            // Spec mandates copy before sort
            // But it is only detectable if a compare function was provided
            if (compareFn != nullptr)
            {
                uint32 elementSize = typedArrayBase->GetBytesPerElement();
                uint32 byteLength = elementSize * length;
                byte* list = AnewArray(tempAlloc, byte, byteLength);
                memcpy(list, buffer, byteLength);
                // Sort the copied data
                typedArrayBase->SortHelper(list, length, compareFn, scriptContext, tempAlloc);

                // A comparison callback may have detached the buffer (throw)
                // or shrunk it. Write only the indices that are still valid;
                // an OOB fixed view leaves its retained bytes unchanged.
                if (typedArrayBase->IsDetachedBuffer())
                {
                    JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray].prototype.sort"));
                }

                uint32 currentLength = typedArrayBase->GetLength();
                if (currentLength > 0)
                {
                    uint32 copyLength = min(length, currentLength);
                    memcpy(buffer, list, copyLength * elementSize);
                }
            }
            else
            {
                // perform an in-place sort when no comparison method is provided
                typedArrayBase->SortHelper(buffer, length, nullptr, scriptContext, tempAlloc);
            }
        }

        return typedArrayBase;
    }

    // Same-type copy. Does not call the constructor and does not read @@species.
    // Virtual typed arrays share the element TypeId; Create builds the intrinsic instance.
    TypedArrayBase* TypedArrayBase::CreateSameType(TypedArrayBase* exemplar, uint32 length)
    {
        ScriptContext* scriptContext = exemplar->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        uint32 byteLength = 0;
        if (UInt32Math::Mul(length, exemplar->GetBytesPerElement(), &byteLength) ||
            byteLength > ArrayBuffer::MaxArrayBufferLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayLength);
        }

        ArrayBuffer* arrayBuffer = library->CreateArrayBuffer(byteLength);
        Var created = nullptr;
        switch (exemplar->GetTypeId())
        {
        case TypeIds_Int8Array:
            created = Int8Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Uint8Array:
            created = Uint8Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Uint8ClampedArray:
            created = Uint8ClampedArray::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Int16Array:
            created = Int16Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Uint16Array:
            created = Uint16Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Int32Array:
            created = Int32Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Uint32Array:
            created = Uint32Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Float32Array:
            created = Float32Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Float64Array:
            created = Float64Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Float16Array:
            created = Float16Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Int64Array:
            created = Int64Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_Uint64Array:
            created = Uint64Array::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_CharArray:
            created = CharArray::Create(arrayBuffer, 0, length, library);
            break;
        case TypeIds_BoolArray:
            created = BoolArray::Create(arrayBuffer, 0, length, library);
            break;
        default:
            AssertMsg(false, "CreateSameType: unknown typed array TypeId");
            Js::Throw::FatalInternalError();
        }
        return static_cast<TypedArrayBase*>(created);
    }

    static double TypedToIntegerOrInfinity(Var value, ScriptContext* scriptContext)
    {
        JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
        double integer = 0;
        JS_REENTRANT(jsReentLock, integer = JavascriptConversion::ToInteger(value, scriptContext));
        return integer;
    }

    static double TypedToNumber(Var value, ScriptContext* scriptContext)
    {
        JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
        double number = 0;
        JS_REENTRANT(jsReentLock, number = JavascriptConversion::ToNumber(value, scriptContext));
        return number;
    }

    static Var TypedToBigInt(Var value, ScriptContext* scriptContext)
    {
        JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
        Var bigint = nullptr;
        JS_REENTRANT(jsReentLock, bigint = JavascriptConversion::ToBigInt(value, scriptContext));
        return bigint;
    }

    // %TypedArray%.prototype.toReversed. Length 0 is a new empty array of the same type.
    Var TypedArrayBase::EntryToReversed(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.toReversed"));

        Assert(!(callInfo.Flags & CallFlags_New));

        TypedArrayBase* source = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.toReversed"));
        uint32 length = source->GetLength();
        TypedArrayBase* result = CreateSameType(source, length);
        for (uint32 k = 0; k < length; ++k)
        {
            // DirectGetItem throws if the source detaches. Do not cache the byte buffer.
            Var value = source->DirectGetItem(length - k - 1);
            result->DirectSetItem(k, value);
        }
        return result;
    }

    // %TypedArray%.prototype.toSorted. comparefn is checked before ValidateTypedArray.
    // The new buffer is sorted in place. The source is not written back, so a detach
    // during comparefn after the copy does not throw.
    Var TypedArrayBase::EntryToSorted(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.toSorted"));

        Assert(!(callInfo.Flags & CallFlags_New));

        RecyclableObject* compareFn = nullptr;
        if (args.Info.Count > 1 && !JavascriptOperators::IsUndefined(args[1]))
        {
            if (!JavascriptConversion::IsCallable(args[1]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("[TypedArray].prototype.toSorted"));
            }
            compareFn = UnsafeVarTo<RecyclableObject>(args[1]);
        }

        TypedArrayBase* source = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.toSorted"));
        uint32 length = source->GetLength();
        TypedArrayBase* result = CreateSameType(source, length);

        if (length > 0)
        {
            // CreateArrayBuffer can run finalizers. Re-check before touching bytes.
            if (source->IsDetachedBuffer())
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("[TypedArray].prototype.toSorted"));
            }
            uint32 byteLength = 0;
            if (UInt32Math::Mul(length, source->GetBytesPerElement(), &byteLength))
            {
                Js::Throw::FatalInternalError();
            }
            memcpy(result->GetByteBuffer(), source->GetByteBuffer(), byteLength);
        }

        if (length >= 2)
        {
            // SortHelper's mergesort allocates on this arena. Release it if comparefn throws.
            // EntrySort's BEGIN_TEMP_ALLOCATOR has no END; this guard releases both ways.
            struct SortScratch
            {
                ScriptContext* scriptContext;
                TempArenaAllocatorObject* temp;
                ArenaAllocator* alloc;
                SortScratch(ScriptContext* sc)
                    : scriptContext(sc)
                    , temp(sc->GetTemporaryAllocator(_u("toSorted")))
                    , alloc(temp->GetAllocator())
                {
                }
                ~SortScratch()
                {
                    scriptContext->ReleaseTemporaryAllocator(temp);
                }
                SortScratch(const SortScratch&) = delete;
                SortScratch& operator=(const SortScratch&) = delete;
            };
            SortScratch scratch(scriptContext);
            result->SortHelper(result->GetByteBuffer(), length, compareFn, scriptContext, scratch.alloc);
        }
        return result;
    }

    // %TypedArray%.prototype.with. Order: ToInteger, negative index relative
    // to the captured length, ToNumber, then IsValidIntegerIndex against the
    // live length. ToNumber always runs; its throw wins. The copy is later.
    Var TypedArrayBase::EntryWith(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("[TypedArray].prototype.with"));

        Assert(!(callInfo.Flags & CallFlags_New));

        TypedArrayBase* source = ValidateTypedArray(args, scriptContext, _u("[TypedArray].prototype.with"));
        uint32 length = source->GetLength();
        Var indexValue = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        double relative = TypedToIntegerOrInfinity(indexValue, scriptContext);
        double actualIndex = relative >= 0
            ? relative
            : static_cast<double>(length) + relative;

        Var value = args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined();
        Var convertedValue = nullptr;
        if (source->IsBigIntElementType())
        {
            // ES2023 %TypedArray%.prototype.with step 7: BigInt content uses
            // ToBigInt, not ToNumber. The conversion runs before the live
            // index check and before the copy.
            convertedValue = TypedToBigInt(value, scriptContext);
        }
        else
        {
            double numeric = TypedToNumber(value, scriptContext);
            convertedValue = JavascriptNumber::ToVarNoCheck(numeric, scriptContext);
        }

        // The value conversion can grow or shrink the buffer. The positive
        // index is not clamped to the captured length; validity is checked
        // against the current element length.
        uint32 currentLength = source->GetLength();
        if (source->IsDetachedBuffer() || actualIndex < 0 || actualIndex >= static_cast<double>(currentLength))
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_InvalidTypedArrayIndex);
        }
        uint32 actual = static_cast<uint32>(actualIndex);
        TypedArrayBase* result = CreateSameType(source, length);
        for (uint32 k = 0; k < length; ++k)
        {
            if (k == actual)
            {
                result->DirectSetItem(k, convertedValue);
            }
            else
            {
                result->DirectSetItem(k, source->DirectGetItem(k));
            }
        }
        return result;
    }

    Var TypedArrayBase::EntryValues(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));
        CHAKRATEL_LANGSTATS_INC_BUILTINCOUNT(TypedArray_Prototype_values);

        return GetKeysEntriesValuesHelper(args, scriptContext, _u("[TypedArray].prototype.values"), JavascriptArrayIteratorKind::Value);
    }

    BOOL TypedArrayBase::GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        ENTER_PINNED_SCOPE(JavascriptString, toStringResult);
        toStringResult = JavascriptObject::ToStringInternal(this, requestContext);
        stringBuilder->Append(toStringResult->GetString(), toStringResult->GetLength());
        LEAVE_PINNED_SCOPE();
        return TRUE;
    }

    BOOL TypedArrayBase::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        switch(GetTypeId())
        {
        case TypeIds_Int8Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Int8Array)"));
            break;

        case TypeIds_Uint8Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Uint8Array)"));
            break;

        case TypeIds_Uint8ClampedArray:
            stringBuilder->AppendCppLiteral(_u("Object, (Uint8ClampedArray)"));
            break;

        case TypeIds_Int16Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Int16Array)"));
            break;

        case TypeIds_Uint16Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Uint16Array)"));
            break;

        case TypeIds_Int32Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Int32Array)"));
            break;

        case TypeIds_Uint32Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Uint32Array)"));
            break;

        case TypeIds_Float32Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Float32Array)"));
            break;

        case TypeIds_Float64Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Float64Array)"));
            break;

        case TypeIds_Float16Array:
            stringBuilder->AppendCppLiteral(_u("Object, (Float16Array)"));
            break;

        case TypeIds_Int64Array:
            stringBuilder->AppendCppLiteral(_u("Object, (BigInt64Array)"));
            break;

        case TypeIds_Uint64Array:
            stringBuilder->AppendCppLiteral(_u("Object, (BigUint64Array)"));
            break;

        default:
            Assert(false);
            stringBuilder->AppendCppLiteral(_u("Object"));
            break;
        }

        return TRUE;
    }

    bool TypedArrayBase::TryGetLengthForOptimizedTypedArray(const Var var, uint32 *const lengthRef, TypeId *const typeIdRef)
    {
        Assert(var);
        Assert(lengthRef);
        Assert(typeIdRef);

        if(!VarIs<RecyclableObject>(var))
        {
            return false;
        }

        const TypeId typeId = VarTo<RecyclableObject>(var)->GetTypeId();
        switch(typeId)
        {
            case TypeIds_Int8Array:
            case TypeIds_Uint8Array:
            case TypeIds_Uint8ClampedArray:
            case TypeIds_Int16Array:
            case TypeIds_Uint16Array:
            case TypeIds_Int32Array:
            case TypeIds_Uint32Array:
            case TypeIds_Float32Array:
            case TypeIds_Float64Array:
                Assert(ValueType::FromTypeId(typeId,false).IsOptimizedTypedArray());
                *lengthRef = VarTo<TypedArrayBase>(var)->GetLength();
                *typeIdRef = typeId;
                return true;
        }

        Assert(!ValueType::FromTypeId(typeId,false).IsOptimizedTypedArray());
        return false;
    }

    _Use_decl_annotations_ BOOL TypedArrayBase::ValidateIndexAndDirectSetItem(Js::Var index, Js::Var value, bool * isNumericIndex)
    {
        bool skipSetItem = false;
        uint32 indexToSet = ValidateAndReturnIndex(index, &skipSetItem, isNumericIndex);

        // If index is not numeric, goto [[Set]] property path
        if (*isNumericIndex)
        {
            // [[Set]] step 1.b.ii: canonical numeric assignment to an
            // immutable view reports failure before value conversion.
            if (IsImmutableBuffer(GetArrayBuffer()))
            {
                return FALSE;
            }
            return skipSetItem ?
                DirectSetItemNoSet(indexToSet, value) :
                DirectSetItem(indexToSet, value);
        }
        else
        {
            return TRUE;
        }
    }

    // Validate the index used for typed arrays with below rules:
    // 1. if numeric string, let sIndex = ToNumber(index) :
    //    a. if sIndex is not integer, skip set operation
    //    b. if sIndex == -0, skip set operation
    //    c. if sIndex < 0 or sIndex >= length, skip set operation
    //    d. else return sIndex and perform set operation
    // 2. if tagged int, let nIndex = untag(index) :
    //    a. if nIndex < 0 or nIndex >= length, skip set operation
    // 3. else:
    //    a. if index is not integer, skip set operation
    //    b. if index < 0 or index >= length, skip set operation
    //    NOTE: if index == -0, it is treated as 0 and perform set operation
    //          as per 7.1.12.1 of ES6 spec 7.1.12.1 ToString Applied to the Number Type
    _Use_decl_annotations_ uint32 TypedArrayBase::ValidateAndReturnIndex(Js::Var index, bool * skipOperation, bool * isNumericIndex)
    {
        *skipOperation = false;
        *isNumericIndex = true;
        uint32 length = GetLength();

        if (TaggedInt::Is(index))
        {
            int32 indexInt = TaggedInt::ToInt32(index);
            *skipOperation = (indexInt < 0 || (uint32)indexInt >= length);
            return (uint32)indexInt;
        }
        else
        {
            double dIndexValue = 0;
            if (VarIs<JavascriptString>(index))
            {
                if (JavascriptConversion::CanonicalNumericIndexString(VarTo<JavascriptString>(index), &dIndexValue, GetScriptContext()))
                {
                    if (JavascriptNumber::IsNegZero(dIndexValue))
                    {
                        *skipOperation = true;
                        return 0;
                    }
                    // If this is numeric index embedded in string, perform regular numeric index checks below
                }
                else
                {
                    // not numeric index, go the [[Set]] path to add as string property
                    *isNumericIndex = false;
                    return 0;
                }
            }
            else
            {
                // JavascriptNumber::Is_NoTaggedIntCheck(index)
                dIndexValue = JavascriptNumber::GetValue(index);
            }

            // OK to lose data because we want to verify ToInteger()
            uint32 uint32Index = (uint32)dIndexValue;

            // IsInteger()
            if ((double)uint32Index != dIndexValue)
            {
                *skipOperation = true;
            }
            // index >= length
            else if (uint32Index >= GetLength())
            {
                *skipOperation = true;
            }
            return uint32Index;
        }
    }

    // static
    JavascriptFunction* TypedArrayBase::GetDefaultConstructor(Var object, ScriptContext* scriptContext)
    {
        switch (JavascriptOperators::GetTypeId(object))
        {
        case TypeId::TypeIds_Int8Array:
            return scriptContext->GetLibrary()->GetInt8ArrayConstructor();
        case TypeId::TypeIds_Uint8Array:
            return scriptContext->GetLibrary()->GetUint8ArrayConstructor();
        case TypeId::TypeIds_Uint8ClampedArray:
            return scriptContext->GetLibrary()->GetUint8ClampedArrayConstructor();
        case TypeId::TypeIds_Int16Array:
            return scriptContext->GetLibrary()->GetInt16ArrayConstructor();
        case TypeId::TypeIds_Uint16Array:
            return scriptContext->GetLibrary()->GetUint16ArrayConstructor();
        case TypeId::TypeIds_Int32Array:
            return scriptContext->GetLibrary()->GetInt32ArrayConstructor();
        case TypeId::TypeIds_Uint32Array:
            return scriptContext->GetLibrary()->GetUint32ArrayConstructor();
        case TypeId::TypeIds_Float32Array:
            return scriptContext->GetLibrary()->GetFloat32ArrayConstructor();
        case TypeId::TypeIds_Float64Array:
            return scriptContext->GetLibrary()->GetFloat64ArrayConstructor();
        case TypeId::TypeIds_Float16Array:
            return scriptContext->GetLibrary()->GetFloat16ArrayConstructor();
        case TypeId::TypeIds_Int64Array:
            return scriptContext->GetLibrary()->GetBigInt64ArrayConstructor();
        case TypeId::TypeIds_Uint64Array:
            return scriptContext->GetLibrary()->GetBigUint64ArrayConstructor();
        default:
            Assert(UNREACHED);
            return nullptr;
        }
    }

    Var TypedArrayBase::FindMinOrMax(Js::ScriptContext * scriptContext, TypeId typeId, bool findMax)
    {
        if (this->IsDetachedBuffer()) // 9.4.5.8 IntegerIndexedElementGet
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }

        switch (typeId)
        {
        case TypeIds_Int8Array:
            return this->FindMinOrMax<int8, false>(scriptContext, findMax);

        case TypeIds_Uint8Array:
        case TypeIds_Uint8ClampedArray:
            return this->FindMinOrMax<uint8, false>(scriptContext, findMax);

        case TypeIds_Int16Array:
            return this->FindMinOrMax<int16, false>(scriptContext, findMax);

        case TypeIds_Uint16Array:
            return this->FindMinOrMax<uint16, false>(scriptContext, findMax);

        case TypeIds_Int32Array:
            return this->FindMinOrMax<int32, false>(scriptContext, findMax);

        case TypeIds_Uint32Array:
            return this->FindMinOrMax<uint32, false>(scriptContext, findMax);

        case TypeIds_Float32Array:
            return this->FindMinOrMax<float, true>(scriptContext, findMax);

        case TypeIds_Float64Array:
            return this->FindMinOrMax<double, true>(scriptContext, findMax);

        case TypeIds_Float16Array:
        {
            // Widen each element exactly, then reuse the double NaN/-0
            // semantics. Kept inline because Float16Bits is a storage
            // struct, not an arithmetic type.
            Float16Bits* typedBuffer = (Float16Bits*)this->buffer;
            uint len = this->GetLength();
            Assert(sizeof(Float16Bits) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            double currentRes = Float16::ToDouble(typedBuffer[0].bits);
            for (uint i = 0; i < len; i++)
            {
                Assert((i + 1) * sizeof(Float16Bits) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
                double compare = Float16::ToDouble(typedBuffer[i].bits);
                if (JavascriptNumber::IsNan(compare))
                {
                    return scriptContext->GetLibrary()->GetNaN();
                }
                if (findMax ? currentRes < compare : currentRes > compare ||
                    (compare == 0 && Js::JavascriptNumber::IsNegZero(currentRes)))
                {
                    currentRes = compare;
                }
            }
            return Js::JavascriptNumber::ToVarNoCheck(currentRes, scriptContext);
        }

        default:
            AssertMsg(false, "Unsupported array for fast path");
            return nullptr;
        }
    }

    template<typename T, bool checkNaNAndNegZero>
    Var TypedArrayBase::FindMinOrMax(Js::ScriptContext * scriptContext, bool findMax)
    {
        T* typedBuffer = (T*)this->buffer;
        uint len = this->GetLength();

        Assert(sizeof(T)+GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        T currentRes = typedBuffer[0];
        for (uint i = 0; i < len; i++)
        {
            Assert((i + 1) * sizeof(T)+GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            T compare = typedBuffer[i];
            if (checkNaNAndNegZero && JavascriptNumber::IsNan(double(compare)))
            {
                return scriptContext->GetLibrary()->GetNaN();
            }
            if (findMax ? currentRes < compare : currentRes > compare ||
                (checkNaNAndNegZero && compare == 0 && Js::JavascriptNumber::IsNegZero(double(currentRes))))
            {
                currentRes = compare;
            }
        }
        return Js::JavascriptNumber::ToVarNoCheck(currentRes, scriptContext);
    }

    // static
    TypedArrayBase * TypedArrayBase::ValidateTypedArray(Arguments &args, ScriptContext *scriptContext, LPCWSTR apiName, bool writeAccess)
    {
        if (args.Info.Count == 0)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        return ValidateTypedArray(args[0], scriptContext, apiName, writeAccess);
    }

    // static
    TypedArrayBase* TypedArrayBase::ValidateTypedArray(Var aValue, ScriptContext *scriptContext, LPCWSTR apiName, bool writeAccess)
    {
        TypedArrayBase *typedArrayBase = JavascriptOperators::TryFromVar<Js::TypedArrayBase>(aValue);
        if (!typedArrayBase)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray);
        }

        // ValidateTypedArray step 4: write-mode validation rejects an
        // immutable backing buffer before any length or argument work.
        if (writeAccess && IsImmutableBuffer(typedArrayBase->GetArrayBuffer()))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        if (typedArrayBase->IsDetachedBuffer() || typedArrayBase->IsOutOfBounds())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, apiName);
        }

        return typedArrayBase;
    }

    // static
    Var TypedArrayBase::TypedArrayCreate(Var constructor, Arguments *args, uint32 length, ScriptContext *scriptContext, bool writeAccess)
    {
        Var newObj = JavascriptOperators::NewScObject(constructor, *args, scriptContext);

        TypedArrayBase * typedArray = TypedArrayBase::ValidateTypedArray(newObj, scriptContext, nullptr, writeAccess);

        // ECMA262 22.2.4.6 TypedArrayCreate line 3. "If argumentList is a List of a single Number" (args[0] == constructor)
        if (args->Info.Count == 2 && typedArray->GetLength() < length)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_InvalidTypedArrayLength);
        }

        return newObj;
    }

#if ENABLE_TTD
    TTD::NSSnapObjects::SnapObjectType TypedArrayBase::GetSnapTag_TTD() const
    {
        return TTD::NSSnapObjects::SnapObjectType::SnapTypedArrayObject;
    }

    void TypedArrayBase::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
    {
        TTD::NSSnapObjects::SnapTypedArrayInfo* stai = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapTypedArrayInfo>();
        stai->ArrayBufferAddr = TTD_CONVERT_VAR_TO_PTR_ID(this->GetArrayBuffer());
        stai->ByteOffset = this->GetByteOffset();
        stai->Length = this->GetLength();

        TTD_PTR_ID* depArray = alloc.SlabAllocateArray<TTD_PTR_ID>(1);
        depArray[0] = TTD_CONVERT_VAR_TO_PTR_ID(this->GetArrayBuffer());

        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapTypedArrayInfo*, TTD::NSSnapObjects::SnapObjectType::SnapTypedArrayObject>(objData, stai, alloc, 1, depArray);
    }
#endif

    template<>
    inline BOOL Int8Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt8);
    }

    template<>
    inline BOOL Int8VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt8);
    }

    template<>
    inline BOOL Int8Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt8);
    }

    template<>
    inline BOOL Int8VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt8);
    }

    template<>
    inline Var Int8Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

#define DIRECT_SET_NO_DETACH_CHECK(TypedArrayName, convertFn) \
    template<> \
    inline BOOL TypedArrayName##::DirectSetItemNoDetachCheck(_In_ uint32 index, _In_ Var value) \
    { \
        return BaseTypedDirectSetItemNoDetachCheck(index, value, convertFn); \
    }

    DIRECT_SET_NO_DETACH_CHECK(Int8Array, JavascriptConversion::ToInt8);
    DIRECT_SET_NO_DETACH_CHECK(Int8VirtualArray, JavascriptConversion::ToInt8);
    DIRECT_SET_NO_DETACH_CHECK(Uint8Array, JavascriptConversion::ToUInt8);
    DIRECT_SET_NO_DETACH_CHECK(Uint8VirtualArray, JavascriptConversion::ToUInt8);
    DIRECT_SET_NO_DETACH_CHECK(Int16Array, JavascriptConversion::ToInt16);
    DIRECT_SET_NO_DETACH_CHECK(Int16VirtualArray, JavascriptConversion::ToInt16);
    DIRECT_SET_NO_DETACH_CHECK(Uint16Array, JavascriptConversion::ToUInt16);
    DIRECT_SET_NO_DETACH_CHECK(Uint16VirtualArray, JavascriptConversion::ToUInt16);
    DIRECT_SET_NO_DETACH_CHECK(Int32Array, JavascriptConversion::ToInt32);
    DIRECT_SET_NO_DETACH_CHECK(Int32VirtualArray, JavascriptConversion::ToInt32);
    DIRECT_SET_NO_DETACH_CHECK(Uint32Array, JavascriptConversion::ToUInt32);
    DIRECT_SET_NO_DETACH_CHECK(Uint32VirtualArray, JavascriptConversion::ToUInt32);
    DIRECT_SET_NO_DETACH_CHECK(Float32Array, JavascriptConversion::ToFloat);
    DIRECT_SET_NO_DETACH_CHECK(Float32VirtualArray, JavascriptConversion::ToFloat);
    DIRECT_SET_NO_DETACH_CHECK(Float64Array, JavascriptConversion::ToNumber);
    DIRECT_SET_NO_DETACH_CHECK(Float64VirtualArray, JavascriptConversion::ToNumber);
    DIRECT_SET_NO_DETACH_CHECK(Float16Array, ToFloat16Element);
    DIRECT_SET_NO_DETACH_CHECK(Int64Array, ToBigInt64Element);
    DIRECT_SET_NO_DETACH_CHECK(Uint64Array, ToBigUint64Element);
    DIRECT_SET_NO_DETACH_CHECK(Uint8ClampedArray, JavascriptConversion::ToUInt8Clamped);
    DIRECT_SET_NO_DETACH_CHECK(Uint8ClampedVirtualArray, JavascriptConversion::ToUInt8Clamped);
    DIRECT_SET_NO_DETACH_CHECK(BoolArray, JavascriptConversion::ToBool);

#define DIRECT_GET_NO_DETACH_CHECK(TypedArrayName) \
    template<> \
    inline Var TypedArrayName##::DirectGetItemNoDetachCheck(_In_ uint32 index) \
    { \
        return BaseTypedDirectGetItemNoDetachCheck(index); \
    }

#define DIRECT_GET_VAR_CHECK_NO_DETACH_CHECK(TypedArrayName) \
    template<> \
    inline Var TypedArrayName##::DirectGetItemNoDetachCheck(_In_ uint32 index) \
    { \
        return DirectGetItemVarCheckNoDetachCheck(index); \
    }

    DIRECT_GET_NO_DETACH_CHECK(Int8Array);
    DIRECT_GET_NO_DETACH_CHECK(Int8VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Uint8Array);
    DIRECT_GET_NO_DETACH_CHECK(Uint8VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Int16Array);
    DIRECT_GET_NO_DETACH_CHECK(Int16VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Uint16Array);
    DIRECT_GET_NO_DETACH_CHECK(Uint16VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Int32Array);
    DIRECT_GET_NO_DETACH_CHECK(Int32VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Uint32Array);
    DIRECT_GET_NO_DETACH_CHECK(Uint32VirtualArray);
    DIRECT_GET_NO_DETACH_CHECK(Uint8ClampedArray);
    DIRECT_GET_NO_DETACH_CHECK(Uint8ClampedVirtualArray);
    DIRECT_GET_VAR_CHECK_NO_DETACH_CHECK(Float32Array);
    DIRECT_GET_VAR_CHECK_NO_DETACH_CHECK(Float32VirtualArray);
    DIRECT_GET_VAR_CHECK_NO_DETACH_CHECK(Float64Array);
    DIRECT_GET_VAR_CHECK_NO_DETACH_CHECK(Float64VirtualArray);

    // Float16Array loads widen exactly; the generic helpers take an
    // arithmetic element type, so both getters are specialized below.
    template<>
    inline Var Float16Array::DirectGetItemNoDetachCheck(_In_ uint32 index)
    {
        Assert(!IsDetachedBuffer());
        Assert(index < GetLength());
        Assert((index + 1)* sizeof(Float16Bits) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        return Float16ElementAt((uint16*)buffer, index, GetScriptContext());
    }

#define TypedArrayBeginStub(TypedArrayName) \
        /* The index was validated before value coercion; user code may detach, \
           shrink or grow the buffer, so the post-conversion check owns the \
           live buffer/index invariant for value-taking ops. */ \
        ScriptContext *scriptContext = GetScriptContext(); \
        typedef TypedArrayName::TypedArrayType type; \
        type *buffer = (type*)this->buffer + accessIndex;

// The value conversion below can run user code that detaches the buffer or
// shrinks a resizable one. `buffer` is a raw pointer captured above, so
// revalidate after conversion and before the atomic instruction.
#define TypedArrayCheckStillValidForAtomics() \
        if (IsDetachedBuffer() || IsOutOfBounds() || accessIndex >= GetLength()) \
        { \
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("Atomics")); \
        }

#define TypedArrayStore(TypedArrayName, fnName, convertFn) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex, _In_ Var value) \
    { \
        TypedArrayBeginStub(TypedArrayName); \
        double retVal = JavascriptConversion::ToInteger(value, scriptContext); \
        TypedArrayCheckStillValidForAtomics(); \
        AtomicsOperations::fnName(buffer, convertFn(retVal)); \
        return JavascriptNumber::ToVarWithCheck(retVal, scriptContext); \
    }

#define TypedArrayOp1(TypedArrayName, fnName, convertFn) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex) \
    { \
        TypedArrayBeginStub(TypedArrayName); \
        type result = AtomicsOperations::fnName(buffer); \
        return JavascriptNumber::ToVar(result, scriptContext); \
    }

#define TypedArrayOp2(TypedArrayName, fnName, convertFn) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex, _In_ Var value) \
    { \
        TypedArrayBeginStub(TypedArrayName); \
        type convertedValue = convertFn(value, scriptContext); \
        TypedArrayCheckStillValidForAtomics(); \
        type result = AtomicsOperations::fnName(buffer, convertedValue); \
        return JavascriptNumber::ToVar(result, scriptContext); \
    }

#define TypedArrayOp3(TypedArrayName, fnName, convertFn) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex, _In_ Var first, _In_ Var value) \
    { \
        TypedArrayBeginStub(TypedArrayName); \
        type firstValue = convertFn(first, scriptContext); \
        type secondValue = convertFn(value, scriptContext); \
        TypedArrayCheckStillValidForAtomics(); \
        type result = AtomicsOperations::fnName(buffer, firstValue, secondValue); \
        return JavascriptNumber::ToVar(result, scriptContext); \
    }

#define GenerateNotSupportedStub1(TypedArrayName, fnName) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex) \
    { \
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray); \
    }

#define GenerateNotSupportedStub2(TypedArrayName, fnName) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex, _In_ Var value) \
    { \
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray); \
    }

#define GenerateNotSupportedStub3(TypedArrayName, fnName) \
    template<>\
    Var TypedArrayName##::Typed##fnName(_In_ uint32 accessIndex, _In_ Var first, _In_ Var value) \
    { \
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray); \
    }


#define GENERATE_FOREACH_TYPEDARRAY(TYPEDARRAY_DEF, NOTSUPPORTEDSTUB, OP) \
        TYPEDARRAY_DEF(Int8Array, OP, JavascriptConversion::ToInt8); \
        TYPEDARRAY_DEF(Int8VirtualArray, OP, JavascriptConversion::ToInt8); \
        TYPEDARRAY_DEF(Uint8Array, OP, JavascriptConversion::ToUInt8); \
        TYPEDARRAY_DEF(Uint8VirtualArray, OP, JavascriptConversion::ToUInt8); \
        TYPEDARRAY_DEF(Int16Array, OP, JavascriptConversion::ToInt16); \
        TYPEDARRAY_DEF(Int16VirtualArray, OP, JavascriptConversion::ToInt16); \
        TYPEDARRAY_DEF(Uint16Array, OP, JavascriptConversion::ToUInt16); \
        TYPEDARRAY_DEF(Uint16VirtualArray, OP, JavascriptConversion::ToUInt16); \
        TYPEDARRAY_DEF(Int32Array, OP, JavascriptConversion::ToInt32); \
        TYPEDARRAY_DEF(Int32VirtualArray, OP, JavascriptConversion::ToInt32); \
        TYPEDARRAY_DEF(Uint32Array, OP, JavascriptConversion::ToUInt32); \
        TYPEDARRAY_DEF(Uint32VirtualArray, OP, JavascriptConversion::ToUInt32); \
        NOTSUPPORTEDSTUB(Float32Array, OP); \
        NOTSUPPORTEDSTUB(Float32VirtualArray, OP); \
        NOTSUPPORTEDSTUB(Float64Array, OP); \
        NOTSUPPORTEDSTUB(Float64VirtualArray, OP); \
        NOTSUPPORTEDSTUB(Float16Array, OP); \
        NOTSUPPORTEDSTUB(Int64Array, OP); \
        NOTSUPPORTEDSTUB(Uint64Array, OP); \
        NOTSUPPORTEDSTUB(Uint8ClampedArray, OP); \
        NOTSUPPORTEDSTUB(Uint8ClampedVirtualArray, OP); \
        NOTSUPPORTEDSTUB(BoolArray, OP);

    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, Add)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, And)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp3, GenerateNotSupportedStub3, CompareExchange)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, Exchange)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp1, GenerateNotSupportedStub1, Load)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, Or)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayStore, GenerateNotSupportedStub2, Store)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, Sub)
    GENERATE_FOREACH_TYPEDARRAY(TypedArrayOp2, GenerateNotSupportedStub2, Xor)

    template<>
    VTableValue Int8Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt8Array;
    }

    template<>
    inline Var Int8VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Int8VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt8VirtualArray;
    }

    template<>
    inline BOOL Uint8Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt8);
    }

    template<>
    inline BOOL Uint8Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt8);
    }

    template<>
    inline Var Uint8Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint8Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint8Array;
    }

    template<>
    inline BOOL Uint8VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt8);
    }

    template<>
    inline BOOL Uint8VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt8);
    }

    template<>
    inline Var Uint8VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint8VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint8VirtualArray;
    }

    template<>
    inline BOOL Uint8ClampedArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt8Clamped);
    }

    template<>
    inline BOOL Uint8ClampedArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt8Clamped);
    }

    template<>
    inline Var Uint8ClampedArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint8ClampedArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint8ClampedArray;
    }

    template<>
    inline BOOL Uint8ClampedVirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt8Clamped);
    }

    template<>
    inline BOOL Uint8ClampedVirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt8Clamped);
    }

    template<>
    inline Var Uint8ClampedVirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint8ClampedVirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint8ClampedVirtualArray;
    }

    template<>
    inline BOOL Int16Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt16);
    }

    template<>
    inline BOOL Int16Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt16);
    }

    template<>
    inline Var Int16Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Int16Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt16Array;
    }

    template<>
    inline BOOL Int16VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt16);
    }

    template<>
    inline BOOL Int16VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt16);
    }

    template<>
    inline Var Int16VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Int16VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt16VirtualArray;
    }

    template<>
    inline BOOL Uint16Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt16);
    }

    template<>
    inline BOOL Uint16Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt16);
    }

    template<>
    inline Var Uint16Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint16Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint16Array;
    }

    template<>
    inline BOOL Uint16VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt16);
    }

    template<>
    inline BOOL Uint16VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt16);
    }

    template<>
    inline Var Uint16VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint16VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint16VirtualArray;
    }

    template<>
    inline BOOL Int32Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt32);
    }

    template<>
    inline BOOL Int32Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt32);
    }

    template<>
    inline Var Int32Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Int32Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt32Array;
    }

    template<>
    inline BOOL Int32VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToInt32);
    }

    template<>
    inline BOOL Int32VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToInt32);
    }

    template<>
    inline Var Int32VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Int32VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt32VirtualArray;
    }

    template<>
    inline BOOL Uint32Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt32);
    }

    template<>
    inline BOOL Uint32Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt32);
    }

    template<>
    inline Var Uint32Array::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint32Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint32Array;
    }

    template<>
    inline BOOL Uint32VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToUInt32);
    }

    template<>
    inline BOOL Uint32VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToUInt32);
    }

    template<>
    inline Var Uint32VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return BaseTypedDirectGetItem(index);
    }

    template<>
    VTableValue Uint32VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint32VirtualArray;
    }

    template<>
    inline BOOL Float32Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToFloat);
    }

    template<>
    inline BOOL Float32Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToFloat);
    }

    template<>
    Var Float32Array::DirectGetItem(_In_ uint32 index)
    {
        return TypedDirectGetItemWithCheck(index);
    }

    template<>
    VTableValue Float32Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableFloat32Array;
    }

    template<>
    inline BOOL Float32VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToFloat);
    }

    template<>
    inline BOOL Float32VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToFloat);
    }

    template<>
    Var Float32VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return TypedDirectGetItemWithCheck(index);
    }

    template<>
    VTableValue Float32VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableFloat32VirtualArray;
    }

    template<>
    inline BOOL Float64Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToNumber);
    }

    template<>
    inline BOOL Float64Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToNumber);
    }

    template<>
    inline Var Float64Array::DirectGetItem(_In_ uint32 index)
    {
        return TypedDirectGetItemWithCheck(index);
    }

    template<>
    VTableValue Float64Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableFloat64Array;
    }

    template<>
    inline BOOL Float64VirtualArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToNumber);
    }

    template<>
    inline BOOL Float64VirtualArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToNumber);
    }

    template<>
    inline Var Float64VirtualArray::DirectGetItem(_In_ uint32 index)
    {
        return TypedDirectGetItemWithCheck(index);
    }

    template<>
    VTableValue Float64VirtualArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableFloat64VirtualArray;
    }

    template<>
    inline BOOL Int64Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedBigIntDirectSetItem(index, value, ToBigInt64Element);
    }

    template<>
    inline BOOL Int64Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedBigIntDirectSetItemNoSet(index, value, ToBigInt64Element);
    }

    template<>
    inline Var Int64Array::DirectGetItem(_In_ uint32 index)
    {
        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }
        if (index < GetLength())
        {
            Assert((index + 1) * sizeof(int64) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            return BigInt64ElementAt((int64*)buffer, index, GetScriptContext());
        }
        return GetLibrary()->GetUndefined();
    }

    template<>
    inline Var Int64Array::DirectGetItemNoDetachCheck(_In_ uint32 index)
    {
        Assert(!IsDetachedBuffer());
        Assert(index < GetLength());
        Assert((index + 1) * sizeof(int64) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        return BigInt64ElementAt((int64*)buffer, index, GetScriptContext());
    }

    template<>
    VTableValue Int64Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableInt64Array;
    }

    template<>
    inline BOOL Uint64Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedBigIntDirectSetItem(index, value, ToBigUint64Element);
    }

    template<>
    inline BOOL Uint64Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedBigIntDirectSetItemNoSet(index, value, ToBigUint64Element);
    }

    template<>
    inline Var Uint64Array::DirectGetItem(_In_ uint32 index)
    {
        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }
        if (index < GetLength())
        {
            Assert((index + 1) * sizeof(uint64) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            return BigUint64ElementAt((uint64*)buffer, index, GetScriptContext());
        }
        return GetLibrary()->GetUndefined();
    }

    template<>
    inline Var Uint64Array::DirectGetItemNoDetachCheck(_In_ uint32 index)
    {
        Assert(!IsDetachedBuffer());
        Assert(index < GetLength());
        Assert((index + 1) * sizeof(uint64) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        return BigUint64ElementAt((uint64*)buffer, index, GetScriptContext());
    }

    template<>
    VTableValue Uint64Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableUint64Array;
    }

    template<>
    inline BOOL Float16Array::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, ToFloat16Element);
    }

    template<>
    inline BOOL Float16Array::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, ToFloat16Element);
    }

    template<>
    inline Var Float16Array::DirectGetItem(_In_ uint32 index)
    {
        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }
        if (index < GetLength())
        {
            Assert((index + 1) * sizeof(Float16Bits) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            return Float16ElementAt((uint16*)buffer, index, GetScriptContext());
        }
        return GetLibrary()->GetUndefined();
    }

    template<>
    VTableValue Float16Array::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableFloat16Array;
    }

    template<>
    inline BOOL BoolArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItem(index, value, JavascriptConversion::ToBool);
    }

    template<>
    inline BOOL BoolArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        return BaseTypedDirectSetItemNoSet(index, value, JavascriptConversion::ToBool);
    }

    template<>
    inline Var BoolArray::DirectGetItem(_In_ uint32 index)
    {
        if (index < GetLength())
        {
            Assert((index+1)* sizeof(bool) +GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            bool* typedBuffer = (bool*)buffer;
            return typedBuffer[index] ? GetLibrary()->GetTrue() : GetLibrary()->GetFalse();
        }
        return GetLibrary()->GetUndefined();
    }

    template<>
    inline Var BoolArray::DirectGetItemNoDetachCheck(_In_ uint32 index)
    {
        Assert((index + 1)* sizeof(bool) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        bool* typedBuffer = (bool*)buffer;
        return typedBuffer[index] ? GetLibrary()->GetTrue() : GetLibrary()->GetFalse();
    }

    template<>
    VTableValue BoolArray::DummyVirtualFunctionToHinderLinkerICF()
    {
        return VTableValue::VtableBoolArray;
    }


    Var CharArray::Create(ArrayBufferBase* arrayBuffer, uint32 byteOffSet, uint32 mappedLength, JavascriptLibrary* javascriptLibrary, bool lengthTracking)
    {
        CharArray* arr;
        uint32 totalLength, mappedByteLength;
        if (UInt32Math::Mul(mappedLength, sizeof(char16), &mappedByteLength) ||
            UInt32Math::Add(byteOffSet, mappedByteLength, &totalLength) ||
            (totalLength > arrayBuffer->GetByteLength()))
        {
            JavascriptError::ThrowRangeError(arrayBuffer->GetScriptContext(), JSERR_InvalidTypedArrayLength);
        }
        arr = RecyclerNew(javascriptLibrary->GetRecycler(), CharArray, arrayBuffer, byteOffSet, mappedLength, javascriptLibrary->GetCharArrayType(), lengthTracking);
        return arr;
    }

    template <> bool VarIsImpl<CharArray>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == Js::TypeIds_CharArray;
    }

    Var CharArray::EntrySet(RecyclableObject* function, CallInfo callInfo, ...)
    {
        AssertMsg(FALSE, "not supported in char array");
        JavascriptError::ThrowTypeError(function->GetScriptContext(), JSERR_This_NeedTypedArray);
    }

    Var CharArray::EntrySubarray(RecyclableObject* function, CallInfo callInfo, ...)
    {
        AssertMsg(FALSE, "not supported in char array");
        JavascriptError::ThrowTypeError(function->GetScriptContext(), JSERR_This_NeedTypedArray);
    }

    inline Var CharArray::Subarray(uint32 begin, uint32 end, bool lengthTracking)
    {
        AssertMsg(FALSE, "not supported in char array");
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_This_NeedTypedArray);
    }

    inline BOOL CharArray::DirectSetItem(_In_ uint32 index, _In_ Js::Var value)
    {
        ScriptContext* scriptContext = GetScriptContext();
        // A typed array is Integer Indexed Exotic object, so doing a get translates to 9.4.5.9 IntegerIndexedElementSet
        LPCWSTR asString = (Js::JavascriptConversion::ToString(value, scriptContext))->GetSz();

        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }

        if (index >= GetLength())
        {
            return FALSE;
        }

        AssertMsg(index < GetLength(), "Trying to set out of bound index for typed array.");
        Assert((index + 1)* sizeof(char16) + GetByteOffset() <= GetArrayBuffer()->GetByteLength());
        char16* typedBuffer = (char16*)buffer;

        if (asString != NULL && ::wcslen(asString) == 1)
        {
            typedBuffer[index] = asString[0];
        }
        else
        {
            Js::JavascriptError::MapAndThrowError(scriptContext, E_INVALIDARG);
        }

        return TRUE;
    }

    inline BOOL CharArray::DirectSetItemNoSet(_In_ uint32 index, _In_ Js::Var value)
    {
        ScriptContext* scriptContext = GetScriptContext();
        // A typed array is Integer Indexed Exotic object, so doing a get translates to 9.4.5.9 IntegerIndexedElementSet
        Js::JavascriptConversion::ToString(value, scriptContext);

        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }

        return FALSE;
    }

    inline BOOL CharArray::DirectSetItemNoDetachCheck(_In_ uint32 index, _In_ Js::Var value)
    {
        return DirectSetItem(index, value);
    }

    inline Var CharArray::DirectGetItemNoDetachCheck(_In_ uint32 index)
    {
        return DirectGetItem(index);
    }

    Var CharArray::TypedAdd(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedAnd(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedCompareExchange(_In_ uint32 index, _In_ Var comparand, _In_ Var replacementValue)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedExchange(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedLoad(_In_ uint32 index)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedOr(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedStore(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedSub(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    Var CharArray::TypedXor(_In_ uint32 index, _In_ Var second)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_InvalidOperationOnTypedArray);
    }

    inline Var CharArray::DirectGetItem(_In_ uint32 index)
    {
        // A typed array is Integer Indexed Exotic object, so doing a get translates to 9.4.5.8 IntegerIndexedElementGet
        if (this->IsDetachedBuffer())
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_DetachedTypedArray);
        }
        if (index < GetLength())
        {
            Assert((index + 1)* sizeof(char16)+GetByteOffset() <= GetArrayBuffer()->GetByteLength());
            char16* typedBuffer = (char16*)buffer;
            return GetLibrary()->GetCharStringCache().GetStringForChar(typedBuffer[index]);
        }
        return GetLibrary()->GetUndefined();
    }

    Var CharArray::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        function->GetScriptContext()->GetThreadContext()->ProbeStack(Js::Constants::MinStackDefault, function->GetScriptContext());

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Assert(!(callInfo.Flags & CallFlags_New) || args[0] == nullptr);
        Var object = TypedArrayBase::CreateNewInstance(args, scriptContext, sizeof(char16), CharArray::Create);
#if ENABLE_DEBUG_CONFIG_OPTIONS
        if (Js::Configuration::Global.flags.IsEnabled(Js::autoProxyFlag))
        {
            object = Js::JavascriptProxy::AutoProxyWrapper(object);
        }
#endif
        return object;
    }
}
