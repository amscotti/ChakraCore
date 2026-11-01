//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    // Used when a detached state must release only its RefCountedBuffer: the
    // host owns the backing bytes (externalized/external ArrayBuffer).
    void __cdecl NoOpFree(void* data);

    long RefCountedBuffer::AddRef()
    {
        long ref = InterlockedIncrement(&refCount);
        AssertOrFailFast(ref > 1);
        return ref;
    }

    long RefCountedBuffer::Release()
    {
        long ref = InterlockedDecrement(&refCount);
        AssertOrFailFastMsg(ref >= 0, "Buffer already freed");
        return ref;
    }

    void ArrayBufferDetachedStateBase::AddRefBufferContent()
    {
        if (buffer != nullptr)
        {
            buffer->AddRef();
        }
    }

    long ArrayBufferDetachedStateBase::ReleaseRefBufferContent()
    {
        if (buffer != nullptr)
        {
            long ref = buffer->Release();
            AssertOrFailFast(ref > 0);
            return ref;
        }
        return 0;
    }

    template <> bool VarIsImpl<ArrayBufferBase>(RecyclableObject* obj)
    {
        return VarIs<ArrayBuffer>(obj) || VarIs<SharedArrayBuffer>(obj);
    }

    ArrayBuffer* ArrayBuffer::NewFromDetachedState(DetachedStateBase* state, JavascriptLibrary *library)
    {
        ArrayBufferDetachedStateBase* arrayBufferState = (ArrayBufferDetachedStateBase *)state;
        ArrayBuffer *toReturn = nullptr;

        switch (arrayBufferState->allocationType)
        {
        case ArrayBufferAllocationType::CoTask:
            toReturn = library->CreateProjectionArraybuffer(arrayBufferState->buffer, arrayBufferState->bufferLength);
            break;
        case ArrayBufferAllocationType::Heap:
        case ArrayBufferAllocationType::MemAlloc:
            toReturn = library->CreateArrayBuffer(arrayBufferState->buffer, arrayBufferState->bufferLength);
            break;
        case ArrayBufferAllocationType::External:
            toReturn = static_cast<ExternalArrayBufferDetachedState*>(state)->Create(library);
            break;
        default:
            AssertMsg(false, "Unknown allocationType of ArrayBufferDetachedStateBase ");
        }

        return toReturn;
    }

    uint32 ArrayBuffer::GetByteLength() const
    {
        return this->bufferLength;
    }

    BYTE* ArrayBuffer::GetBuffer() const
    {
        return this->bufferContent != nullptr ? this->bufferContent->GetBuffer() : nullptr;
    }

    void ArrayBuffer::DetachBufferFromParent(ArrayBufferParent* parent)
    {
        if (parent == nullptr)
        {
            return;
        }

        switch (JavascriptOperators::GetTypeId(parent))
        {
        case TypeIds_Int8Array:
                if (VarIs<Int8VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Int8VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Int8Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Int8VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Int8Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Uint8Array:
                if (VarIs<Uint8VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Uint8VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Uint8Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Uint8VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Uint8Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Uint8ClampedArray:
                if (VarIs<Uint8ClampedVirtualArray>(parent))
                {
                    if (VirtualTableInfo<Uint8ClampedVirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Uint8ClampedArray>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Uint8ClampedVirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Uint8ClampedArray>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Int16Array:
                if (VarIs<Int16VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Int16VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Int16Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Int16VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Int16Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Uint16Array:
                if (VarIs<Uint16VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Uint16VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Uint16Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Uint16VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Uint16Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Int32Array:
                if (VarIs<Int32VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Int32VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Int32Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Int32VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Int32Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Uint32Array:
                if (VarIs<Uint32VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Uint32VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Uint32Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Uint32VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Uint32Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Float32Array:
                if (VarIs<Float32VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Float32VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Float32Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Float32VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Float32Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Float64Array:
                if (VarIs<Float64VirtualArray>(parent))
                {
                    if (VirtualTableInfo<Float64VirtualArray>::HasVirtualTable(parent))
                    {
                        VirtualTableInfo<Float64Array>::SetVirtualTable(parent);
                    }
                    else
                    {
                        Assert(VirtualTableInfo<CrossSiteObject<Float64VirtualArray>>::HasVirtualTable(parent));
                        VirtualTableInfo<CrossSiteObject<Float64Array>>::SetVirtualTable(parent);
                    }
                }
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
                break;

        case TypeIds_Float16Array:
                // Float16Array has no virtual-array variant: fixed views
                // only record the detach, like the BigInt kinds.
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
            break;

        case TypeIds_Int64Array:
        case TypeIds_Uint64Array:
        case TypeIds_CharArray:
        case TypeIds_BoolArray:
                UnsafeVarTo<TypedArrayBase>(parent)->ClearLengthAndBufferOnDetach();
            break;

        case TypeIds_DataView:
                VarTo<DataView>(parent)->ClearLengthAndBufferOnDetach();
            break;

        default:
            AssertMsg(false, "We need an explicit case for any parent of ArrayBuffer.");
            break;
        }
    }

    void ArrayBuffer::ReportExternalMemoryFree()
    {
        Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
        recycler->ReportExternalMemoryFree(allocatedByteLength);
    }

    void ArrayBuffer::Detach()
    {
        Assert(!this->isDetached);

        // we are about to lose track of the buffer to another owner
        // report that we no longer own the memory
        ReportExternalMemoryFree();

        this->bufferContent = nullptr;
        this->bufferLength = 0;
        this->allocatedByteLength = 0;
        this->isDetached = true;

        if (this->primaryParent != nullptr && this->primaryParent->Get() == nullptr)
        {
            this->primaryParent = nullptr;
        }

        if (this->primaryParent != nullptr)
        {
            this->DetachBufferFromParent(this->primaryParent->Get());
        }

        if (this->otherParents != nullptr)
        {
            this->otherParents->Map([&](RecyclerWeakReference<ArrayBufferParent>* item)
            {
                this->DetachBufferFromParent(item->Get());
            });
        }
    }

    ArrayBufferDetachedStateBase* ArrayBuffer::DetachAndGetState(bool queueForDelayFree/* = true*/)
    {
        // Save the state before detaching. The delayed-free scan must cover the
        // full allocation (capacity), not just the current logical length.
        uint32 allocatedLength = this->allocatedByteLength;
        AutoPtr<ArrayBufferDetachedStateBase> arrayBufferState(this->CreateDetachedState(this->bufferContent, this->bufferLength));
        Detach();

        // Now put this bufferContent to the queue so that we can manage the lifetime of the buffer later.
        if (queueForDelayFree && arrayBufferState->buffer != nullptr)
        {
            DelayedFreeArrayBuffer * local = GetScriptContext()->GetThreadContext()->GetScanStackCallback();
            local->Push(this->CopyBufferContentForDelayedFree(arrayBufferState->buffer, allocatedLength));
        }

        return arrayBufferState.Detach();
    }

    void ArrayBuffer::AddParent(ArrayBufferParent* parent)
    {
        if (this->primaryParent == nullptr || this->primaryParent->Get() == nullptr)
        {
            this->primaryParent = this->GetRecycler()->CreateWeakReferenceHandle(parent);
        }
        else
        {
            if (this->otherParents == nullptr)
            {
                this->otherParents = RecyclerNew(this->GetRecycler(), OtherParents, this->GetRecycler());
            }

            if (this->otherParents->increasedCount >= ParentsCleanupThreshold)
            {
                auto iter = this->otherParents->GetEditingIterator();
                while (iter.Next())
                {
                    if (iter.Data()->Get() == nullptr)
                    {
                        iter.RemoveCurrent();
                    }
                }

                this->otherParents->increasedCount = 0;
            }

            this->otherParents->PrependNode(this->GetRecycler()->CreateWeakReferenceHandle(parent));
            this->otherParents->increasedCount++;
        }
    }

    ArrayBuffer * ArrayBuffer::GetAsArrayBuffer()
    {
        AssertOrFailFast(VarIsCorrectType(this));
        return this;
    }

    uint32 ArrayBuffer::ToIndex(Var value, int32 errorCode, ScriptContext *scriptContext, uint32 MaxAllowedLength, bool checkSameValueZero)
    {
        if (JavascriptOperators::IsUndefined(value))
        {
            return 0;
        }

        if (TaggedInt::Is(value))
        {
            int64 index = TaggedInt::ToInt64(value);
            if (index < 0 || index >(int64)MaxAllowedLength)
            {
                JavascriptError::ThrowRangeError(scriptContext, errorCode);
            }

            return  (uint32)index;
        }

        // Slower path

        double d = JavascriptConversion::ToInteger(value, scriptContext);
        if (d < 0.0 || d >(double)MaxAllowedLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, errorCode);
        }

        if (checkSameValueZero)
        {
            Var integerIndex = JavascriptNumber::ToVarNoCheck(d, scriptContext);
            Var index = JavascriptNumber::ToVar(JavascriptConversion::ToLength(integerIndex, scriptContext), scriptContext);
            if (!JavascriptConversion::SameValueZero(integerIndex, index))
            {
                JavascriptError::ThrowRangeError(scriptContext, errorCode);
            }
        }

        return (uint32)d;
    }

    // ToIndex without the engine's MaxArrayBufferLength clamp. The constructor
    // defers the engine allocation limit until after OrdinaryCreateFromConstructor
    // (spec AllocateArrayBuffer); values above 2^53 - 1 are still a RangeError.
    static double ToIndexNoEngineCap(Var value, int32 errorCode, ScriptContext* scriptContext)
    {
        if (JavascriptOperators::IsUndefined(value))
        {
            return 0.0;
        }

        if (TaggedInt::Is(value))
        {
            int64 index = TaggedInt::ToInt64(value);
            if (index < 0)
            {
                JavascriptError::ThrowRangeError(scriptContext, errorCode);
            }
            return (double)index;
        }

        double d = JavascriptConversion::ToInteger(value, scriptContext);
        if (d < 0.0 || d > 9007199254740991.0) // 2^53 - 1
        {
            JavascriptError::ThrowRangeError(scriptContext, errorCode);
        }
        return d;
    }

    Var ArrayBuffer::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);

        if (!args.IsNewCall() || (newTarget && JavascriptOperators::IsUndefinedObject(newTarget)))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("ArrayBuffer"));
        }

        double byteLengthValue = 0.0;
        if (args.Info.Count > 1)
        {
            byteLengthValue = ToIndexNoEngineCap(args[1], JSERR_ArrayLengthConstructIncorrect, scriptContext);
        }

        // GetArrayBufferMaxByteLengthOption: only objects participate; the
        // property is read before the allocation and coerced with ToIndex.
        bool hasMaxByteLength = false;
        double requestedMaxByteLength = 0.0;
        if (args.Info.Count > 2 && JavascriptOperators::IsObjectType(JavascriptOperators::GetTypeId(args[2])))
        {
            PropertyId maxByteLengthId = scriptContext->GetOrAddPropertyIdTracked(_u("maxByteLength"));
            Var maxByteLengthValue = JavascriptOperators::GetProperty(VarTo<RecyclableObject>(args[2]), maxByteLengthId, scriptContext);
            if (!JavascriptOperators::IsUndefined(maxByteLengthValue))
            {
                hasMaxByteLength = true;
                requestedMaxByteLength = ToIndexNoEngineCap(maxByteLengthValue, JSERR_ArrayLengthConstructIncorrect, scriptContext);
            }
        }

        // Validate byteLength/maxByteLength before newTarget.prototype access.
        if (hasMaxByteLength && byteLengthValue > requestedMaxByteLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        bool byteLengthExceedsEngineLimit = byteLengthValue > (double)MaxArrayBufferLength;
        bool maxByteLengthExceedsEngineLimit = hasMaxByteLength && requestedMaxByteLength > (double)MaxArrayBufferLength;

        // AllocateArrayBuffer creates the JS object before the data block. Do
        // the prototype lookup now so allocation failure happens after it.
        RecyclableObject* newTargetPrototype = nullptr;
        if (isCtorSuperCall)
        {
            Var proto = JavascriptOperators::GetPropertyNoCache(VarTo<RecyclableObject>(newTarget), PropertyIds::prototype, scriptContext);
            if (JavascriptOperators::IsObjectType(JavascriptOperators::GetTypeId(proto)))
            {
                newTargetPrototype = VarTo<RecyclableObject>(proto);
            }
        }

        if (byteLengthExceedsEngineLimit || maxByteLengthExceedsEngineLimit)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        uint32 byteLength = (uint32)byteLengthValue;
        RecyclableObject* newArr = hasMaxByteLength
            ? library->CreateResizableArrayBuffer(byteLength, (uint32)requestedMaxByteLength)
            : library->CreateArrayBuffer(byteLength);
        Assert(VarIs<ArrayBuffer>(newArr));
        if (byteLength > 0 && !VarTo<ArrayBuffer>(newArr)->GetByteLength())
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid);
        }
#if ENABLE_DEBUG_CONFIG_OPTIONS
        if (Js::Configuration::Global.flags.IsEnabled(Js::autoProxyFlag))
        {
            newArr = Js::JavascriptProxy::AutoProxyWrapper(newArr);
        }
#endif
        if (newTargetPrototype != nullptr)
        {
            JavascriptObject::ChangePrototype(newArr, newTargetPrototype, true, scriptContext);
        }
        return newArr;
    }

    // ArrayBuffer.prototype.byteLength as described in ES6 draft #20 section 24.1.4.1
    Var ArrayBuffer::EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        if (arrayBuffer->IsDetached())
        {
            return JavascriptNumber::ToVar(0, scriptContext);
        }
        return JavascriptNumber::ToVar(arrayBuffer->GetByteLength(), scriptContext);
    }

    // ArrayBuffer.prototype.detached getter.
    Var ArrayBuffer::EntryGetterDetached(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        return VarTo<ArrayBuffer>(args[0])->IsDetached() ? scriptContext->GetLibrary()->GetTrue() : scriptContext->GetLibrary()->GetFalse();
    }

    // ArrayBuffer.prototype.maxByteLength getter.
    Var ArrayBuffer::EntryGetterMaxByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        if (arrayBuffer->IsDetached())
        {
            return JavascriptNumber::ToVar(0, scriptContext);
        }
        return JavascriptNumber::ToVar(arrayBuffer->GetMaxByteLength(), scriptContext);
    }

    // ArrayBuffer.prototype.resizable getter. The slot is unaffected by detach.
    Var ArrayBuffer::EntryGetterResizable(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        return VarTo<ArrayBuffer>(args[0])->IsResizable() ? scriptContext->GetLibrary()->GetTrue() : scriptContext->GetLibrary()->GetFalse();
    }

    // ArrayBuffer.prototype.immutable getter. The slot is fixed for the
    // lifetime of an immutable buffer and is independent of detach state.
    Var ArrayBuffer::EntryGetterImmutable(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        return VarTo<ArrayBuffer>(args[0])->IsImmutable() ? scriptContext->GetLibrary()->GetTrue() : scriptContext->GetLibrary()->GetFalse();
    }

    void ArrayBuffer::Resize(uint32 newByteLength)
    {
        Assert(!this->isDetached);
        Assert(this->isResizable);
        Assert(newByteLength <= this->maxByteLength);
        Assert(newByteLength <= this->allocatedByteLength);

        uint32 oldByteLength = this->bufferLength;
        if (newByteLength > oldByteLength)
        {
            Assert(this->bufferContent != nullptr && this->bufferContent->GetBuffer() != nullptr);
            ZeroMemory(this->bufferContent->GetBuffer() + oldByteLength, newByteLength - oldByteLength);
        }
        this->bufferLength = newByteLength;

        // TypedArray views cache their effective length in the
        // ArrayObject length field (read directly by JIT inline caches
        // and bounds checks); refresh it for every live view.
        auto refreshView = [](ArrayBufferParent* parent)
        {
            if (parent != nullptr && TypedArrayBase::Is(JavascriptOperators::GetTypeId(parent)))
            {
                UnsafeVarTo<TypedArrayBase>(parent)->RefreshEffectiveLength();
            }
        };
        if (this->primaryParent != nullptr)
        {
            refreshView(this->primaryParent->Get());
        }
        if (this->otherParents != nullptr)
        {
            this->otherParents->Map([&](RecyclerWeakReference<ArrayBufferParent>* item)
            {
                refreshView(item->Get());
            });
        }
    }

    // ArrayBuffer.prototype.resize ( newLength )
    Var ArrayBuffer::EntryResize(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        if (arrayBuffer->IsImmutable() || !arrayBuffer->IsResizable())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        Var newLength = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        double newByteLength = JavascriptConversion::ToInteger(newLength, scriptContext);

        // test262 observes the detached check after argument coercion.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.resize"));
        }

        if (newByteLength < 0.0 || newByteLength > (double)arrayBuffer->GetMaxByteLength())
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        arrayBuffer->Resize((uint32)newByteLength);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    static void DetachAndFree(ArrayBuffer* arrayBuffer)
    {
        DetachedStateBase* state = arrayBuffer->DetachAndGetState(false /*queueForDelayFree*/);
        Assert(state != nullptr);
        state->CleanUp();
    }

    static ArrayBuffer* TransferBuffer(ArrayBuffer* source, Var newLength, bool preserveResizability, bool immutable, const char16* funcName, ScriptContext* scriptContext)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();

        double newByteLengthValue;
        if (JavascriptOperators::IsUndefined(newLength))
        {
            newByteLengthValue = (double)source->GetByteLength();
        }
        else
        {
            // ArrayBufferCopyAndDetach performs ToIndex(newLength) before the
            // detached/range checks. ToIndex throws for negative or >2^53-1
            // values here; the engine allocation limit is checked afterward.
            newByteLengthValue = ToIndexNoEngineCap(newLength, JSERR_ArrayLengthConstructIncorrect, scriptContext);
        }

        if (source->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, funcName);
        }

        // ArrayBufferCopyAndDetach step 6: an immutable buffer can never be
        // detached or transferred, independent of the requested length.
        if (source->IsImmutable())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        // WebAssembly.Memory buffers cannot be detached. This is the
        // [[ArrayBufferDetachKey]] check (step 7), which precedes the
        // engine's allocation-limit check and destination allocation.
        if (source->IsWebAssemblyArrayBuffer())
        {
            JavascriptError::ThrowTypeError(scriptContext, WASMERR_CantDetach);
        }

        if (newByteLengthValue > (double)ArrayBuffer::MaxArrayBufferLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }
        uint32 newByteLength = (uint32)newByteLengthValue;

        bool resizable = !immutable && preserveResizability && source->IsResizable();
        uint32 maxByteLength = resizable ? source->GetMaxByteLength() : newByteLength;
        if (resizable && newByteLength > maxByteLength)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        ArrayBuffer* destination = resizable
            ? library->CreateResizableArrayBuffer(newByteLength, maxByteLength)
            : library->CreateArrayBuffer(newByteLength);
        if (immutable)
        {
            destination->SetImmutable();
        }

        // Destination allocation can collect or run finalizers that detach
        // the source. Recheck before reading its length/bytes or calling
        // DetachAndFree, which asserts that the source is still attached.
        if (source->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.transfer"));
        }

        uint32 sourceLength = source->GetByteLength();
        uint32 copyLength = newByteLength < sourceLength ? newByteLength : sourceLength;
        if (copyLength > 0)
        {
            js_memcpy_s(destination->GetBuffer(), newByteLength, source->GetBuffer(), copyLength);
        }

        DetachAndFree(source);
        return destination;
    }

    // ArrayBuffer.prototype.transfer ( [ newLength ] )
    Var ArrayBuffer::EntryTransfer(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        Var newLength = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        return TransferBuffer(arrayBuffer, newLength, true, false, _u("ArrayBuffer.prototype.transfer"), scriptContext);
    }

    // ArrayBuffer.prototype.transferToImmutable ( [ newLength ] )
    Var ArrayBuffer::EntryTransferToImmutable(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        Var newLength = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        return TransferBuffer(arrayBuffer, newLength, false, true, _u("ArrayBuffer.prototype.transferToImmutable"), scriptContext);
    }

    // ArrayBuffer.prototype.transferToFixedLength ( [ newLength ] )
    Var ArrayBuffer::EntryTransferToFixedLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        Var newLength = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        return TransferBuffer(arrayBuffer, newLength, false, false, _u("ArrayBuffer.prototype.transferToFixedLength"), scriptContext);
    }

    // ArrayBuffer.isView as described in ES6 draft #20 section 24.1.3.1
    Var ArrayBuffer::EntryIsView(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);

        Assert(!(callInfo.Flags & CallFlags_New));

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        JavascriptLibrary* library = function->GetScriptContext()->GetLibrary();

        Var arg = library->GetUndefined();

        if (args.Info.Count > 1)
        {
            arg = args[1];
        }

        // Only DataView or any TypedArray objects have [[ViewedArrayBuffer]] internal slots
        if (VarIs<DataView>(arg) || VarIs<TypedArrayBase>(arg))
        {
            return library->GetTrue();
        }

        return library->GetFalse();
    }

