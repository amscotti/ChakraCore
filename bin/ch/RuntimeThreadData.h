//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once
#include <list>

class RuntimeThreadData
{
public:
    RuntimeThreadData();
    ~RuntimeThreadData();
    HANDLE hevntInitialScriptCompleted;
    HANDLE hevntReceivedBroadcast;
    HANDLE hevntShutdown;
    // Set by the child AFTER it consumed the current broadcast's shared
    // content (its SAB wrapper was created), so the parent can wait for
    // every live child before releasing the content handle and before a
    // later broadcast overwrites RuntimeThreadData::sharedContent. Replaces
    // the old per-broadcast semaphore, which the parent closed after the
    // FIRST child's release, leaving the remaining children to signal a
    // closed (possibly recycled) handle. Auto-reset, owned by the child's
    // RuntimeThreadData for the child's whole lifetime, so the parent never
    // waits on a handle that can be closed from under it.
    HANDLE hevntBroadcastConsumed;
    HANDLE hThread;

    JsSharedArrayBufferContentHandle sharedContent;
    JsValueRef receiveBroadcastCallbackFunc;


    JsRuntimeHandle runtime;
    JsContextRef context;

    // Per-child job/timer queue (S6e). Created in ThreadProc after the child
    // context exists and installed as the callbackState of the promise and
    // timer continuation callbacks, so promise reactions, async-function
    // steps and engine-delayed tasks (Atomics.waitAsync timeout/monitor
    // tasks) raised on the child context are drained on the child thread.
    // Owned by ThreadProc: torn down (RemoveAll + delete) on the child thread
    // BEFORE JsDisposeRuntime, because messages own JsRefs that must be
    // released while the runtime is still valid.
    MessageQueue* messageQueue;


    std::string initialSource;

    RuntimeThreadData* parent;
    
    std::list<RuntimeThreadData*> children;

    CRITICAL_SECTION csReportQ;
    std::list<std::string> reportQ;

    bool leaving;


    DWORD ThreadProc();

};

struct RuntimeThreadLocalData
{
    // can't use ctor/dtor because it's not supported in VS2012
    // error C2483: 'threadLocalData' : object with constructor or destructor cannot be declared 'thread' 
    void Initialize(RuntimeThreadData* threadData);
    void Uninitialize();
    RuntimeThreadData* threadData;
};

RuntimeThreadLocalData& GetRuntimeThreadLocalData();
