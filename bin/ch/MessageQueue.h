//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

class MessageBase
{
private:
    // Absolute due time in 64-bit monotonic ticks (PAL GetTickCount64, ms).
    // This must stay 64-bit: 32-bit storage wrapped for large valid delays
    // (the timer continuation callback can receive up to UINT32_MAX ms ≈
    // 49.7 days — Atomics.waitAsync clamps there — and GetTickCount itself
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

    static unsigned int s_messageCount;

    MessageBase(const MessageBase&);

public:
    MessageBase(unsigned int time) : m_time(time), m_id(s_messageCount++) { }
    virtual ~MessageBase() { }

    void BeginTimer() { m_time += GetTickCount64(); };
    unsigned long long GetTime() { return m_time; };
    unsigned int GetId() { return m_id; };

    virtual HRESULT Call(LPCSTR fileName) = 0;
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
        head(nullptr)
    {
    }

    ~SortedList()
    {
        while (head != nullptr)
        {
            Remove(head);
        }
    }

    // Scan through the sorted list
    // Insert before the first node that satisfies the LessThan function
    // This function maintains the invariant that the list is always sorted
    void Insert(const T& data)
    {
        DListNode<T>* curr = head;
        DListNode<T>* node = new DListNode<T>(data);
        DListNode<T>* prev = nullptr; 

        // Now, if we have to insert, we have to insert *after* some node
        while (curr != nullptr)
        {
            if (T::LessThan(data, curr->data))
            {
                break;
            }
            prev = curr;
            curr = curr->next;
        }

        InsertAfter(node, prev);
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
    void Remove(PredicateFn fn)
    {
        DListNode<T>* node = head;

        while (node != nullptr)
        {
            if (fn(node->data))
            {
                Remove(node);
                return;
            }
        }
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

    bool IsEmpty()
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

        if (node->next != nullptr)
        {
            node->next->prev = node->prev;
        }

        delete node;
    }

    void InsertAfter(DListNode<T>* newNode, DListNode<T>* node)
    {
        // If the list is empty, just set head to newNode
        if (head == nullptr)
        {
            Assert(node == nullptr);
            head = newNode;
            return;
        }

        // If node is null here, we must be trying to insert before head
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

        if (node->next)
        {
            node->next->prev = newNode;
        }

        node->next = newNode;
    }

    DListNode<T>* head;
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

    SortedList<ListEntry> m_queue;

public:
    void InsertSorted(MessageBase *message)
    {
        message->BeginTimer();
        unsigned long long time = message->GetTime();
        m_queue.Insert(ListEntry(time, message));
    }

    MessageBase* PopAndWait()
    {
        Assert(!m_queue.IsEmpty());

        ListEntry entry = m_queue.Pop();
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

    bool IsEmpty()
    {
        return m_queue.IsEmpty();
    }

    // Absolute due time of the earliest message. Requires !IsEmpty(); callers
    // use it to compute how long their event loop may block before a delayed
    // task (e.g. an Atomics.waitAsync timeout/monitor task) becomes due.
    unsigned long long NextDueTime()
    {
        return m_queue.Peek().time;
    }

    // True when the earliest message's absolute due time has passed.
    bool HasDueMessage()
    {
        return !m_queue.IsEmpty() &&
            (long long)(m_queue.Peek().time - GetTickCount64()) <= 0;
    }

    // Pop the earliest message iff it is already due; nullptr otherwise.
    // Unlike PopAndWait this never sleeps, so an event loop can interleave
    // due messages with its own wake events.
    MessageBase* PopDue()
    {
        if (!HasDueMessage())
        {
            return nullptr;
        }
        ListEntry entry = m_queue.Pop();
        return entry.message;
    }

    void RemoveById(unsigned int id)
    {
        // Search for the message with the correct id, and delete it. Can be updated
        // to a hash to improve speed, if necessary.
        m_queue.Remove([id](const ListEntry& entry) 
        {
            MessageBase *msg = entry.message;
            if(msg->GetId() == id)
            {
                delete msg;
                return true;
            }

            return false;
        });
    }

    void RemoveAll()
    {
        m_queue.RemoveAll([](const ListEntry& entry) { 
            MessageBase* msg = entry.message;
            delete msg;
        });
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
