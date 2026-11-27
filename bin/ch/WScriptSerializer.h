//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// Ownership record for WScript.SerializeObject / WScript.Deserialize (GR-FE3):
// - SerializerBlob (C++ heap) owns `data` (libc malloc/realloc/free, transferred
//   from SerializeBufferState after JsVarSerializerReleaseData) and any
//   externalized-and-detached `transferableArrays` (each freed via its paired
//   ArrayBufferFreeFn if unconsumed).
// - SerializeObject allocates an opaque 8-byte token buffer (libc calloc/free)
//   for the returned external ArrayBuffer and registers `tokenBuf -> blob` in
//   SerializerRegistry() under HostGlobalsLock. Script-visible bytes hold no
//   pointers or metadata.
// - Sole release authority for a registered SerializerBlob is either:
//   (1) WScript.Deserialize: looks up `data` (checked for length == sizeof(uint64_t)),
//       erases the registry entry under HostGlobalsLock, takes sole ownership
//       into a local unique_ptr, frees the deserializer handle before the blob,
//       and deletes the blob on both success and failure (preventing replay/UAF).
//   (2) SerializerTokenFinalizer: runs when the external ArrayBuffer is GC'd or
//       the runtime is disposed; erases and deletes any unconsumed blob under
//       HostGlobalsLock, then frees `tokenBuf`.
struct ArrayBufferTransferInfo
{
    byte* buffer;
    uint length;
    ArrayBufferFreeFn freeFn;
};

struct SerializerBlob
{
    void *data = nullptr;
    size_t dataLength = 0;
    std::vector<ArrayBufferTransferInfo> transferableArrays;

    ~SerializerBlob()
    {
        if (data != nullptr)
        {
            free(data);
            data = nullptr;
        }
        for (size_t i = 0; i < transferableArrays.size(); ++i)
        {
            if (transferableArrays[i].freeFn != nullptr && transferableArrays[i].buffer != nullptr)
            {
                transferableArrays[i].freeFn(transferableArrays[i].buffer);
            }
        }
        transferableArrays.clear();
    }
};

struct SerializeBufferState
{
    byte *buffer = nullptr;

    ~SerializeBufferState()
    {
        if (buffer != nullptr)
        {
            free(buffer);
            buffer = nullptr;
        }
    }
};

struct VarSerializerGuard
{
    JsVarSerializerHandle handle = nullptr;

    ~VarSerializerGuard()
    {
        if (handle != nullptr)
        {
            ChakraRTInterface::JsVarSerializerFree(handle);
            handle = nullptr;
        }
    }
};

struct VarDeserializerGuard
{
    JsVarDeserializerHandle handle = nullptr;

    ~VarDeserializerGuard()
    {
        if (handle != nullptr)
        {
            ChakraRTInterface::JsVarDeserializerFree(handle);
            handle = nullptr;
        }
    }
};

// Host-owned registry mapping external ArrayBuffer token storage pointers to
// live, unconsumed SerializerBlob records. Intentionally leaked on process exit
// like HostGlobalsCS() so teardown never depends on static destructor order;
// JsDisposeRuntime finalizes live external ArrayBuffers and clears entries.
static std::unordered_map<void*, SerializerBlob*>& SerializerRegistry()
{
    static std::unordered_map<void*, SerializerBlob*>* s_registry =
        new std::unordered_map<void*, SerializerBlob*>();
    return *s_registry;
}

static void CHAKRA_CALLBACK SerializerTokenFinalizer(void *state)
{
    if (state == nullptr)
    {
        return;
    }

    SerializerBlob *unconsumedBlob = nullptr;
    {
        HostGlobalsLock lock;
        auto& registry = SerializerRegistry();
        auto it = registry.find(state);
        if (it != registry.end())
        {
            unconsumedBlob = it->second;
            registry.erase(it);
        }
    }

    delete unconsumedBlob;
    free(state);
}

