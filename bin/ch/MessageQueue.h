//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once
#include <atomic>

class MessageBase
{
private:
    // Absolute due time in 64-bit monotonic ticks (PAL GetTickCount64, ms).
    // This must stay 64-bit: 32-bit storage wrapped for large valid delays
    // (the timer continuation callback can receive up to UINT32_MAX ms ~
    // 49.7 days -- Atomics.waitAsync clamps there -- and GetTickCount itself
    // wraps every ~49.7 days), which made a far-future task due in the past
    // so it fired immediately instead of staying pending.
    //
    // Precision: the tick source is millisecond-truncated from a monotonic
    // clock, and PAL Sleep is served against CLOCK_REALTIME, so the single
    // Sleep in PopAndWait may wake a few milliseconds before the monotonic
    // deadline (the queue does not re-check after sleeping). The host timer
    // contract is a minimum delay, not sub-millisecond precision.
    unsigned long long m_time;
    unsigned int m_id;

    static std::atomic<unsigned int> s_messageCount;

    MessageBase(const MessageBase&);

public:
    MessageBase(unsigned int time) : m_time(time), m_id(s_messageCount.fetch_add(1, std::memory_order_relaxed)) { }
    virtual ~MessageBase() { }

    void BeginTimer() { m_time += GetTickCount64(); };
    unsigned long long GetTime() { return m_time; };
    unsigned int GetId() { return m_id; };

    virtual HRESULT Call(LPCSTR fileName) = 0;
    virtual bool IsTimerContinuationTask(JsValueRef) const { return false; }
};

template <typename T>
class SortedList 
{
    template <typename U>
    struct DListNode
    {
        U data;
        DListNode<U>* prev;
        DListNode<U>* next;

    public:
        DListNode(const U& data) :
            data(data),
            prev(nullptr),
            next(nullptr)
        { }
    };

public:
    SortedList():
        head(nullptr),
        tail(nullptr)
    {
    }

    ~SortedList()
    {
        while (head != nullptr)
        {
            Remove(head);
        }
    }

    // Append at the tail in O(1) for equal-key FIFO queues (e.g. microtasks).
    void Append(const T& data)
    {
        DListNode<T>* node = new DListNode<T>(data);
        InsertAfter(node, tail);
    }

    // Insert maintaining sorted order with FIFO tie-breaking for equal keys.
    // Fast paths at head/tail make monotonic ascending/equal/descending timer
    // bursts O(1) instead of O(N).
    void Insert(const T& data)
    {
        DListNode<T>* node = new DListNode<T>(data);

        if (tail == nullptr)
        {
            InsertAfter(node, nullptr);
            return;
        }

        // Fast path: data >= tail->data (common case for monotonic timers / equal keys).
        if (!T::LessThan(data, tail->data))
        {
            InsertAfter(node, tail);
            return;
        }

        // Fast path: data < head->data (strictly earlier than earliest entry).
        if (T::LessThan(data, head->data))
        {
            InsertAfter(node, nullptr);
            return;
        }

        // Scan backwards from tail to find the last node with !(data < curr->data).
        DListNode<T>* curr = tail->prev;
        while (curr != nullptr && T::LessThan(data, curr->data))
        {
            curr = curr->prev;
        }

        InsertAfter(node, curr);
    }

    T Pop()
    {
        T data = head->data;
        Remove(head);
        return data;
    }

    // Earliest entry without removing it. Requires a non-empty list.
    const T& Peek() const
    {
        Assert(head != nullptr);
        return head->data;
    }

    template <typename PredicateFn>
    bool Remove(PredicateFn fn)
    {
        DListNode<T>* node = head;

        while (node != nullptr)
        {
            if (fn(node->data))
            {
                Remove(node);
                return true;
            }
            // Advance: without this, a predicate that misses the head (e.g.
            // ClearTimeout for a timer sitting behind earlier-queued
            // messages) spun forever on the first entry -- a latent hang the
            // microtask-before-timer ordering made reachable
            // (gc-async-continuation, doc/e2-gc-async-continuation.md).
            node = node->next;
        }
        return false;
    }

    template <typename PredicateFn>
    void RemoveAll(PredicateFn fn)
    {
        while (head != nullptr)
        {
            fn(head->data);
            Remove(head);
        }
    }

    bool IsEmpty() const
    {
        return head == nullptr;
    }

private:
    void Remove(DListNode<T>* node)
    {
        if (node->prev == nullptr)
        {
            head = node->next;
        }
        else
        {
            node->prev->next = node->next;
        }

        if (node->next == nullptr)
        {
            tail = node->prev;
        }
        else
        {
            node->next->prev = node->prev;
        }

        delete node;
    }

    void InsertAfter(DListNode<T>* newNode, DListNode<T>* node)
    {
        // If the list is empty, set both head and tail to newNode.
        if (head == nullptr)
        {
            Assert(node == nullptr && tail == nullptr);
            head = newNode;
            tail = newNode;
            return;
        }

        // If node is null here, insert before head.
        if (node == nullptr)
        {
            newNode->next = head;
            head->prev = newNode;
            head = newNode;
            return;
        }

        Assert(node);
        newNode->next = node->next;
        newNode->prev = node;

        if (node->next != nullptr)
        {
            node->next->prev = newNode;
        }
        else
        {
            tail = newNode;
        }

        node->next = newNode;
    }

    DListNode<T>* head;
    DListNode<T>* tail;
};

class MessageQueue
{
    struct ListEntry
    {
        // Absolute due time (64-bit monotonic ticks); see MessageBase::m_time.
        unsigned long long time;
        MessageBase* message;

        ListEntry(unsigned long long time, MessageBase* message):
            time(time),
            message(message)
        { }