#if ENABLE_DEBUG_CONFIG_OPTIONS
    Var ArrayBuffer::EntryDetach(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();

        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count < 2 || !VarIs<ArrayBuffer>(args[1]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[1]);

        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.detach"));
        }

        // DetachArrayBuffer step 2: an immutable buffer is never detachable.
        if (arrayBuffer->IsImmutable())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        // Discard the buffer
        // We are clearing out the buffer now instead of queueing to flush out JIT bugs.
        DetachedStateBase* state = arrayBuffer->DetachAndGetState(false /*queueForDelayFree*/);
        state->CleanUp();

        return scriptContext->GetLibrary()->GetUndefined();
    }
#endif

    // ArrayBuffer.prototype.slice as described in ES6 draft #19 section 24.1.4.3.
    Var ArrayBuffer::EntrySlice(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ScriptContext* scriptContext = function->GetScriptContext();

        PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Assert(!(callInfo.Flags & CallFlags_New));

        if (!VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        JavascriptLibrary* library = scriptContext->GetLibrary();
        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);

        if (arrayBuffer->IsDetached()) // 24.1.4.3: 5. If IsDetachedBuffer(O) is true, then throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.slice"));
        }

        int64 len = arrayBuffer->bufferLength;
        int64 start = 0, end = 0;
        int64 newLen;

        // If no start or end arguments, use the entire length
        if (args.Info.Count < 2)
        {
            newLen = len;
        }
        else
        {
            start = JavascriptArray::GetIndexFromVar(args[1], len, scriptContext);

            // If no end argument, use length as the end
            if (args.Info.Count < 3 || args[2] == library->GetUndefined())
            {
                end = len;
            }
            else
            {
                end = JavascriptArray::GetIndexFromVar(args[2], len, scriptContext);
            }

            newLen = end > start ? end - start : 0;
        }

        // We can't have allocated an ArrayBuffer with byteLength > MaxArrayBufferLength.
        // start and end are clamped to valid indices, so the new length also cannot exceed MaxArrayBufferLength.
        // Therefore, should be safe to cast down newLen to uint32.
        // TODO: If we ever support allocating ArrayBuffer with byteLength > MaxArrayBufferLength we may need to review this math.
        Assert(newLen <= MaxArrayBufferLength);
        uint32 byteLength = static_cast<uint32>(newLen);

        ArrayBuffer* newBuffer = nullptr;

        JavascriptFunction* defaultConstructor = scriptContext->GetLibrary()->GetArrayBufferConstructor();
        RecyclableObject* constructor = JavascriptOperators::SpeciesConstructor(arrayBuffer, defaultConstructor, scriptContext);
        AssertOrFailFast(JavascriptOperators::IsConstructor(constructor));

        bool isDefaultConstructor = constructor == defaultConstructor;
        Js::Var newVar = JavascriptOperators::NewObjectCreationHelper_ReentrancySafe(constructor, isDefaultConstructor, scriptContext->GetThreadContext(), [=]()->Js::Var
        {
            Js::Var constructorArgs[] = { constructor, JavascriptNumber::ToVar(byteLength, scriptContext) };
            Js::CallInfo constructorCallInfo(Js::CallFlags_New, _countof(constructorArgs));
            return JavascriptOperators::NewScObject(constructor, Js::Arguments(constructorCallInfo, constructorArgs), scriptContext);
        });

        if (!VarIs<ArrayBuffer>(newVar)) // 24.1.4.3: 19.If new does not have an [[ArrayBufferData]] internal slot throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        newBuffer = VarTo<ArrayBuffer>(newVar);

        if (newBuffer->IsDetached()) // 24.1.4.3: 21. If IsDetachedBuffer(new) is true, then throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.slice"));
        }

        // ArrayBuffer.prototype.slice step 23: the species result must not be
        // immutable because slice fills it after construction.
        if (newBuffer->IsImmutable())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        if (newBuffer == arrayBuffer) // 24.1.4.3: 22. If SameValue(new, O) is true, then throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        if (newBuffer->bufferLength < byteLength) // 24.1.4.3: 23.If the value of new's [[ArrayBufferByteLength]] internal slot < newLen, then throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ArgumentOutOfRange, _u("ArrayBuffer.prototype.slice"));
        }

        Assert(newBuffer);
        Assert(newBuffer->bufferLength >= byteLength);

        if (arrayBuffer->IsDetached()) // 24.1.4.3: 24. NOTE: Side-effects of the above steps may have detached O. 25. If IsDetachedBuffer(O) is true, then throw a TypeError exception.
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.slice"));
        }

        // Re-read the source length after start/end/species/constructor user
        // code. A resizable source may have shrunk (or grown) since the initial
        // snapshot; copy only what currently exists and zero-fill the rest.
        uint32 currentLength = arrayBuffer->GetByteLength();
        uint32 first = (uint32)start;
        uint32 copyLength = 0;
        if (first < currentLength)
        {
            uint32 available = currentLength - first;
            copyLength = byteLength < available ? byteLength : available;
        }

        if (copyLength > 0)
        {
            AssertMsg(arrayBuffer->GetBuffer() != nullptr, "buffer must not be null when we copy from it");
            js_memcpy_s(newBuffer->GetBuffer(), byteLength, arrayBuffer->GetBuffer() + first, copyLength);
        }
        // Do not touch bytes beyond copyLength: normal destinations are already
        // zero-initialized, and a species-returned buffer keeps its own bytes.

        return newBuffer;
    }

    namespace
    {
        // ResolveBounds (len, start, end) from the immutable-arraybuffer
        // proposal. ToIntegerOrInfinity clamps to len for positive values and
        // to zero for negative values below -len; NaN and -0 become zero.
        void ResolveArrayBufferBounds(Var start, Var end, uint32 len, ScriptContext* scriptContext, uint32* first, uint32* final)
        {
            double relativeStart = JavascriptConversion::ToInteger(start, scriptContext);
            double from;
            if (JavascriptNumber::IsNegInf(relativeStart))
            {
                from = 0;
            }
            else if (relativeStart < 0)
            {
                from = max((double)len + relativeStart, 0.0);
            }
            else
            {
                from = min(relativeStart, (double)len);
            }

            double relativeEnd;
            if (JavascriptOperators::IsUndefined(end))
            {
                relativeEnd = (double)len;
            }
            else
            {
                relativeEnd = JavascriptConversion::ToInteger(end, scriptContext);
            }

            double to;
            if (JavascriptNumber::IsNegInf(relativeEnd))
            {
                to = 0;
            }
            else if (relativeEnd < 0)
            {
                to = max((double)len + relativeEnd, 0.0);
            }
            else
            {
                to = min(relativeEnd, (double)len);
            }

            *first = (uint32)from;
            *final = (uint32)to;
        }

        ArrayBuffer* CreateImmutableArrayBuffer(uint32 length, ScriptContext* scriptContext)
        {
            ArrayBuffer* buffer = scriptContext->GetLibrary()->CreateArrayBuffer(length);
            buffer->SetImmutable();
            return buffer;
        }
    }

    // ArrayBuffer.prototype.sliceToImmutable ( start, end )
    Var ArrayBuffer::EntrySliceToImmutable(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<ArrayBuffer>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }

        ArrayBuffer* arrayBuffer = VarTo<ArrayBuffer>(args[0]);
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.sliceToImmutable"));
        }

        JavascriptLibrary* library = scriptContext->GetLibrary();
        Var start = args.Info.Count > 1 ? args[1] : library->GetUndefined();
        Var end = args.Info.Count > 2 ? args[2] : library->GetUndefined();
        uint32 len = arrayBuffer->GetByteLength();
        uint32 first = 0;
        uint32 final = 0;
        ResolveArrayBufferBounds(start, end, len, scriptContext, &first, &final);

        // Bounds coercion can detach or resize the receiver.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.sliceToImmutable"));
        }

        uint32 currentLength = arrayBuffer->GetByteLength();
        if (currentLength < final)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }

        uint32 newLength = final > first ? final - first : 0;
        ArrayBuffer* newBuffer = CreateImmutableArrayBuffer(newLength, scriptContext);

        // Allocating the destination can collect or run finalizers that
        // reenter and detach or resize the source. Revalidate and re-read the
        // live length before touching source bytes.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("ArrayBuffer.prototype.sliceToImmutable"));
        }
        currentLength = arrayBuffer->GetByteLength();
        if (currentLength < final)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_ArrayLengthConstructIncorrect);
        }
        uint32 copyLength = first < currentLength ? min(newLength, currentLength - first) : 0;
        if (copyLength > 0)
        {
            AssertMsg(arrayBuffer->GetBuffer() != nullptr, "buffer must not be null when we copy from it");
            js_memcpy_s(newBuffer->GetBuffer(), newLength, arrayBuffer->GetBuffer() + first, copyLength);
        }
        return newBuffer;
    }

    Var ArrayBuffer::EntryGetterSymbolSpecies(RecyclableObject* function, CallInfo callInfo, ...)
    {
        ARGUMENTS(args, callInfo);

        Assert(args.Info.Count > 0);

        return args[0];
    }

    ArrayBufferContentForDelayedFreeBase* ArrayBuffer::CopyBufferContentForDelayedFree(RefCountedBuffer * content, DECLSPEC_GUARD_OVERFLOW uint32 bufferLength)
    {
        Assert(content != nullptr);
        FreeFn freeFn = nullptr;
        if (this->externalized)
        {
            // The host owns the backing allocation; release only the wrapper.
            freeFn = NoOpFree;
        }
#if ENABLE_FAST_ARRAYBUFFER
        else if (!this->isResizable && IsValidVirtualBufferLength(bufferLength))
        {
            freeFn = FreeMemAlloc;
        }
#endif
        else
        {
            freeFn = free;
        }

        // This heap object will be deleted when the Recycler::DelayedFreeArrayBuffer determines to remove this item
        return HeapNew(ArrayBufferContentForDelayedFree<FreeFn>, content, bufferLength, GetScriptContext()->GetRecycler(), freeFn);
    }

    template <class Allocator>
    ArrayBuffer::ArrayBuffer(uint32 length, DynamicType * type, Allocator allocator) :
        ArrayBufferBase(type), bufferContent(nullptr), bufferLength(0),
        maxByteLength(0), allocatedByteLength(0), isResizable(false)
    {
        if (length > MaxArrayBufferLength)
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_FunctionArgument_Invalid);
        }
        else if (length > 0)
        {
            BYTE * buffer = nullptr;
            Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
            if (recycler->RequestExternalMemoryAllocation(length))
            {
                buffer = (BYTE*)allocator(length);
                if (buffer == nullptr)
                {
                    recycler->ReportExternalMemoryFree(length);
                }
            }

            if (buffer == nullptr)
            {
                recycler->CollectNow<CollectOnTypedArrayAllocation>();

                if (recycler->RequestExternalMemoryAllocation(length))
                {
                    buffer = (BYTE*)allocator(length);
                    if (buffer == nullptr)
                    {
                        recycler->ReportExternalMemoryFailure(length);
                    }
                }
            }

            if (buffer == nullptr)
            {
                JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
            }
            else
            {
                bufferLength = length;
                maxByteLength = length;
                allocatedByteLength = length;
                ZeroMemory(buffer, bufferLength);
                RefCountedBuffer* localContent = HeapNew(RefCountedBuffer, buffer);
                this->bufferContent = localContent;
            }
        }
    }

    template <class Allocator>
    ArrayBuffer::ArrayBuffer(uint32 length, uint32 maxByteLength, DynamicType * type, Allocator allocator) :
        ArrayBufferBase(type), bufferContent(nullptr), bufferLength(0),
        maxByteLength(maxByteLength), allocatedByteLength(0), isResizable(true)
    {
        if (maxByteLength > MaxArrayBufferLength)
        {
            JavascriptError::ThrowRangeError(GetScriptContext(), JSERR_ArrayLengthConstructIncorrect);
        }
        if (length > maxByteLength)
        {
            JavascriptError::ThrowRangeError(GetScriptContext(), JSERR_ArrayLengthConstructIncorrect);
        }

        if (maxByteLength > 0)
        {
            BYTE * buffer = nullptr;
            Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
            if (recycler->RequestExternalMemoryAllocation(maxByteLength))
            {
                buffer = (BYTE*)allocator(maxByteLength);
                if (buffer == nullptr)
                {
                    recycler->ReportExternalMemoryFree(maxByteLength);
                }
            }

            if (buffer == nullptr)
            {
                recycler->CollectNow<CollectOnTypedArrayAllocation>();

                if (recycler->RequestExternalMemoryAllocation(maxByteLength))
                {
                    buffer = (BYTE*)allocator(maxByteLength);
                    if (buffer == nullptr)
                    {
                        recycler->ReportExternalMemoryFailure(maxByteLength);
                    }
                }
            }

            if (buffer == nullptr)
            {
                JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
            }

            // The whole data block (capacity) is zero-initialized; growing
            // later only exposes bytes inside this stable allocation.
            ZeroMemory(buffer, maxByteLength);
            this->bufferContent = HeapNew(RefCountedBuffer, buffer);
            this->allocatedByteLength = maxByteLength;
        }

        this->bufferLength = length;
    }

    ArrayBuffer::ArrayBuffer(RefCountedBuffer* buffContent, uint32 length, DynamicType * type)
       : ArrayBufferBase(type), bufferContent(nullptr), bufferLength(length),
       maxByteLength(length), allocatedByteLength(length), isResizable(false)
    {
        if (length > MaxArrayBufferLength)
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_FunctionArgument_Invalid);
        }

        // we take the ownership of the buffer and will have to free it so charge it to our quota.
        if (!this->GetRecycler()->RequestExternalMemoryAllocation(length))
        {
            JavascriptError::ThrowOutOfMemoryError(this->GetScriptContext());
        }

        this->bufferContent = buffContent;

        // The bufferContent can be null as might have detached an ArrayBuffer which does not have bufferContent.
        if (this->bufferContent != nullptr)
        {
            this->bufferContent->AddRef();
        }
    }

    ArrayBuffer::ArrayBuffer(byte* buffer, uint32 length, DynamicType * type, bool isExternal) :
        ArrayBufferBase(type), bufferContent(nullptr), bufferLength(length),
        maxByteLength(length), allocatedByteLength(length), isResizable(false)
    {
        if (length > MaxArrayBufferLength)
        {
            JavascriptError::ThrowTypeError(GetScriptContext(), JSERR_FunctionArgument_Invalid);
        }

        if (!isExternal)
        {
            // we take the ownership of the buffer and will have to free it so charge it to our quota.
            if (!this->GetRecycler()->RequestExternalMemoryAllocation(length))
            {
                JavascriptError::ThrowOutOfMemoryError(this->GetScriptContext());
            }
        }

        if (buffer != nullptr)
        {
            RefCountedBuffer* localContent = HeapNew(RefCountedBuffer, buffer);
            this->bufferContent = localContent;
        }
    }

    BOOL ArrayBuffer::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("Object, (ArrayBuffer)"));
        return TRUE;
    }

    BOOL ArrayBuffer::GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("[object ArrayBuffer]"));
        return TRUE;
    }

    void ArrayBuffer::ReleaseBufferContent()
    {
        if (this->bufferContent != nullptr && this->bufferContent->GetBuffer() != nullptr)
        {
            RefCountedBuffer *content = this->bufferContent;
            this->bufferContent = nullptr;
            long refCount = content->Release();
            AssertOrFailFast(refCount == 0);
            HeapDelete(content);
        }
    }