static byte * CHAKRA_CALLBACK ReallocateBufferMemory(void * state, byte *oldBuffer, size_t newSize, size_t *allocatedSize)
{
    size_t allocSize = (newSize == 0) ? 1 : newSize;
    void* data = realloc((void*)oldBuffer, allocSize);
    if (data == nullptr)
    {
        // Keep oldBuffer tracked in state so cleanup frees it instead of leaking.
        if (state != nullptr)
        {
            static_cast<SerializeBufferState*>(state)->buffer = oldBuffer;
        }
        return nullptr;
    }
    if (state != nullptr)
    {
        static_cast<SerializeBufferState*>(state)->buffer = static_cast<byte*>(data);
    }
    if (allocatedSize != nullptr)
    {
        *allocatedSize = allocSize;
    }
    return static_cast<byte*>(data);
}

static bool CHAKRA_CALLBACK WriteHostObject(void * state, JsValueRef data)
{
    // Not implemented
    return true;
}

JsValueRef __stdcall WScriptJsrt::SerializeObject(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    static const size_t kMaxTransferListLength = 65536;

    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("");
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    HRESULT hr = S_OK;
    std::vector<JsValueRef> transferVars;
    SerializeBufferState bufferState;
    std::unique_ptr<SerializerBlob> blob;
    VarSerializerGuard serializerGuard;
    void *tokenBuf = nullptr;
    bool tokenRegistered = false;

    if (argumentCount < 2)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Need an argument for WScript.Serialize");
    }
    else
    {
        JsValueRef rootObject = arguments[1];
        JsValueRef transferArray = nullptr;
        if (argumentCount > 2)
        {
            JsValueType argumentType = JsUndefined;
            transferArray = arguments[2];
            IfJsrtErrorSetGo(ChakraRTInterface::JsGetValueType(transferArray, &argumentType));

            if (argumentType != JsUndefined)
            {
                if (argumentType != JsArray)
                {
                    errorCode = JsErrorInvalidArgument;
                    goto Error;
                }

                JsPropertyIdRef lengthPropId;
                JsValueRef arrayLengthObj = JS_INVALID_REFERENCE;
                double arrayLengthDouble = 0.0;
                IfJsrtErrorSetGo(CreatePropertyIdFromString("length", &lengthPropId));
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetProperty(transferArray, lengthPropId, &arrayLengthObj));
                IfJsrtErrorSetGo(ChakraRTInterface::JsNumberToDouble(arrayLengthObj, &arrayLengthDouble));
                if (!std::isfinite(arrayLengthDouble) || arrayLengthDouble < 0.0 ||
                    arrayLengthDouble > static_cast<double>(kMaxTransferListLength))
                {
                    errorCode = JsErrorInvalidArgument;
                    errorMessage = _u("Invalid transfer list length for WScript.SerializeObject");
                    goto Error;
                }

                size_t arrayLength = static_cast<size_t>(arrayLengthDouble);
                if (arrayLength > 0)
                {
                    transferVars.reserve(arrayLength);
                    std::unordered_set<void*> seenTransferables;

                    for (size_t i = 0; i < arrayLength; i++)
                    {
                        JsValueRef index;
                        JsValueRef value = JS_INVALID_REFERENCE;
                        JsValueType jsType = JsUndefined;

                        IfJsrtErrorSetGo(ChakraRTInterface::JsIntToNumber(static_cast<int>(i), &index));
                        IfJsrtErrorSetGo(ChakraRTInterface::JsGetIndexedProperty(transferArray, index, &value));
                        IfJsrtErrorSetGo(ChakraRTInterface::JsGetValueType(value, &jsType));
                        if (jsType == JsArrayBuffer)
                        {
                            if (!seenTransferables.insert(static_cast<void*>(value)).second)
                            {
                                errorCode = JsErrorInvalidArgument;
                                errorMessage = _u("Duplicate ArrayBuffer in transfer list for WScript.SerializeObject");
                                goto Error;
                            }
                            transferVars.push_back(value);
                        }
                    }
                }
            }
        }

        blob.reset(new (std::nothrow) SerializerBlob());
        if (!blob)
        {
            errorCode = JsErrorOutOfMemory;
            goto Error;
        }

        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializer(ReallocateBufferMemory, WriteHostObject, &bufferState, &serializerGuard.handle));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerSetTransferableVars(
            serializerGuard.handle,
            transferVars.empty() ? nullptr : transferVars.data(),
            transferVars.size()));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerWriteValue(serializerGuard.handle, rootObject));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerReleaseData(serializerGuard.handle, (byte**)&blob->data, &blob->dataLength));
        // Ownership of the realloc'd byte buffer has transferred to `blob`.
        bufferState.buffer = nullptr;

        // Release serializer handle eagerly now that its buffer has been released.
        {
            JsVarSerializerHandle handleToFree = serializerGuard.handle;
            serializerGuard.handle = nullptr;
            IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerFree(handleToFree));
        }

        for (size_t i = 0; i < transferVars.size(); i++)
        {
            JsValueRef arrayBuffer = transferVars[i];
            ArrayBufferTransferInfo bufferInfo = {};
            IfJsrtErrorSetGo(ChakraRTInterface::JsGetArrayBufferStorage(arrayBuffer, &bufferInfo.buffer, &bufferInfo.length));
            IfJsrtErrorSetGo(ChakraRTInterface::JsExternalizeArrayBuffer(arrayBuffer));
            IfJsrtErrorSetGo(ChakraRTInterface::JsGetArrayBufferFreeFunction(arrayBuffer, &bufferInfo.freeFn));
            IfJsrtErrorSetGo(ChakraRTInterface::JsDetachArrayBuffer(arrayBuffer));
            // Only claim ownership in blob after detach succeeds, so a failed
            // detach cannot double-free storage still owned by arrayBuffer.
            blob->transferableArrays.push_back(bufferInfo);
        }

        tokenBuf = calloc(1, sizeof(uint64_t));
        if (tokenBuf == nullptr)
        {
            errorCode = JsErrorOutOfMemory;
            goto Error;
        }

        {
            HostGlobalsLock lock;
            SerializerRegistry()[tokenBuf] = blob.get();
            tokenRegistered = true;
        }

        errorCode = ChakraRTInterface::JsCreateExternalArrayBuffer(
            tokenBuf,
            static_cast<unsigned int>(sizeof(uint64_t)),
            SerializerTokenFinalizer,
            tokenBuf,
            &returnValue);
        if (errorCode != JsNoError)
        {
            goto Error;
        }

        // Ownership of both tokenBuf and blob is now committed to the external
        // ArrayBuffer + SerializerRegistry.
        blob.release();
        tokenBuf = nullptr;
        tokenRegistered = false;
    }