        // Strict less-than keeps FIFO order for equal due times: SortedList
        // inserts after existing equals, so ties run in insertion order.
        static bool LessThan(const ListEntry& first, const ListEntry& second)
        {
            return first.time < second.time;
        }
    };

    // Separate O(1) FIFO queue for immediate (microtask) messages so queuing
    // many promise continuations does not scan timed entries or vice versa.
    SortedList<ListEntry> m_immediateQueue;
    SortedList<ListEntry> m_timedQueue;

public:
    // `immediate` marks a microtask message (a promise-continuation task
    // raised by the engine's JsSetPromiseContinuationCallback). ES job
    // semantics require the continuation queued by a finished callback to
    // run before the next macrotask (timer), so immediate messages live in
    // `m_immediateQueue` (logical due time 0, drained strictly before any
    // entry in `m_timedQueue`, whose due time is always GetTickCount64()+delay).
    // Without this, an immediate task stamped "now" could land after an
    // already-queued timer whose absolute due time is <= now (a slow earlier
    // callback overrunning a pending timer's deadline, or a timer stamped in
    // the same millisecond), and the continuation would silently run after
    // later timers -- observed as async functions never resuming
    // (gc-async-continuation, doc/e2-gc-async-continuation.md).
    void InsertSorted(MessageBase *message, bool immediate = false)
    {
        if (immediate)
        {
            m_immediateQueue.Append(ListEntry(0, message));
        }
        else
        {
            message->BeginTimer();
            m_timedQueue.Insert(ListEntry(message->GetTime(), message));
        }
    }

    MessageBase* PopAndWait()
    {
        Assert(!IsEmpty());

        if (!m_immediateQueue.IsEmpty())
        {
            return m_immediateQueue.Pop().message;
        }

        ListEntry entry = m_timedQueue.Pop();
        MessageBase *tmp = entry.message;

        // 64-bit difference: a far-future due time (e.g. UINT32_MAX ms from
        // a clamped waitAsync timeout) no longer wraps negative. Sleep takes
        // a DWORD, which covers the maximum possible gap; clamp defensively.
        const long long waitTime = (long long)(tmp->GetTime() - GetTickCount64());
        if (waitTime > 0)
        {
            unsigned long long clamped =
                waitTime > (long long)UINT32_MAX ? (unsigned long long)UINT32_MAX : (unsigned long long)waitTime;
            Sleep((DWORD)clamped);
        }

        return tmp;
    }

    bool IsEmpty() const
    {
        return m_immediateQueue.IsEmpty() && m_timedQueue.IsEmpty();
    }

    // Absolute due time of the earliest message. Requires !IsEmpty(); callers
    // use it to compute how long their event loop may block before a delayed
    // task (e.g. an Atomics.waitAsync timeout/monitor task) becomes due.
    unsigned long long NextDueTime() const
    {
        Assert(!IsEmpty());
        if (!m_immediateQueue.IsEmpty())
        {
            return 0;
        }
        return m_timedQueue.Peek().time;
    }

    // True when the earliest message's absolute due time has passed.
    bool HasDueMessage() const
    {
        if (!m_immediateQueue.IsEmpty())
        {
            return true;
        }
        return !m_timedQueue.IsEmpty() &&
            (long long)(m_timedQueue.Peek().time - GetTickCount64()) <= 0;
    }

    // Pop the earliest message iff it is already due; nullptr otherwise.
    // Unlike PopAndWait this never sleeps, so an event loop can interleave
    // due messages with its own wake events.
    MessageBase* PopDue()
    {
        if (!m_immediateQueue.IsEmpty())
        {
            return m_immediateQueue.Pop().message;
        }
        if (m_timedQueue.IsEmpty() ||
            (long long)(m_timedQueue.Peek().time - GetTickCount64()) > 0)
        {
            return nullptr;
        }
        return m_timedQueue.Pop().message;
    }

    void RemoveById(unsigned int id)
    {
        // Search for the message with the correct id, and delete it.
        auto matchId = [id](const ListEntry& entry)
        {
            MessageBase *msg = entry.message;
            if (msg->GetId() == id)
            {
                delete msg;
                return true;
            }

            return false;
        };
        if (!m_timedQueue.Remove(matchId))
        {
            m_immediateQueue.Remove(matchId);
        }
    }

    void RemoveTimerTask(JsValueRef task)
    {
        auto matchTask = [task](const ListEntry& entry)
        {
            MessageBase *msg = entry.message;
            if (msg->IsTimerContinuationTask(task))
            {
                delete msg;
                return true;
            }
            return false;
        };
        if (!m_timedQueue.Remove(matchTask))
        {
            m_immediateQueue.Remove(matchTask);
        }
    }

    void RemoveAll()
    {
        auto destroyEntry = [](const ListEntry& entry) {
            MessageBase* msg = entry.message;
            delete msg;
        };
        m_immediateQueue.RemoveAll(destroyEntry);
        m_timedQueue.RemoveAll(destroyEntry);
    }

    HRESULT ProcessAll(LPCSTR fileName)
    {
        while(!IsEmpty())
        {
            MessageBase *msg = PopAndWait();

            // Omit checking return value for async function, since it shouldn't affect others.
            msg->Call(fileName);
            delete msg;

            ChakraRTInterface::JsTTDNotifyYield();
        }
        return S_OK;
    }
};

//
// A custom message helper class to assist defining messages handled by callback functions.
//
template <class Func, class CustomBase>
class CustomMessage : public CustomBase
{
private:
    Func m_func;

public:
    CustomMessage(unsigned int time, JsValueRef customArg, const Func& func) :
        CustomBase(time, customArg), m_func(func)
    {}

    virtual HRESULT Call(LPCSTR fileName) override
    {
        return m_func(*this);
    }
};