#if ENABLE_TTD
    void ArrayBufferParent::MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor)
    {
        extractor->MarkVisitVar(this->arrayBuffer);
    }

    void ArrayBufferParent::ProcessCorePaths()
    {
        this->GetScriptContext()->TTDWellKnownInfo->EnqueueNewPathVarAsNeeded(this, this->arrayBuffer, _u("!buffer"));
    }
#endif

    JavascriptArrayBuffer::JavascriptArrayBuffer(uint32 length, DynamicType * type) :
        ArrayBuffer(length, type, IsValidVirtualBufferLength(length) ? AsmJsVirtualAllocator : malloc)
    {
    }
    JavascriptArrayBuffer::JavascriptArrayBuffer(uint32 length, uint32 maxByteLength, DynamicType * type) :
        ArrayBuffer(length, maxByteLength, type, malloc)
    {
    }
    JavascriptArrayBuffer::JavascriptArrayBuffer(byte* buffer, uint32 length, DynamicType * type) :
        ArrayBuffer(buffer, length, type)
    {
    }

    JavascriptArrayBuffer::JavascriptArrayBuffer(RefCountedBuffer* buffer, uint32 length, DynamicType * type) :
        ArrayBuffer(buffer, length, type)
    {
    }


    JavascriptArrayBuffer::JavascriptArrayBuffer(DynamicType * type) : ArrayBuffer(0, type, malloc)
    {
    }

    JavascriptArrayBuffer* JavascriptArrayBuffer::Create(uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptArrayBuffer, length, type);
        Assert(result);
        recycler->AddExternalMemoryUsage(length);
        return result;
    }

    JavascriptArrayBuffer* JavascriptArrayBuffer::Create(uint32 length, uint32 maxByteLength, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptArrayBuffer, length, maxByteLength, type);
        Assert(result);
        recycler->AddExternalMemoryUsage(maxByteLength);
        return result;
    }

    JavascriptArrayBuffer* JavascriptArrayBuffer::Create(byte* buffer, uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptArrayBuffer, buffer, length, type);
        Assert(result);
        recycler->AddExternalMemoryUsage(length);
        return result;
    }

    JavascriptArrayBuffer* JavascriptArrayBuffer::Create(RefCountedBuffer* content, uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        JavascriptArrayBuffer* result = RecyclerNewFinalized(recycler, JavascriptArrayBuffer, content, length, type);
        Assert(result);
        recycler->AddExternalMemoryUsage(length);
        return result;
    }

    bool JavascriptArrayBuffer::UsesVirtualAllocation() const
    {
#if ENABLE_FAST_ARRAYBUFFER
        return !this->isResizable && IsValidVirtualBufferLength(this->allocatedByteLength);
#else
        return false;
#endif
    }

    ArrayBuffer::FreeFn JavascriptArrayBuffer::GetArrayBufferFreeFn()
    {
        return UsesVirtualAllocation() ? FreeMemAlloc : free;
    }

    ArrayBufferDetachedStateBase* JavascriptArrayBuffer::CreateDetachedState(RefCountedBuffer * content, uint32 bufferLength)
    {
        FreeFn freeFn = nullptr;
        ArrayBufferAllocationType allocationType;
        if (this->externalized)
        {
            // JsExternalizeArrayBuffer handed the bytes to the host. Detach or
            // transfer must not free them; the host's free function owns them.
            allocationType = ArrayBufferAllocationType::Heap;
            freeFn = NoOpFree;
        }
        else if (UsesVirtualAllocation())
        {
            allocationType = ArrayBufferAllocationType::MemAlloc;
            freeFn = FreeMemAlloc;
        }
        else
        {
            allocationType = ArrayBufferAllocationType::Heap;
            freeFn = free;
        }
        return HeapNew(ArrayBufferDetachedState<FreeFn>, content, bufferLength, freeFn, GetScriptContext()->GetRecycler(), allocationType);
    }


    bool JavascriptArrayBuffer::IsValidAsmJsBufferLengthAlgo(uint length, bool forceCheck)
    {
        /*
        1. length >= 2^16
        2. length is power of 2 or (length > 2^24 and length is multiple of 2^24)
        3. length is a multiple of 4K
        */
        const bool isLongEnough = length >= 0x10000;
        const bool isPow2 = ::Math::IsPow2(length);
        // No need to check for length > 2^24, because it already has to be non zero length
        const bool isMultipleOf2e24 = (length & 0xFFFFFF) == 0;
        const bool isPageSizeMultiple = (length % AutoSystemInfo::PageSize) == 0;
        return (
#ifndef ENABLE_FAST_ARRAYBUFFER
            forceCheck &&
#endif
            isLongEnough &&
            (isPow2 || isMultipleOf2e24) &&
            isPageSizeMultiple
        );
    }

    bool JavascriptArrayBuffer::IsValidAsmJsBufferLength(uint length, bool forceCheck)
    {
        return IsValidAsmJsBufferLengthAlgo(length, forceCheck);
    }

    bool JavascriptArrayBuffer::IsValidVirtualBufferLength(uint length) const
    {
#if ENABLE_FAST_ARRAYBUFFER
        return PHASE_FORCE1(Js::TypedArrayVirtualPhase) || (!PHASE_OFF1(Js::TypedArrayVirtualPhase) && IsValidAsmJsBufferLengthAlgo(length, true));
#else
        return false;
#endif
    }

    void JavascriptArrayBuffer::Finalize(bool isShutdown)
    {
        if (this->bufferContent == nullptr)
        {
            return;
        }

        RefCountedBuffer *content = this->bufferContent;
        this->bufferContent = nullptr;
        long refCount = content->Release();
        if (refCount == 0)
        {
            BYTE * buffer = content->GetBuffer();
            if (buffer && !this->externalized)
            {
                // Recycler may not be available at Dispose. We need to
                // free the memory and report that it has been freed at the same
                // time. Otherwise, AllocationPolicyManager is unable to provide correct feedback
                if (UsesVirtualAllocation())
                {
                    FreeMemAlloc(buffer);
                }
                else
                {
                    free(buffer);
                }
            }
            Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
            recycler->ReportExternalMemoryFree(allocatedByteLength);
            HeapDelete(content);
        }

        bufferLength = 0;
        allocatedByteLength = 0;
    }

    void JavascriptArrayBuffer::Dispose(bool isShutdown)
    {
        /* See JavascriptArrayBuffer::Finalize */
    }

