//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
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
    hevntBroadcastConsumed(nullptr),
    hThread(nullptr),
    nextBroadcastSeq(0),
    consumedBroadcastSeq(0),
    receiveBroadcastCallbackFunc(nullptr),
    runtime(nullptr),
    context(nullptr),
    messageQueue(nullptr),
    parent(nullptr),
    leaving(false)
{
    this->hevntInitialScriptCompleted = CreateEvent(NULL, TRUE, FALSE, NULL);
    this->hevntReceivedBroadcast = CreateEvent(NULL, TRUE, FALSE, NULL);
    this->hevntShutdown = CreateEvent(NULL, TRUE, FALSE, NULL);
    this->hevntBroadcastConsumed = CreateEvent(NULL, FALSE, FALSE, NULL);

    InitializeCriticalSection(&csBroadcastQ);
    InitializeCriticalSection(&csReportQ);
}

RuntimeThreadData::~RuntimeThreadData()
{
    CloseHandle(this->hevntInitialScriptCompleted);
    CloseHandle(this->hevntReceivedBroadcast);
    CloseHandle(this->hevntShutdown);
    CloseHandle(this->hevntBroadcastConsumed);
    CloseHandle(this->hThread);
    DeleteCriticalSection(&csBroadcastQ);
    DeleteCriticalSection(&csReportQ);
}

void RuntimeThreadData::DrainBroadcastQueue(bool signalConsumed)
{
    while (true)
    {
        BroadcastEntry entry = { nullptr, 0 };
        EnterCriticalSection(&this->csBroadcastQ);
        if (!this->broadcastQ.empty())
        {
            entry = this->broadcastQ.front();
            this->broadcastQ.pop_front();
        }
        if (this->broadcastQ.empty())
        {
            ResetEvent(this->hevntReceivedBroadcast);
        }
        LeaveCriticalSection(&this->csBroadcastQ);

        if (entry.content == nullptr)
        {
            break;
        }

        ChakraRTInterface::JsReleaseSharedArrayBufferContentHandle(entry.content);
        if (signalConsumed)
        {
            this->consumedBroadcastSeq.store(entry.seq);
            SetEvent(this->hevntBroadcastConsumed);
        }
    }
}

DWORD RuntimeThreadData::ThreadProc()
{
    JsValueRef scriptSource;
    JsValueRef fname;
    const char* fullPath = "agent source";
    HRESULT hr = S_OK;
    MessageQueue* agentQueue = nullptr;

    threadLocalData.Initialize(this);

    // Agent runtimes are small, short-lived and single-threaded: create them
    // with JsRuntimeAttributeDisableBackgroundWork so the recycler runs
    // in-thread (OptimizeForManyInstances) instead of spawning a
    // "Chakra Background Recycler" + parallel-mark threads per agent. ch
    // installs no JsRT thread service, so each agent runtime otherwise owns
    // raw background GC threads whose concurrent/parallel shutdown raced
    // agent script and teardown (review-reproduced SIGSEGV/SIGTRAP/SIGBUS:
    // Recycler::FinalizeConcurrent -> RecyclerParallelThread::Shutdown ->
    // ThreadService::HasCallback on a stale pointer; queued-callback
    // context teardown; recycler->IsReentrantState() and
    // !IsScriptActive() asserts) — with 8+ agents the thread population and
    // the races multiply. This is the same attribute embedders use for
    // worker runtimes; GC still runs, just in-thread.
    IfJsErrorFailLog(ChakraRTInterface::JsCreateRuntime(
        (JsRuntimeAttributes)(JsRuntimeAttributeDisableBackgroundWork), nullptr, &runtime));
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
            BroadcastEntry entry = { nullptr, 0 };
            EnterCriticalSection(&this->csBroadcastQ);
            if (!this->broadcastQ.empty())
            {
                entry = this->broadcastQ.front();
                this->broadcastQ.pop_front();
            }
            if (this->broadcastQ.empty())
            {
                ResetEvent(this->hevntReceivedBroadcast);
            }
            LeaveCriticalSection(&this->csBroadcastQ);

            if (entry.content != nullptr)
            {
                if (this->leaving.load() || WaitForSingleObject(this->hevntShutdown, 0) == WAIT_OBJECT_0)
                {
                    // Departing agent: release our queued content ref and ack
                    // so the parent handshake completes promptly.
                    ChakraRTInterface::JsReleaseSharedArrayBufferContentHandle(entry.content);
                    this->consumedBroadcastSeq.store(entry.seq);
                    SetEvent(this->hevntBroadcastConsumed);
                }
                else
                {
                    JsValueRef args[3];
                    ChakraRTInterface::JsGetGlobalObject(&args[0]);
                    ChakraRTInterface::JsCreateSharedArrayBufferWithSharedContent(entry.content, &args[1]);
                    ChakraRTInterface::JsReleaseSharedArrayBufferContentHandle(entry.content);
                    ChakraRTInterface::JsDoubleToNumber(1, &args[2]);

                    // Notify the parent we consumed this broadcast (the wrapper
                    // above already AddRef'd SharedContents and we released our
                    // queue item ref) BEFORE running user script.
                    this->consumedBroadcastSeq.store(entry.seq);
                    SetEvent(this->hevntBroadcastConsumed);

                    if (this->receiveBroadcastCallbackFunc)
                    {
                        ChakraRTInterface::JsCallFunction(this->receiveBroadcastCallbackFunc, args, 3, nullptr);
                    }
                }
            }
        }

        if (waitRet == WAIT_OBJECT_0 || waitRet == WAIT_TIMEOUT)
        {
            // Safe point: drain due jobs/timers on this thread. Each message
            // may queue more (promise reactions run synchronously queued
            // continuations; the monitor task re-enqueues itself with a
            // future due time, which the next timed wait above waits for).
            // Stop draining when the script asked to leave or the parent
            // signaled shutdown so teardown stays prompt. Unlike the main
            // loop's ProcessAll this deliberately does not call
            // JsTTDNotifyYield: TTD is not modeled for crossthread agents
            // (the engine schedules no delayed tasks while recording or
            // replaying), so there is no TTD yield point to honor on an
            // agent thread.
            while (!this->leaving.load() &&
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

        if ((waitRet == WAIT_OBJECT_0 + 1) || this->leaving.load() ||
            WaitForSingleObject(this->hevntShutdown, 0) == WAIT_OBJECT_0)
        {
            // Drain any remaining queued broadcasts while this child's context
            // is still current, releasing every handle and unblocking any
            // waiting BroadcastCallback.
            this->DrainBroadcastQueue(true);

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

    // Release the spawning parent before anything else: it waits with an
    // INFINITE timeout on hevntInitialScriptCompleted
    // (WScriptJsrt::LoadScriptCallback, crossthread branch), and until
    // this fix only the success path above ever set that event — a child
    // whose runtime/context creation or host Initialize failed landed
    // here, exited, and left the parent blocked forever. `parent` is
    // assigned before CreateThread and outlives this thread (the parent
    // joins children before deleting them). On the leaving-loop `break`
    // path that also reaches this label the event was already set during
    // spawn, so re-setting the manual-reset event is a no-op.
    if (this->parent != nullptr)
    {
        SetEvent(this->parent->hevntInitialScriptCompleted);
    }

    if (this->context != nullptr)
    {
        this->DrainBroadcastQueue(true);
    }

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
