//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "JsrtPch.h"
#include "JsrtExternalArrayBuffer.h"

namespace Js
{
    JsrtExternalArrayBuffer::JsrtExternalArrayBuffer(byte *buffer, uint32 length, JsFinalizeCallback finalizeCallback, void *callbackState, DynamicType *type)
        : ExternalArrayBuffer(buffer, length, type), finalizeCallback(finalizeCallback), callbackState(callbackState)
    {
    }

    JsrtExternalArrayBuffer::JsrtExternalArrayBuffer(RefCountedBuffer *buffer, uint32 length, JsFinalizeCallback finalizeCallback, void *callbackState, DynamicType *type)
        : ExternalArrayBuffer(buffer, length, type), finalizeCallback(finalizeCallback), callbackState(callbackState)
    {
    }

    JsrtExternalArrayBuffer* JsrtExternalArrayBuffer::New(byte *buffer, uint32 length, JsFinalizeCallback finalizeCallback, void *callbackState, DynamicType *type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        return RecyclerNewFinalized(recycler, JsrtExternalArrayBuffer, buffer, length, finalizeCallback, callbackState, type);
    }

    JsrtExternalArrayBuffer* JsrtExternalArrayBuffer::New(RefCountedBuffer *buffer, uint32 length, JsFinalizeCallback finalizeCallback, void *callbackState, DynamicType *type)
    {
        Recycler* recycler = type->GetScriptContext()->GetRecycler();
        return RecyclerNewFinalized(recycler, JsrtExternalArrayBuffer, buffer, length, finalizeCallback, callbackState, type);
    }

    // Ownership record (doc/memory-management.md section 3):
    //   Resource: host (finalizeCallback, callbackState) pair plus backing
    //     RefCountedBuffer wrapper created by JsrtExternalArrayBuffer::New
    //     (via JsCreateExternalArrayBuffer).
    //   Domain: Recycler-managed JsrtExternalArrayBuffer object; C++ heap
    //     (HeapNew/HeapDelete) for RefCountedBuffer and
    //     JsrtExternalArrayBufferDetachedState; host-owned backing bytes
    //     released only through finalizeCallback(callbackState).
    //   Owner & transfer points (strict single-owner invariant):
    //     1. Attached or directly-detached (JsDetachArrayBuffer ->
    //        ArrayBuffer::Detach()): this JsrtExternalArrayBuffer owns
    //        (finalizeCallback, callbackState) until Finalize(isShutdown) runs
    //        on GC collection or JsDisposeRuntime. Finalize always clears the
    //        slot before invoking the callback so it fires at most once.
    //     2. Transfer / detach-with-state (CreateDetachedState): transfers
    //        (finalizeCallback, callbackState) into the returned
    //        JsrtExternalArrayBufferDetachedState and immediately clears the
    //        source object's fields to nullptr so Finalize on the source
    //        wrapper never double-invokes the finalizer. (If HeapNew throws
    //        OOM, ownership stays on the source wrapper.)
    //     3. Claimed vs discarded detached state:
    //        - Create(library) transfers (finalizeCallback, callbackState)
    //          to the new JsrtExternalArrayBuffer and clears them on the
    //          detached state.
    //        - DiscardState() invokes and clears any remaining finalizer
    //          (both for non-null buffer via DiscardStateBase and for a
    //          0-byte null-buffer external ArrayBuffer).
    //   Thread/context: Finalize/DiscardState run under the Recycler/runtime
    //     protocol; finalizeCallback must not reenter JS or touch unrelated
    //     JsRT state without host coordination.
    void JsrtExternalArrayBuffer::Finalize(bool isShutdown)
    {
        ReleaseBufferContent();

        if (finalizeCallback != nullptr)
        {
            JsFinalizeCallback cb = finalizeCallback;
            void* state = callbackState;
            finalizeCallback = nullptr;
            callbackState = nullptr;
            cb(state);
        }
    }

    ArrayBufferDetachedStateBase* JsrtExternalArrayBuffer::CreateDetachedState(RefCountedBuffer* buffer, DECLSPEC_GUARD_OVERFLOW uint32 bufferLength)
    {
        ArrayBufferDetachedStateBase* detachedState = HeapNew(JsrtExternalArrayBufferDetachedState, buffer, bufferLength, finalizeCallback, callbackState);
        this->finalizeCallback = nullptr;
        this->callbackState = nullptr;
        return detachedState;
    };

    JsrtExternalArrayBuffer::JsrtExternalArrayBufferDetachedState::JsrtExternalArrayBufferDetachedState(
        RefCountedBuffer* buffer, uint32 bufferLength, JsFinalizeCallback finalizeCallback, void *callbackState)
        : ExternalArrayBufferDetachedState(buffer, bufferLength), finalizeCallback(finalizeCallback), callbackState(callbackState)
    {}

    void JsrtExternalArrayBuffer::JsrtExternalArrayBufferDetachedState::ClearSelfOnly()
    {
        HeapDelete(this);
    }

    void JsrtExternalArrayBuffer::JsrtExternalArrayBufferDetachedState::DiscardState()
    {
        DiscardStateBase([&](byte* data)
        {
            if (finalizeCallback != nullptr)
            {
                JsFinalizeCallback cb = finalizeCallback;
                void* state = callbackState;
                finalizeCallback = nullptr;
                callbackState = nullptr;
                cb(state);
            }
        });

        // DiscardStateBase skips freeFunction when buffer == nullptr (e.g., a
        // 0-length external buffer created with nullptr data). Invoke and clear
        // the remaining finalizer so ownership never leaks on unclaimed discard.
        if (finalizeCallback != nullptr)
        {
            JsFinalizeCallback cb = finalizeCallback;
            void* state = callbackState;
            finalizeCallback = nullptr;
            callbackState = nullptr;
            cb(state);
        }
    }

    ArrayBuffer* JsrtExternalArrayBuffer::JsrtExternalArrayBufferDetachedState::Create(JavascriptLibrary* library)
    {
        ArrayBuffer* arr = JsrtExternalArrayBuffer::New(buffer, bufferLength, finalizeCallback, callbackState, library->GetArrayBufferType());
        finalizeCallback = nullptr;
        callbackState = nullptr;
        JS_ETW(EventWriteJSCRIPT_RECYCLER_ALLOCATE_OBJECT(arr));
        return arr;
    }
}