#if ENABLE_TTD
    TTD::NSSnapObjects::SnapObjectType JavascriptArrayBuffer::GetSnapTag_TTD() const
    {
        return TTD::NSSnapObjects::SnapObjectType::SnapArrayBufferObject;
    }

    void JavascriptArrayBuffer::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
    {
        TTD::NSSnapObjects::SnapArrayBufferInfo* sabi = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapArrayBufferInfo>();

        sabi->Length = this->GetByteLength();
        if (sabi->Length == 0)
        {
            sabi->Buff = nullptr;
        }
        else
        {
            sabi->Buff = alloc.SlabAllocateArray<byte>(sabi->Length);
            memcpy(sabi->Buff, this->GetBuffer(), sabi->Length);
        }

        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapArrayBufferInfo*, TTD::NSSnapObjects::SnapObjectType::SnapArrayBufferObject>(objData, sabi);
    }
#endif

#ifdef ENABLE_WASM
    // Same as realloc but zero newly allocated portion if newSize > oldSize
    static BYTE* ReallocZero(BYTE* ptr, size_t oldSize, size_t newSize)
    {
        BYTE* ptrNew = (BYTE*)realloc(ptr, newSize);
        if (ptrNew && newSize > oldSize)
        {
            ZeroMemory(ptrNew + oldSize, newSize - oldSize);
        }
        return ptrNew;
    }

    template<typename Allocator>
    Js::WebAssemblyArrayBuffer::WebAssemblyArrayBuffer(uint32 length, DynamicType * type, Allocator allocator):
        JavascriptArrayBuffer(length, type, allocator)
    {
#ifndef ENABLE_FAST_ARRAYBUFFER
        CompileAssert(UNREACHED);
#endif
        Assert(allocator == WasmVirtualAllocator);
        // Make sure we always have a buffer even if the length is 0
        if (bufferContent == nullptr && length == 0)
        {
            // We want to allocate an empty buffer using virtual memory
            BYTE *buffer = (BYTE*)allocator(0);
            if (buffer == nullptr)
            {
                JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
            }

            RefCountedBuffer* localContent = HeapNew(RefCountedBuffer, buffer);
            this->bufferContent = localContent;
        }
        if (bufferContent == nullptr)
        {
            JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
        }
    }

    // Treat as a normal JavascriptArrayBuffer
    WebAssemblyArrayBuffer::WebAssemblyArrayBuffer(uint32 length, DynamicType * type) :
        JavascriptArrayBuffer(length, type, malloc)
    {
        // Make sure we always have a bufferContent even if the length is 0
        if (bufferContent == nullptr && length == 0)
        {
            bufferContent = HeapNew(RefCountedBuffer, nullptr);
        }
        if (bufferContent == nullptr)
        {
            JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
        }
    }

    WebAssemblyArrayBuffer::WebAssemblyArrayBuffer(byte* buffer, uint32 length, DynamicType * type):
        JavascriptArrayBuffer(buffer, length, type)
    {
#if ENABLE_FAST_ARRAYBUFFER
        if (CONFIG_FLAG(WasmFastArray) && buffer == nullptr)
        {
            JavascriptError::ThrowOutOfMemoryError(GetScriptContext());
        }
#endif
    }

    WebAssemblyArrayBuffer* WebAssemblyArrayBuffer::Create(byte* buffer, uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        WebAssemblyArrayBuffer* result = nullptr;
        if (buffer)
        {
            result = RecyclerNewFinalized(recycler, WebAssemblyArrayBuffer, buffer, length, type);
        }
        else
        {
#if ENABLE_FAST_ARRAYBUFFER
            if (CONFIG_FLAG(WasmFastArray))
            {
                result = RecyclerNewFinalized(recycler, WebAssemblyArrayBuffer, length, type, WasmVirtualAllocator);
            }
            else
#endif
            {
                result = RecyclerNewFinalized(recycler, WebAssemblyArrayBuffer, length, type);
            }
        }

        recycler->AddExternalMemoryUsage(length);
        Assert(result);
        return result;
    }

    bool WebAssemblyArrayBuffer::IsValidVirtualBufferLength(uint length) const
    {
#if ENABLE_FAST_ARRAYBUFFER
        return CONFIG_FLAG(WasmFastArray);
#else
        return false;
#endif
    }

    ArrayBufferDetachedStateBase* WebAssemblyArrayBuffer::CreateDetachedState(RefCountedBuffer* buffer, uint32 bufferLength)
    {
        JavascriptError::ThrowTypeError(GetScriptContext(), WASMERR_CantDetach);
    }

    WebAssemblyArrayBuffer* WebAssemblyArrayBuffer::GrowMemory(uint32 newBufferLength)
    {
        if (newBufferLength < this->bufferLength)
        {
            Assert(UNREACHED);
            JavascriptError::ThrowTypeError(GetScriptContext(), WASMERR_BufferGrowOnly);
        }

        uint32 growSize = newBufferLength - this->bufferLength;
        const auto finalizeGrowMemory = [&](WebAssemblyArrayBuffer* newArrayBuffer)
        {
            AssertOrFailFast(newArrayBuffer && newArrayBuffer->GetByteLength() == newBufferLength);
            RefCountedBuffer *local = this->GetBufferContent();
            // Detach the buffer from this ArrayBuffer
            this->Detach();
            if (local != nullptr)
            {
                HeapDelete(local);
            }
            return newArrayBuffer;
        };

        // We're not growing the buffer, just create a new WebAssemblyArrayBuffer and detach this
        if (growSize == 0)
        {
            return finalizeGrowMemory(this->GetLibrary()->CreateWebAssemblyArrayBuffer(this->GetBuffer(), this->bufferLength));
        }

#if ENABLE_FAST_ARRAYBUFFER
        // 8Gb Array case
        if (CONFIG_FLAG(WasmFastArray))
        {
            AssertOrFailFast(this->GetBuffer());
            const auto virtualAllocFunc = [&]
            {
                return !!VirtualAlloc(this->GetBuffer() + this->bufferLength, growSize, MEM_COMMIT, PAGE_READWRITE);
            };
            if (!this->GetRecycler()->DoExternalAllocation(growSize, virtualAllocFunc))
            {
                return nullptr;
            }

            // We are transferring the buffer to the new owner.
            // To avoid double-charge to the allocation quota we will free the "diff" amount here.
            this->GetRecycler()->ReportExternalMemoryFree(growSize);

            return finalizeGrowMemory(this->GetLibrary()->CreateWebAssemblyArrayBuffer(this->GetBuffer(), newBufferLength));
        }
#endif

        // No previous buffer case
        if (this->GetByteLength() == 0)
        {
            Assert(newBufferLength == growSize);
            // Creating a new buffer will do the external memory allocation report
            return finalizeGrowMemory(this->GetLibrary()->CreateWebAssemblyArrayBuffer(newBufferLength));
        }

        // Regular growing case
        {
            // Disable Interrupts while doing a ReAlloc to minimize chances to end up in a bad state
            AutoDisableInterrupt autoDisableInterrupt(this->GetScriptContext()->GetThreadContext(), false);

            byte* newBuffer = nullptr;
            const auto reallocFunc = [&]
            {
                newBuffer = ReallocZero(this->GetBuffer(), this->bufferLength, newBufferLength);
                if (newBuffer != nullptr)
                {
                    // Realloc freed this->buffer
                    // if anything goes wrong before we detach, we can't recover the state and should failfast
                    autoDisableInterrupt.RequireExplicitCompletion();
                }
                return !!newBuffer;
            };

            if (!this->GetRecycler()->DoExternalAllocation(growSize, reallocFunc))
            {
                return nullptr;
            }

            // We are transferring the buffer to the new owner.
            // To avoid double-charge to the allocation quota we will free the "diff" amount here.
            this->GetRecycler()->ReportExternalMemoryFree(growSize);

            WebAssemblyArrayBuffer* newArrayBuffer = finalizeGrowMemory(this->GetLibrary()->CreateWebAssemblyArrayBuffer(newBuffer, newBufferLength));
            // We've successfully Detached this buffer and created a new WebAssemblyArrayBuffer
            autoDisableInterrupt.Completed();
            return newArrayBuffer;
        }
    }