Error:
    if (tokenRegistered && tokenBuf != nullptr)
    {
        HostGlobalsLock lock;
        SerializerRegistry().erase(tokenBuf);
    }
    if (tokenBuf != nullptr)
    {
        free(tokenBuf);
        tokenBuf = nullptr;
    }

    if (errorCode != JsNoError && errorMessage[0] == _u('\0'))
    {
        errorMessage = _u("WScript.SerializeObject failed");
    }
    SetExceptionIf(errorCode, errorMessage);

    return returnValue;
}

static JsValueRef CHAKRA_CALLBACK ReadHostObject(void * state)
{
    Assert(false); // TBD
    return nullptr;
}

static JsValueRef CHAKRA_CALLBACK GetSharedArrayBufferFromId(void * state, uint32_t id)
{
    Assert(false); // TBD
    return nullptr;
}

struct BufferFreeFunctionState
{
    ArrayBufferFreeFn freeFn;
    void* buffer;
};

static void CHAKRA_CALLBACK BufferFreeFunction(void * state)
{
    BufferFreeFunctionState* bufferState = (BufferFreeFunctionState*)state;
    if (!bufferState)
    {
        return;
    }
    if (bufferState->freeFn)
    {
        bufferState->freeFn(bufferState->buffer);
    }
    delete bufferState;
}

