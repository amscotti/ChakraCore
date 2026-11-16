//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

struct ArrayBufferTransferInfo
{
    byte* buffer;
    uint length;
    ArrayBufferFreeFn freeFn;
};

struct SerializerBlob
{
    void *data;
    size_t dataLength;
    std::vector<ArrayBufferTransferInfo> transferableArrays;
};

static byte * CHAKRA_CALLBACK ReallocateBufferMemory(void * state, byte *oldBuffer, size_t newSize, size_t *allocatedSize)
{
    void* data = realloc((void*)oldBuffer, newSize);
    if (allocatedSize)
    {
        *allocatedSize = newSize;
    }
    return (byte*)data;
}

static bool CHAKRA_CALLBACK WriteHostObject(void * state, JsValueRef data)
{
    // Not implemented
    return true;
}

JsValueRef __stdcall WScriptJsrt::SerializeObject(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("");
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    HRESULT hr = S_OK;
    JsValueRef *transferVarsArray = nullptr;
    int transferVarsCount = 0;
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
                int arrayLength = 0;
                IfJsrtErrorSetGo(CreatePropertyIdFromString("length", &lengthPropId));
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetProperty(transferArray, lengthPropId, &arrayLengthObj));
                IfJsrtErrorSetGo(ChakraRTInterface::JsNumberToInt(arrayLengthObj, &arrayLength));
                if (arrayLength > 0)
                {
                    transferVarsArray = new JsValueRef[arrayLength];
                    if (transferVarsArray == nullptr)
                    {
                        errorCode = JsErrorOutOfMemory;
                        goto Error;
                    }

                    for (int i = 0; i < arrayLength; i++)
                    {
                        JsValueRef index;
                        JsValueRef value = JS_INVALID_REFERENCE;
                        JsValueType jsType = JsUndefined;

                        IfJsrtErrorSetGo(ChakraRTInterface::JsIntToNumber(i, &index));
                        IfJsrtErrorSetGo(ChakraRTInterface::JsGetIndexedProperty(transferArray, index, &value));
                        IfJsrtErrorSetGo(ChakraRTInterface::JsGetValueType(value, &jsType));
                        if (jsType == JsArrayBuffer)
                        {
                            *(transferVarsArray + transferVarsCount) = value;
                            transferVarsCount++;
                        }
                    }
                }
            }
        }

        JsVarSerializerHandle serializerHandle = nullptr;

        // This memory will be claimed at WScriptJsrt::Deserialize.
        SerializerBlob *blob = new SerializerBlob();
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializer(ReallocateBufferMemory, WriteHostObject, nullptr, &serializerHandle));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerSetTransferableVars(serializerHandle, transferVarsArray, transferVarsCount));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerWriteValue(serializerHandle, rootObject));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerReleaseData(serializerHandle, (byte**)&blob->data, &blob->dataLength));

        for (int i = 0; i < transferVarsCount; i++)
        {
            JsValueRef arrayBuffer = transferVarsArray[i];
            ArrayBufferTransferInfo bufferInfo;
            IfJsrtErrorSetGo(ChakraRTInterface::JsGetArrayBufferStorage(arrayBuffer, &bufferInfo.buffer, &bufferInfo.length));
            IfJsrtErrorSetGo(ChakraRTInterface::JsExternalizeArrayBuffer(arrayBuffer));
            IfJsrtErrorSetGo(ChakraRTInterface::JsGetArrayBufferFreeFunction(arrayBuffer, &bufferInfo.freeFn));
            blob->transferableArrays.push_back(bufferInfo);
            IfJsrtErrorSetGo(ChakraRTInterface::JsDetachArrayBuffer(arrayBuffer));
        }

        errorCode = ChakraRTInterface::JsCreateExternalArrayBuffer((void*)blob, sizeof(SerializerBlob), nullptr, nullptr, &returnValue);
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarSerializerFree(serializerHandle));
    }
Error:
    SetExceptionIf(errorCode, errorMessage);

    if (transferVarsArray)
    {
        delete[] transferVarsArray;
    }

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
    JsValueRef * transferables = nullptr;
    HRESULT hr = S_OK;
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
        SerializerBlob *blob = (SerializerBlob*)data;
        JsVarDeserializerHandle deserializerHandle = nullptr;
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializer(blob->data, blob->dataLength, ReadHostObject, GetSharedArrayBufferFromId, nullptr, &deserializerHandle));

        size_t arraySize = blob->transferableArrays.size();
        if (arraySize > 0)
        {
            transferables = new JsValueRef[arraySize];

            for (size_t i = 0; i < arraySize; ++i)
            {
                JsValueRef result = nullptr;
                BufferFreeFunctionState* bufferFreeState = new BufferFreeFunctionState();
                bufferFreeState->buffer = blob->transferableArrays[i].buffer;
                bufferFreeState->freeFn = blob->transferableArrays[i].freeFn;
                IfJsrtErrorSetGo(ChakraRTInterface::JsCreateExternalArrayBuffer(blob->transferableArrays[i].buffer, blob->transferableArrays[i].length, BufferFreeFunction, bufferFreeState, &result));
                transferables[i] = result;
            }

            IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerSetTransferableVars(deserializerHandle, transferables, arraySize));
        }

        IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerReadValue(deserializerHandle, &returnValue));
        IfJsrtErrorSetGo(ChakraRTInterface::JsVarDeserializerFree(deserializerHandle));
        delete blob;
    }

Error:
    SetExceptionIf(errorCode, errorMessage);
    if (transferables)
    {
        delete[] transferables;
    }

    return returnValue;
}
