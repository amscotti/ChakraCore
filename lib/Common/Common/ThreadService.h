//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

// Note, these definitions must match the definitions in jsrt.h.

// Callback for a background work item.
namespace JsUtil
{
    class ThreadService
    {
    public:
        typedef void (CALLBACK *BackgroundWorkItemCallback)(void * callbackData);

        // Callback for the thread service itself, to which we can submit background work items.

        typedef bool (CALLBACK *ThreadServiceCallback)(BackgroundWorkItemCallback callback, void * callbackData);

    private:
        ThreadServiceCallback threadService;
        bool isInCallback;

    public:
        ThreadService(ThreadServiceCallback threadService) :
            threadService(threadService),
            isInCallback(false)
        {
        }

        bool Invoke(BackgroundWorkItemCallback callback, void * callbackData)
        {
            isInCallback = true;
            bool result = threadService(callback, callbackData);
            isInCallback = false;
            return result;
        }

        // Precondition: a valid ThreadService object. A null ThreadService*
        // means "no host callback" and must be normalized or guarded by the
        // caller (Recycler::EnableConcurrent does this); calling a member
        // through a null pointer is UB and modern optimizers elide any
        // this-null check, which crashed GCStress at address 0.
        bool HasCallback() const
        {
            return threadService != nullptr;
        }

        bool IsInCallback() const
        {
            return isInCallback;
        }
    };
};