#endif

    ProjectionArrayBuffer::ProjectionArrayBuffer(uint32 length, DynamicType * type) :
        ArrayBuffer(length, type, CoTaskMemAlloc)
    {
    }

    ProjectionArrayBuffer::ProjectionArrayBuffer(byte* buffer, uint32 length, DynamicType * type) :
        ArrayBuffer(buffer, length, type)
    {
    }

    ProjectionArrayBuffer::ProjectionArrayBuffer(RefCountedBuffer* buffer, uint32 length, DynamicType * type) :
        ArrayBuffer(buffer, length, type)
    {
    }

    ProjectionArrayBuffer* ProjectionArrayBuffer::Create(uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        recycler->AddExternalMemoryUsage(length);
        return RecyclerNewFinalized(recycler, ProjectionArrayBuffer, length, type);
    }

    ProjectionArrayBuffer* ProjectionArrayBuffer::Create(byte* buffer, uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        ProjectionArrayBuffer* result = RecyclerNewFinalized(recycler, ProjectionArrayBuffer, buffer, length, type);
        // This is user passed [in] buffer, user should AddExternalMemoryUsage before calling jscript, but
        // I don't see we ask everyone to do this. Let's add the memory pressure here as well.
        recycler->AddExternalMemoryUsage(length);
        return result;

    }

    ProjectionArrayBuffer* ProjectionArrayBuffer::Create(RefCountedBuffer* buffer, uint32 length, DynamicType * type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();

        ProjectionArrayBuffer* result = RecyclerNewFinalized(recycler, ProjectionArrayBuffer, buffer, length, type);
        // This is user passed [in] buffer, user should AddExternalMemoryUsage before calling jscript, but
        // I don't see we ask everyone to do this. Let's add the memory pressure here as well.
        recycler->AddExternalMemoryUsage(length);
        return result;
    }

    void ProjectionArrayBuffer::Finalize(bool isShutdown)
    {
        if (this->bufferContent == nullptr || this->bufferContent->GetBuffer() == nullptr)
        {
            return;
        }

        RefCountedBuffer *content = this->bufferContent;
        this->bufferContent = nullptr;
        long refCount = content->Release();
        if (refCount == 0)
        {
            CoTaskMemFree(content->GetBuffer());
            // Recycler may not be available at Dispose. We need to
            // free the memory and report that it has been freed at the same
            // time. Otherwise, AllocationPolicyManager is unable to provide correct feedback
            Recycler* recycler = GetType()->GetLibrary()->GetRecycler();
            recycler->ReportExternalMemoryFree(allocatedByteLength);
            HeapDelete(content);
        }
    }

    void ProjectionArrayBuffer::Dispose(bool isShutdown)
    {
        /* See ProjectionArrayBuffer::Finalize */
    }

    ArrayBufferContentForDelayedFreeBase* ProjectionArrayBuffer::CopyBufferContentForDelayedFree(RefCountedBuffer * content, DECLSPEC_GUARD_OVERFLOW uint32 bufferLength)
    {
        // This heap object will be deleted when the Recycler::DelayedFreeArrayBuffer determines to remove this item
        return HeapNew(ArrayBufferContentForDelayedFree<FreeFn>, content, bufferLength, GetRecycler(), CoTaskMemFree);
    }

    ArrayBuffer* ExternalArrayBufferDetachedState::Create(JavascriptLibrary* library)
    {
        return library->CreateExternalArrayBuffer(buffer, bufferLength);
    }

    ExternalArrayBuffer::ExternalArrayBuffer(byte *buffer, uint32 length, DynamicType *type)
        : ArrayBuffer(buffer, length, type, true)
    {
    }

    ExternalArrayBuffer::ExternalArrayBuffer(RefCountedBuffer *buffer, uint32 length, DynamicType *type)
        : ArrayBuffer(buffer, length, type)
    {
    }

    ExternalArrayBuffer* ExternalArrayBuffer::Create(RefCountedBuffer* buffer, uint32 length, DynamicType * type)
    {
        // This type does not own the external memory, so don't AddExternalMemoryUsage like other ArrayBuffer types do
        return RecyclerNewFinalized(type->GetScriptContext()->GetRecycler(), ExternalArrayBuffer, buffer, length, type);
    }

    ArrayBufferDetachedStateBase* ExternalArrayBuffer::CreateDetachedState(RefCountedBuffer* buffer, DECLSPEC_GUARD_OVERFLOW uint32 bufferLength)
    {
        return HeapNew(ExternalArrayBufferDetachedState, buffer, bufferLength);
    };

    void ExternalArrayBuffer::ReportExternalMemoryFree()
    {
        // This type does not own the external memory, so don't ReportExternalMemoryFree like other ArrayBuffer types do
    }

