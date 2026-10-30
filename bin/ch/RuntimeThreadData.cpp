//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "stdafx.h"

void RuntimeThreadLocalData::Initialize(RuntimeThreadData* threadData)
{
    this->threadData = threadData;
}

void RuntimeThreadLocalData::Uninitialize()
{
}

THREAD_LOCAL RuntimeThreadLocalData threadLocalData;

RuntimeThreadLocalData& GetRuntimeThreadLocalData()
{
    return threadLocalData;
}

RuntimeThreadData::RuntimeThreadData() :
    hSemaphore(nullptr), 
    hThread(nullptr),
    sharedContent(nullptr),
    receiveBroadcastCallbackFunc(nullptr),
    runtime(nullptr),
    context(nullptr),
    messageQueue(nullptr),
    parent(nullptr),
    leaving(false)
{
    this->hevntInitialScriptCompleted = CreateEvent(NULL, TRUE, FALSE, NULL);
    this->hevntReceivedBroadcast = CreateEvent(NULL, FALSE, FALSE, NULL);
    this->hevntShutdown = CreateEvent(NULL, TRUE, FALSE, NULL);

    InitializeCriticalSection(&csReportQ);
}

RuntimeThreadData::~RuntimeThreadData()
{
    CloseHandle(this->hevntInitialScriptCompleted);
    CloseHandle(this->hevntReceivedBroadcast);
    CloseHandle(this->hevntShutdown);
    CloseHandle(this->hThread);
    DeleteCriticalSection(&csReportQ);
}

DWORD RuntimeThreadData::ThreadProc()
{
    JsValueRef scriptSource;
    JsValueRef fname;
    const char* fullPath = "agent source";
    HRESULT hr = S_OK;
    MessageQueue* agentQueue = nullptr;

    threadLocalData.Initialize(this);

    IfJsErrorFailLog(ChakraRTInterface::JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &runtime));
    IfJsErrorFailLog(ChakraRTInterface::JsCreateContext(runtime, &context));
    IfJsErrorFailLog(ChakraRTInterface::JsSetCurrentContext(context));


    if (!WScriptJsrt::Initialize())
    {
        IfFailGo(E_FAIL);
    }

    // Per-child job/timer queue (S6e). The same callbacks the main host loop
    // installs, but on a queue owned by this thread: promise reactions and
    // async-function steps raised on the child context, plus engine-delayed
    // tasks (finite Atomics.waitAsync timeouts and the cross-agent monitor
    // task), are drained on this thread at safe points below. Without this,
    // an async receiveBroadcast suspends at its first await and its
    // continuations are dropped.
    agentQueue = new MessageQueue();
    this->messageQueue = agentQueue;
    IfJsErrorFailLog(ChakraRTInterface::JsSetPromiseContinuationCallback(WScriptJsrt::PromiseContinuationCallback, (void*)agentQueue));
    IfJsErrorFailLog(ChakraRTInterface::JsSetTimerContinuationCallback(WScriptJsrt::TimerContinuationCallback, (void*)agentQueue));


    IfJsErrorFailLog(ChakraRTInterface::JsCreateExternalArrayBuffer((void*)this->initialSource.c_str(),
        (unsigned int)this->initialSource.size(), nullptr, nullptr, &scriptSource));


    ChakraRTInterface::JsCreateString(fullPath, strlen(fullPath), &fname);

    ChakraRTInterface::JsRun(scriptSource, WScriptJsrt::GetNextSourceContext(), fname, JsParseScriptAttributeNone, nullptr);

    SetEvent(this->parent->hevntInitialScriptCompleted);

    // loop waiting for work;

    while (true)
    {
        // Block until a broadcast/shutdown arrives or the earliest delayed
        // task is due. A pending self-rescheduling task (waitAsync monitor)
        // therefore bounds this wait by its poll interval instead of parking
        // the thread forever.
        DWORD waitMs = INFINITE;
        if (!agentQueue->IsEmpty())
        {
            const long long delta = (long long)(agentQueue->NextDueTime() - GetTickCount64());
            waitMs = delta <= 0 ? 0 : (delta > (long long)0xFFFFFFFE ? 0xFFFFFFFE : (DWORD)delta);
        }

        HANDLE handles[] = { this->hevntReceivedBroadcast, this->hevntShutdown };
        DWORD waitRet = WaitForMultipleObjects(_countof(handles), handles, false, waitMs);

        if (waitRet == WAIT_OBJECT_0)
        {
            JsValueRef args[3];
            ChakraRTInterface::JsGetGlobalObject(&args[0]);
            ChakraRTInterface::JsCreateSharedArrayBufferWithSharedContent(this->parent->sharedContent, &args[1]);
            ChakraRTInterface::JsDoubleToNumber(1, &args[2]);

            // notify the parent we received the data
            ReleaseSemaphore(this->parent->hSemaphore, 1, NULL);

            if (this->receiveBroadcastCallbackFunc)
            {
                ChakraRTInterface::JsCallFunction(this->receiveBroadcastCallbackFunc, args, 3, nullptr);
            }
        }

        if (waitRet == WAIT_OBJECT_0 || waitRet == WAIT_TIMEOUT)
        {
            // Safe point: drain due jobs/timers on this thread. Each message
            // may queue more (promise reactions run synchronously queued
            // continuations; the monitor task re-enqueues itself with a
            // future due time, which the next timed wait above waits for).
            // Stop draining when the script asked to leave or the parent
            // signaled shutdown so teardown stays prompt.
            while (!this->leaving &&
                   WaitForSingleObject(this->hevntShutdown, 0) != WAIT_OBJECT_0 &&
                   agentQueue->HasDueMessage())
            {
                MessageBase* msg = agentQueue->PopDue();
                // Omit checking the return value: like the main loop's
                // ProcessAll, one failing job must not block the rest.
                msg->Call(fullPath);
                delete msg;
            }
        }

        if ((waitRet == WAIT_OBJECT_0 + 1) || this->leaving ||
            WaitForSingleObject(this->hevntShutdown, 0) == WAIT_OBJECT_0)
        {
            WScriptJsrt::Uninitialize();

            // Tear the queue down BEFORE disposing the runtime: its messages
            // own JsRefs (task functions) that must be released while the
            // runtime is still valid. A still-pending monitor task's waiter
            // intentionally stays unresolved; runtime disposal finalizes the
            // wrapper, whose finalizer unlinks the native registration token.
            if (agentQueue != nullptr)
            {
                agentQueue->RemoveAll();
                delete agentQueue;
                this->messageQueue = nullptr;
            }

            if (this->receiveBroadcastCallbackFunc)
            {
                ChakraRTInterface::JsRelease(this->receiveBroadcastCallbackFunc, nullptr);
            }
            ChakraRTInterface::JsSetCurrentContext(nullptr);
            ChakraRTInterface::JsDisposeRuntime(runtime);

            threadLocalData.Uninitialize();
            return 0;
        }
        else if (waitRet != WAIT_OBJECT_0 && waitRet != WAIT_TIMEOUT)
        {
            Assert(false);
            break;
        }
    }

Error:

    if (agentQueue != nullptr)
    {
        agentQueue->RemoveAll();
        delete agentQueue;
        this->messageQueue = nullptr;
    }
    ChakraRTInterface::JsSetCurrentContext(nullptr);
    ChakraRTInterface::JsDisposeRuntime(runtime);
    threadLocalData.Uninitialize();
    return 0;
}
