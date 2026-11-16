//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

JsValueRef WScriptJsrt::MonotonicNowCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    LPCWSTR errorMessage = _u("invalid call to WScript.monotonicNow");
    JsErrorCode errorCode = JsNoError;
    HRESULT hr = S_OK;
    JsValueRef result;

    IfJsrtErrorSetGo(ChakraRTInterface::JsDoubleToNumber(static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) / 1e6 /* ns in ms */, &result));
    return result;

Error:
    SetExceptionIf(errorCode, errorMessage);
    return JS_INVALID_REFERENCE;
}

JsValueRef __stdcall WScriptJsrt::BroadcastCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("invalid call to WScript.Broadcast");

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

    if (argumentCount <= 1)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("WScript.Broadcast requires a SharedArrayBuffer");
        goto Error;
    }

    {
        // Validate the broadcast argument BEFORE queuing to any child.
        // Stage a validation handle first so MarkCrossRuntime and
        // MonitorPendingAsyncWaiters run even when there are no live children.
        JsSharedArrayBufferContentHandle validationContent = nullptr;
        if (ChakraRTInterface::JsGetSharedArrayBufferContent(arguments[1], &validationContent) != JsNoError)
        {
            errorCode = JsErrorInvalidArgument;
            errorMessage = _u("WScript.Broadcast requires a SharedArrayBuffer");
            goto Error;
        }

        auto& threadData = GetRuntimeThreadLocalData().threadData;
        if (threadData)
        {
            struct EnqueuedChild
            {
                RuntimeThreadData* child;
                uint64_t targetSeq;
            };
            std::vector<EnqueuedChild> enqueued;
            enqueued.reserve(threadData->children.size());

            // Ownership record (doc/memory-management.md section 3):
            // Each live non-departing child receives its own AddRef'd
            // JsSharedArrayBufferContentHandle in its per-child broadcastQ
            // with a monotonic sequence number. Broadcast N+1 pushes behind
            // broadcast N without overwriting or prematurely releasing N's
            // handle, even if a slow child overruns the 5 s ack wait.
            for (auto i = threadData->children.begin(); i != threadData->children.end(); i++)
            {
                auto child = *i;
                if (child->leaving.load() || WaitForSingleObject(child->hThread, 0) == WAIT_OBJECT_0)
                {
                    continue;
                }
                JsSharedArrayBufferContentHandle childContent = nullptr;
                if (ChakraRTInterface::JsGetSharedArrayBufferContent(arguments[1], &childContent) != JsNoError)
                {
                    continue;
                }
                EnterCriticalSection(&child->csBroadcastQ);
                uint64_t targetSeq = ++child->nextBroadcastSeq;
                child->broadcastQ.push_back({ childContent, targetSeq });
                SetEvent(child->hevntReceivedBroadcast);
                LeaveCriticalSection(&child->csBroadcastQ);
                enqueued.push_back({ child, targetSeq });
            }

            ChakraRTInterface::JsReleaseSharedArrayBufferContentHandle(validationContent);

            for (size_t idx = 0; idx < enqueued.size(); idx++)
            {
                RuntimeThreadData* child = enqueued[idx].child;
                uint64_t targetSeq = enqueued[idx].targetSeq;
                const ULONGLONG startMs = GetTickCount64();
                bool departed = false;
                while (child->consumedBroadcastSeq.load() < targetSeq)
                {
                    if (WaitForSingleObject(child->hThread, 0) == WAIT_OBJECT_0)
                    {
                        departed = true;
                        break;
                    }
                    ULONGLONG elapsed = GetTickCount64() - startMs;
                    if (elapsed >= 5000)
                    {
                        break;
                    }
                    DWORD slice = (DWORD)((5000 - elapsed) < 50 ? (5000 - elapsed) : 50);
                    WaitForSingleObject(child->hevntBroadcastConsumed, slice);
                }
                if (child->consumedBroadcastSeq.load() < targetSeq && !departed)
                {
                    ULONGLONG waitedMs = GetTickCount64() - startMs;
                    fwprintf(stderr, _u("live agent did not acknowledge broadcast (waited %llu ms)\n"),
                        static_cast<unsigned long long>(waitedMs));
                    fflush(stderr);
                }
            }
        }
        else
        {
            ChakraRTInterface::JsReleaseSharedArrayBufferContentHandle(validationContent);
        }
    }

Error:
    SetExceptionIf(errorCode, errorMessage);
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::ReceiveBroadcastCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

    if (argumentCount > 1)
    {
        auto& threadData = GetRuntimeThreadLocalData().threadData;
        if (threadData)
        {
            JsValueRef newCallback = arguments[1];
            if (ChakraRTInterface::JsAddRef(newCallback, nullptr) == JsNoError)
            {
                if (threadData->receiveBroadcastCallbackFunc)
                {
                    ChakraRTInterface::JsRelease(threadData->receiveBroadcastCallbackFunc, nullptr);
                }
                threadData->receiveBroadcastCallbackFunc = newCallback;
            }
        }
    }

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::ReportCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

    if (argumentCount > 1)
    {
        JsValueRef stringRef;
        ChakraRTInterface::JsConvertValueToString(arguments[1], &stringRef);

        AutoString autoStr(stringRef);

        if (autoStr.GetError() == JsNoError)
        {
            std::string str(autoStr.GetString());
            auto& threadData = GetRuntimeThreadLocalData().threadData;

            if (threadData && threadData->parent)
            {
                EnterCriticalSection(&threadData->parent->csReportQ);
                threadData->parent->reportQ.push_back(str);
                LeaveCriticalSection(&threadData->parent->csReportQ);
            }
        }
    }

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::GetReportCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetNullValue(&returnValue));

    if (argumentCount > 0)
    {
        auto& threadData = GetRuntimeThreadLocalData().threadData;
        if (threadData)
        {
            EnterCriticalSection(&threadData->csReportQ);

            if (threadData->reportQ.size() > 0)
            {
                auto str = threadData->reportQ.front();
                threadData->reportQ.pop_front();
                ChakraRTInterface::JsCreateString(str.c_str(), str.size(), &returnValue);
            }
            LeaveCriticalSection(&threadData->csReportQ);
        }
    }

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::LeavingCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

    if (argumentCount > 0)
    {
        auto& threadData = GetRuntimeThreadLocalData().threadData;
        if (threadData)
        {
            threadData->leaving.store(true);
        }
    }

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::SleepCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

    if (argumentCount > 1)
    {
        double timeout = 0.0;
        ChakraRTInterface::JsNumberToDouble(arguments[1], &timeout);
        Sleep((DWORD)timeout);
    }

Error:
    return returnValue;
}