#if ENABLE_TTD
    TTD::NSSnapObjects::SnapObjectType ExternalArrayBuffer::GetSnapTag_TTD() const
    {
        //We re-map ExternalArrayBuffers to regular buffers since the 'real' host will be gone when we replay
        return TTD::NSSnapObjects::SnapObjectType::SnapArrayBufferObject;
    }

    void ExternalArrayBuffer::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
    {
        TTD::NSSnapObjects::SnapArrayBufferInfo* sabi = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapArrayBufferInfo>();

        sabi->Length = this->GetByteLength();
        if(sabi->Length == 0)
        {
            sabi->Buff = nullptr;
        }
        else
        {
            sabi->Buff = alloc.SlabAllocateArray<byte>(sabi->Length);
            memcpy(sabi->Buff, this->GetBuffer(), sabi->Length);
        }

        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapArrayBufferInfo*, TTD::NSSnapObjects::SnapObjectType::SnapArrayBufferObject>(objData, sabi);
    }
#endif

    ExternalArrayBufferDetachedState::ExternalArrayBufferDetachedState(RefCountedBuffer* buffer, uint32 bufferLength)
        : ArrayBufferDetachedStateBase(TypeIds_ArrayBuffer, buffer, bufferLength, ArrayBufferAllocationType::External)
    {}

    void ExternalArrayBufferDetachedState::ClearSelfOnly()
    {
        HeapDelete(this);
    }

    void __cdecl NoOpFree(void* data) { }

    void ExternalArrayBufferDetachedState::DiscardState()
    {
        // Don't actually free the data as it's externally managed, but do the
        // appropriate cleanup for our RefCountedBuffer.
        DiscardStateBase(NoOpFree);
    }

    void ExternalArrayBufferDetachedState::Discard()
    {
        ClearSelfOnly();
    }
}