JsValueRef __stdcall WScriptJsrt::Deserialize(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("");
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    HRESULT hr = S_OK;
    // Declaration order matters: deserializerGuard is destroyed before blob so
    // JsVarDeserializerFree runs while blob->data is still valid.
    std::unique_ptr<SerializerBlob> blob;
    VarDeserializerGuard deserializerGuard;
    std::vector<JsValueRef> transferables;

    if (argumentCount < 2)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Need an argument for WScript.Deserialize");
    }
    else
    {
        JsValueRef dataObject = arguments[1];
        uint32 dataLength = 0;
        BYTE *data = nullptr;
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetArrayBufferStorage(dataObject, &data, &dataLength));
        if (data == nullptr || dataLength != sizeof(uint64_t))
        {
            errorCode = JsErrorInvalidArgument;
            errorMessage = _u("Invalid serialized data buffer for WScript.Deserialize");
            goto Error;
        }

        {
            HostGlobalsLock lock;
            auto& registry = SerializerRegistry();
            auto it = registry.find(static_cast<void*>(data));
            if (it == registry.end())
            {
                errorCode = JsErrorInvalidArgument;
                errorMessage = _u("Invalid or already-deserialized buffer for WScript.Deserialize");
                goto Error;
            }
            blob.reset(it->second);
            registry.erase(it);
        }

        if (!blob || blob->data == nullptr || blob->dataLength == 0)
        {
            errorCode = JsErrorInvalidArgument;
            errorMessage = _u("Empty serialized data buffer for WScript.Deserialize");
            goto Error;
        }

        IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializer(blob->data, blob->dataLength, ReadHostObject, GetSharedArrayBufferFromId, nullptr, &deserializerGuard.handle));

        size_t arraySize = blob->transferableArrays.size();
        if (arraySize > 0)
        {
            transferables.resize(arraySize, JS_INVALID_REFERENCE);

            for (size_t i = 0; i < arraySize; ++i)
            {
                JsValueRef result = nullptr;
                std::unique_ptr<BufferFreeFunctionState> bufferFreeState(new (std::nothrow) BufferFreeFunctionState());
                if (!bufferFreeState)
                {
                    errorCode = JsErrorOutOfMemory;
                    goto Error;
                }
                bufferFreeState->buffer = blob->transferableArrays[i].buffer;
                bufferFreeState->freeFn = blob->transferableArrays[i].freeFn;
                IfJsrtErrorSetGo(ChakraRTInterface::JsCreateExternalArrayBuffer(
                    blob->transferableArrays[i].buffer,
                    blob->transferableArrays[i].length,
                    BufferFreeFunction,
                    bufferFreeState.get(),
                    &result));
                // Ownership of the transferred storage and free state now belongs
                // to the new external ArrayBuffer; clear the blob slot to avoid double-free.
                bufferFreeState.release();
                blob->transferableArrays[i].buffer = nullptr;
                blob->transferableArrays[i].freeFn = nullptr;
                transferables[i] = result;
            }

            IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerSetTransferableVars(
                deserializerGuard.handle,
                transferables.data(),
                transferables.size()));
        }

        IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerReadValue(deserializerGuard.handle, &returnValue));
        {
            JsVarDeserializerHandle handleToFree = deserializerGuard.handle;
            deserializerGuard.handle = nullptr;
            IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerFree(handleToFree));
        }
    }

Error:
    if (errorCode != JsNoError && errorMessage[0] == _u('\0'))
    {
        errorMessage = _u("WScript.Deserialize failed");
    }
    SetExceptionIf(errorCode, errorMessage);

    return returnValue;
}
