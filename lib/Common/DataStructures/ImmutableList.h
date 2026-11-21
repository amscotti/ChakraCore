//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#define IfNullThrowOutOfMemory(result) if(result == nullptr) { Js::Throw::OutOfMemory(); }

namespace regex
{
    template<class T>
    class ImmutableList
    {
        T value;
        ImmutableList<T> * next;
        ImmutableList(T value, ImmutableList<T> * next)
            : value(value), next(next)
        { }

    public:
        // Delete all the nodes in the list (if any).
        static void FreeList(ImmutableList<T> * list, ArenaAllocator * a)
        {
            while (!IsEmpty(list))
            {
                auto nextToDelete = list->next;
                Adelete(a, list);
                list = nextToDelete;
            }
        }

        void FreeList(ArenaAllocator * a)
        {
            if (this == nullptr)
            {
                return;
            }

            auto nextToDelete = this->next;
            Adelete(a, this);
            FreeList(nextToDelete, a);
        }

        // Info:        Return a list with the given value prepended to list (null-safe).
        // Parameters:  list - the list (may be Empty() / nullptr)
        //              value - the value to prepend
        static ImmutableList<T> * Prepend(ImmutableList<T> * list, T value, ArenaAllocator * a)
        {
            return Anew(a, ImmutableList, value, list);
        }

        // Info:        Return a list with the given value prepended to this one.
        // Parameters:  value - the value to prepend
        ImmutableList<T> * Prepend(T value, ArenaAllocator * a)
        {
            return Prepend(this, value, a);
        }

        // Info:        Return a list with the given value appended to list <Note this modifies existing list>
        //              It is required that the tail passed is empty if list is empty otherwise its the last node of list
        static ImmutableList<T> * Append(ImmutableList<T> * list, T value, ArenaAllocator * a, ImmutableList<T> **tail)
        {
#if DBG
            Assert(tail != nullptr);
            Assert((IsEmpty(list) && IsEmpty(*tail)) || IsEmpty((*tail)->next));

            if (!IsEmpty(list))
            {
                auto current = list;
                while(!IsEmpty(current->next))
                {
                    current = current->next;
                }
                Assert(current == (*tail));
            }
#endif

            if (IsEmpty(list))
            {
                *tail = Anew(a, ImmutableList, value, ImmutableList<T>::Empty());
                return *tail;
            }

            (*tail)->next = Anew(a, ImmutableList, value, ImmutableList<T>::Empty());
            *tail = (*tail)->next;
            return list;
        }

        // Info:        Return a list with the given value appended to this one <Note this modifies existing list>
        //              It is required that the tail passed is empty if this is empty list otherwise its the last node of this list
        // Parameters:  value - the value to append
        //              tail - last node of the new list
        ImmutableList<T> * Append(T value, ArenaAllocator * a, ImmutableList<T> **tail)
        {
            return Append(this, value, a, tail);
        }

        // Info:        Return a list with the given array prepended to this one.
        // Parameters:  arr - the array to prepend
        //              count - the elements in the array
        ImmutableList<T> * PrependArray(T * arr, size_t count, ArenaAllocator * a)
        {
            Assert(count > 0);

            // Create the list
            // Using Append here instead of prepend so that we dont need to make another pass just to get the tail and attach the current list at the end of array list
            auto out = ImmutableList<T>::Empty();
            auto tail = out;
            out = AppendArrayToList(out, arr, count, a, &tail);

            Assert(!IsEmpty(tail));
            Assert(IsEmpty(tail->next));
            tail->next = this;
            return out;
        }

        // Info:        Return the list which is resulted by appending array to list <Note this modifies existing list>
        static ImmutableList<T> * AppendArrayToList(ImmutableList<T> * list, T * arr, size_t count, ArenaAllocator * a, ImmutableList<T> **tail)
        {
            auto out = list;
            for(size_t i=0; i<count; i++)
            {
                out = Append(out, arr[i], a, tail);
            }
            return out;
        }

        // Info:        Return the list which is resulted by appending array to the current list<Note this modifies existing list>
        //              It is required that the tail passed is empty if this is empty list otherwise its the last node of this list
        // Parameters:  arr - the array to append
        //              count - the elements in the array
        //              tail - last node of the new list
        ImmutableList<T> * AppendArrayToCurrentList(T * arr, size_t count, ArenaAllocator * a, ImmutableList<T> **tail)
        {
            return AppendArrayToList(this, arr, count, a, tail);
        }

        // Info:        Return a list with the given list appended to this one <Note this modifies existing lists>
        //              returns the list that looks like thisList -> list
        // Parameters:  list - the list to append
        //              tail - optional end ptr so we dont need to traverse the list to find the tail node
        ImmutableList<T> * AppendListToCurrentList(ImmutableList<T> * list, ImmutableList<T> * tail = ImmutableList<T>::Empty()) // TODO : figure out all the tail scenarios
        {
            if (IsEmpty(list))
            {
                return this;
            }

            auto out = this;
            if (IsEmpty(out))
            {
                return list;
            }

            if (IsEmpty(tail))
            {
                // We dont have tail node, find it
                tail = this;
                auto current = this;
                while(!IsEmpty(current))
                {
                    tail = current;
                    current = current->next;
                }
                Assert(IsEmpty(tail->next));
            }
#if DBG
            else
            {
                auto current = this;
                while(!IsEmpty(current->next))
                {
                    current = current->next;
                }
                Assert(current == tail);
            }
#endif
            Assert(!IsEmpty(tail) && IsEmpty(tail->next));
            tail->next = list;
            return out;
        }

        // Info:        Return a new list by calling function f called on each element of this list
        // Parameters:  f - the mapping function
        template<class TOut, class F>
        ImmutableList<TOut> * Select(F f, ArenaAllocator * a)
        {
            auto out = ImmutableList<TOut>::Empty();
            auto tail = out;
            auto current = this;
            while(!IsEmpty(current))
            {
                out = ImmutableList<TOut>::Append(out, f(current->First()), a, &tail);
                current = current->next;
            }
            return out;
        }

        // Info:        Changes the values of current list with the values returned by calling function f on each element  <Note this modifies values in the list>
        // Parameters:  f - the mapping function
        template<class F>
        ImmutableList<T> * SelectInPlace(F f)
        {
            auto current = this;
            while(!IsEmpty(current))
            {
                current->value = f(current->First());
                current = current->next;
            }
            return this;
        }

        // Info:        Return true if any element returns true for the given predicate
        // Parameters:  f - the predicate
        template<class F>
        bool Any(F f)
        {
            return WhereFirst(f).HasValue();
        }

        // Info:        Return a new list by calling function f called on each element of this list. Remove any elements where f returned nullptr.
        // Parameters:  f - the mapping function
        template<class TOut, class F>
        ImmutableList<TOut> * SelectNotNull(F f, ArenaAllocator * a)
        {
            auto out = ImmutableList<TOut>::Empty();
            auto tail = out;
            auto current = this;
            while(!IsEmpty(current))
            {
                auto result = f(current->First());
                if (result!=nullptr)
                {
                    out = ImmutableList<TOut>::Append(out, result, a, &tail);
                }
                current = current->next;
            }
            return out;
        }

        // Info:        Statically cast one list to another. The elements must be statically related
        template<class TOut>
        ImmutableList<TOut> * Cast()
        {
#if DBG
            // Ensure static_cast is valid
            T t = T();
            TOut to = static_cast<TOut>(t);
            to;
#endif
            return reinterpret_cast<ImmutableList<TOut>*>(this);
        }

        // Info:        Call function f for each element of list (null-safe)
        // Parameters:  list - the list (may be Empty() / nullptr)
        //              f - the function to call
        template<class F>
        static void Iterate(ImmutableList<T> * list, F f)
        {
            auto current = list;
            while(!IsEmpty(current))
            {
                f(current->First());
                current = current->next;
            }
        }

        // Info:        Call function f for each element of the list
        // Parameters:  f - the function to call
        template<class F>
        void Iterate(F f)
        {
            Iterate(this, f);
        }

        // Info:        Call function f for each element of the list with an index for each
        // Parameters:  f - the function to call
        //              returns true to continue iterating, false to stop.
        template<class F>
        void IterateWhile(F f)
        {
            auto current = this;
            while(!IsEmpty(current))
            {
                bool shouldContinue = f(current->First());
                if (!shouldContinue)
                {
                    break;
                }
                current = current->next;
            }
        }

        // Info:        Call function f for each element of the list with an index for each
        // Parameters:  f - the function to call
        template<class F>
        void IterateN(F f)
        {
            auto current = this;
            auto index = 0;
            while(!IsEmpty(current))
            {
                f(index, current->First());
                current = current->next;
                ++index;
            }
        }

        // Info:        Call function f for first N elements of the list
        // Parameters:  f - the function to call
        template<class F>
        void IterateFirstN(size_t N, F f)
        {
            Assert(Count() >= N);
            auto current = this;
            while(N > 0)
            {
                f(current->First());
                current = current->next;
                N--;
            }
        }

        // Info:        Call function f for first N elements of the list, f2 on the next element and f3 for remaining elements
        // Parameters:  N -  number of elements to call f with
        //              f - the function to call on first N elements
        //              f2 - the function to call on N+1th element
        //              f3 - the function to call on remaining elements
        template<class F, class F2, class F3>
        void IterateIn3Sets(size_t N, F f, F2 f2, F3 f3)
        {
            Assert(Count() > N);
            auto current = this;
            for (size_t i = 0; i < N; i++)
            {
                f(current->First());
                current = current->next;
            }

            Assert(current != nullptr);
            f2(current->First());
            current = current->next;

            while(current)
            {
                f3(current->First());
                current = current->next;
            }
        }

        // Info:        Iterate two lists at once. Stop when either list runs out.
        // Parameters:  f - the function to call on each pair
        template <class T2, class F> void IterateWith(ImmutableList<T2> *e2,F f)
        {
            auto en1 = this;
            auto en2 = e2;
            while(!IsEmpty(en1) && !ImmutableList<T2>::IsEmpty(en2))
            {
                f(en1->First(),en2->First());
                en1 = en1->GetTail();
                en2 = en2->GetTail();
            }
        }

        // Info:        Iterate over the elements of list and accumulate a result (null-safe)
        // Parameters:  list - the list (may be Empty() / nullptr)
        //              seed - initial accumulator value
        //              f - the function to call for each element
        template <class TAccumulator, class F>
        static TAccumulator Accumulate(ImmutableList<T> * list, TAccumulator seed, F f)
        {
            auto current = list;
            auto accumulated = seed;
            while(!IsEmpty(current))
            {
                accumulated = f(accumulated, current->value);
                current = current->next;
            }
            return accumulated;
        }

        // Info:        Iterate over the elements of the enumerable and accumulate a result
        // Parameters:  f - the function to call for each element
        template <class TAccumulator, class F> TAccumulator Accumulate(TAccumulator seed, F f)
        {
            return Accumulate(this, seed, f);
        }

        // Info:        Sum the elements of the list using values returned from the given function
        // Parameters:  f - the function to call for each element
        template <class TSize, class F> TSize Sum(F f)
        {
            TSize sum = TSize();
            auto current = this;
            while(!IsEmpty(current))
            {
                sum += f(current->value);
                current = current->next;
            }
            return sum;
        }

        // Info:        Return true if f returns true for all elements
        // Parameters:  f - the function to call for each element
        template <class F> bool TrueForAll(F f)
        {
            auto current = this;
            auto stillTrue = true;
            while(!IsEmpty(current) && stillTrue)
            {
                stillTrue = stillTrue && f(current->value);
                current = current->next;
            }
            return stillTrue;
        }

        // Info:        Return list reversed (null-safe)
        static ImmutableList<T> * Reverse(ImmutableList<T> * list, ArenaAllocator * a)
        {
            auto out = ImmutableList<T>::Empty();
            auto current = list;
            while(!IsEmpty(current))
            {
                out = Prepend(out, current->First(), a);
                current = current->next;
            }
            return out;
        }

        // Info:        Return this list reversed
        ImmutableList<T> * Reverse(ArenaAllocator * a)
        {
            return Reverse(this, a);
        }

        // Info:        Return list reversed in-place (null-safe)
        static ImmutableList<T> * ReverseCurrentList(ImmutableList<T> * list)
        {
            auto out = ImmutableList<T>::Empty();
            auto current = list;
            while(!IsEmpty(current))
            {
                auto next = current->next;
                current->next = out;
                out = current;
                current = next;
            }
            return out;
        }

        // Info:        Return this list reversed <Reverses the current list>
        ImmutableList<T> * ReverseCurrentList()
        {
            return ReverseCurrentList(this);
        }

        // Info:        Filter this list by removing the elements where function f returns false
        // Parameters:  f - the predicate function to filter by
        template<class F>
        ImmutableList<T> * Where(F f, ArenaAllocator * a)
        {
            auto out = ImmutableList<T>::Empty();
            auto tail = out;
            auto current = this;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    out = Append(out, head, a, &tail);
                }
                current = current->next;
            }
            return out;
        }

        // Info:        Filter this list by removing the elements where function f returns false <Modifies existing list with removal of nodes that dont satisfy the condition f()>
        // Parameters:  f - the predicate function to filter by
        template<class F>
        ImmutableList<T> * WhereInPlace(F f)
        {
            auto out = ImmutableList<T>::Empty();
            auto tail = out;
            auto current = this;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    if (IsEmpty(out))
                    {
                        // Need to add to the head
                        out = current;
                    }
                    else
                    {
                        // Add to the tail
                        Assert(!IsEmpty(tail));
                        tail->next = current;
                    }
                    tail = current;
                }
                current = current->next;
            }

            if (!IsEmpty(tail))
            {
                tail->next = ImmutableList<T>::Empty();
            }
            return out;
        }

        // Info:        Return a new list by calling function f called on element of this list that satisfy predicate wf
        // Parameters:  wf - the predicate function to filter by
        //              f - the mapping function
        template<class TOut, class selectF, class whereF>
        ImmutableList<TOut> * WhereSelect(whereF wf, selectF sf, ArenaAllocator * a)
        {
            auto out = ImmutableList<TOut>::Empty();
            auto tail = out;
            auto current = this;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (wf(head))
                {
                    out = ImmutableList<TOut>::Append(out, sf(current->First()), a, &tail);
                }
                current = current->next;
            }
            return out;
        }


        // Info:        Run through current list to find the 1 or 0 item that satisfied predicate f
        // Parameters:  f - the predicate function to filter by
        // Info:        If there are 0 or 1 elements, return an option. Throw otherwise.
        template<class TOption, class F>
        Option<TOption> WhereToOption(F f)
        {
            auto out = ImmutableList<T>::Empty();
            auto current = this;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    if (IsEmpty(out))
                    {
                        out = current;
                    }
                    else
                    {
                        // Cannot convert Enumerable to Option because there is more than 1 item in the Enumerable
                       Js::Throw::FatalProjectionError();
                    }
                }
                current = current->next;
            }

            if (IsEmpty(out))
            {
                return nullptr;
            }

            return out->First();
        }

        // Info:        Filter the list down to exactly one item.
        // Parameters:  f - the predicate function to filter by
        template<class F>
        T WhereSingle(F f)
        {
            auto current = this;
            T * result = nullptr;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    Js::VerifyCatastrophic(result==nullptr); // There are multiple matching items.
                    result = &current->value;
                }
                current = current->next;
            }
            Js::VerifyCatastrophic(result!=nullptr); // There are no matching items.
            return *result;
        }

        // Info:        Return the first matching item
        // Parameters:  f - the predicate function to filter by
        template<class F>
        Option<T> WhereFirst(F f)
        {
            auto current = this;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    return &current->value;
                }
                current = current->next;
            }
            return Option<T>();
        }

        // Info:        Return the count of matching items
        // Parameters:  f - the predicate function to filter by
        template<class F>
        size_t CountWhere(F f)
        {
            auto current = this;
            size_t count = 0;
            while(!IsEmpty(current))
            {
                auto head = current->First();
                if (f(head))
                {
                    ++count;
                }
                current = current->next;
            }
            return count;
        }

        // Info:        Return true if any item returns true for f
        // Parameters:  f - the predicate function to filter by
        template<class F>
        bool ContainsWhere(F f)
        {
            return WhereFirst(f).HasValue();
        }

        // Info:        Remove all instances of the given value from current list <Modifies existing list with removal of nodes that has 'value'>
        // Parameters:  value - the value to remove from the list
        ImmutableList<T> * RemoveValueInPlace(T value)
        {
            return WhereInPlace([&](T seen) {return seen != value;});
        }

        // Info:        Return the value from a list with exactly one element. Throw if there are 0 or 2+ elements.
        T ToSingle()
        {
            Js::VerifyCatastrophic(Count()==1);
            return value;
        }

        // Info:        If there are 0 or 1 elements, return an option. Throw otherwise.
        template<class TOption>
        Option<TOption> ToOption()
        {
            auto en = this;
            if (en == Empty())
            {
                return nullptr;
            }
            Js::VerifyCatastrophic(en->next == Empty()); // Cannot convert Enumerable to Option because there is more than 1 item in the Enumerable
            return First();
        }

        // Info:        Return the first element
        T& First()
        {
            Js::VerifyCatastrophic(!IsEmpty(this));
            return value;
        }

        // Info:        Return the last item. Throw if there are none.
        T& Last()
        {
            Js::VerifyCatastrophic(!IsEmpty(this));
            if (IsEmpty(next))
            {
                return value;
            }
            return next->Last(); // Warning: possible stack usage
        }

        // Info:        Return the nth item from the list. Throw if this list isn't long enough.
        T& Nth(size_t n)
        {
            Js::VerifyCatastrophic(!IsEmpty(this));
            if(n==0)
            {
                return value;
            }
            return next->Nth(n-1); // Warning: possible stack usage
        }

        // Info:        Return the rest of the list
        ImmutableList<T> * GetTail()
        {
            return next;
        }

        // Info:        Return the empty list
        static ImmutableList<T> * Empty()
        {
            return nullptr;
        }

        // Info:        Iterate over the elements of an enumerable calling f1 for each and f2 between each.
        // Parameters:  value - the value to remove from the list
        template <class F1, class F2>
        void IterateBetween(F1 f1, F2 f2)
        {
            auto en = this;
            bool more = en!=Empty();

            while(more)
            {
                auto last = en->First();
                f1(last);
                en = en->next;
                more = en!=Empty();
                if (more)
                {
                    auto next = en->First();
                    f2(last,next);
                }
            }
        }

        // Info:        Return the size of list (null-safe)
        static size_t Count(const ImmutableList<T> * list)
        {
            size_t count = 0;
            auto current = list;
            while(!IsEmpty(current))
            {
                ++count;
                current = current->next;
            }

            return count;
        }

        // Info:        Return the size of the list
        size_t Count()
        {
            return Count(this);
        }

        // Info:        Return this list converted to an array in the heap. Get the size by calling Count().
        T ** ToReferenceArrayInHeap(size_t size)
        {
            Assert(size == Count());
            auto result = HeapNewNoThrowArray(T*, size);
            IfNullThrowOutOfMemory(result);

            auto current = this;
            for(size_t index = 0; index<size; ++index)
            {
#pragma warning(push)
#pragma warning(disable:22102)
                result[index] = &current->value;
#pragma warning(pop)
                current = current->next;
            }
            Js::VerifyCatastrophic(IsEmpty(current));
            return result;
        }

        // Info:        Sort the list by the given reference comparer <Modifies existing list such that it is ordered using comparer>
        // Parameters:  comparer - comparer to sort with
        ImmutableList<T> * SortCurrentList(regex::Comparer<T *> * comparer)
        {
            auto size = Count();
            if (size == 0 || size == 1)
            {
                return this;
            }
            auto arr = ToReferenceArrayInHeap(size);
            qsort_s(arr, size, sizeof(T*), [](void* comparer, const void* a, const void* b) {
                return ((regex::Comparer<T *> *)comparer)->Compare(*(T**)a, *(T**)b);
            }, comparer);

            // Convert the reference Array into the List
            auto result = (ImmutableList<T>*)(arr[0] - offsetof(ImmutableList<T>, value));
            auto current = result;
            for(size_t i = 1; i<size; ++i)
            {
                current->next = (ImmutableList<T>*)(arr[i] - offsetof(ImmutableList<T>, value));
                current = current->next;
            }
            current->next = nullptr;
            HeapDeleteArray(size, arr);
            return result;
        }

        // Info:        Sort the list by the given reference comparer in reverse order <Modifies existing list such that it is ordered using comparer>
        // Parameters:  comparer - comparer to sort with
        ImmutableList<T> * ReverseSortCurrentList(regex::Comparer<T *> * comparer)
        {
            auto size = Count();
            if (size == 0 || size == 1)
            {
                return this;
            }
            auto arr = ToReferenceArrayInHeap(size);
            qsort_s(arr, size, sizeof(T*), [](void* comparer, const void* a, const void* b) {
                return ((regex::Comparer<T *> *)comparer)->Compare(*(T**)a, *(T**)b);
            }, comparer);

            // Convert the reference Array into the List
            auto result = (ImmutableList<T>*)(arr[0] - offsetof(ImmutableList<T>, value));
            result->next = nullptr;
            for(size_t i = 1; i<size; ++i)
            {
                auto current = (ImmutableList<T>*)(arr[i] - offsetof(ImmutableList<T>, value));
                current->next = result;
                result = current;
            }
            HeapDeleteArray(size, arr);
            return result;
        }

#ifndef _MSC_VER
       bool __attribute__((noinline)) CheckEq(void* obj1, void *obj2) { return obj1 == obj2; }
#else
#define CheckEq(a, b) (a) == (b)
#endif

        // Info:        Return true if the list is empty (null-safe).
        static bool IsEmpty(const ImmutableList<T> * list)
        {
            return list == Empty();
        }

        // Info:        Return true if the list is empty.
        bool IsEmpty()
        {
            return CheckEq(this, Empty());
        }

        // Info:        Return a list containing the given single value
        // Parameters:  value - the value
        static ImmutableList<T> * OfSingle(T value, ArenaAllocator * a)
        {
            return Anew(a, ImmutableList, value, nullptr);
        }

        // Info:        Group elements that are equal to each other into sublists.
        // NOTE:        This function is N^2 and is currently only suitable for small
        //              lists. If larger lists are needed, please change this function
        //              to group into an intermediate hash table.
        // Parameters:  equals - the value
        template<class FEquals>
        ImmutableList<ImmutableList<T> *> * GroupBy(FEquals equals, ArenaAllocator * a)
        {
            auto out = ImmutableList<ImmutableList<T>*>::Empty();

            auto currentIn = this;
            while(!IsEmpty(currentIn))
            {
                auto currentOut = out;
                bool found = false;
                while(!ImmutableList<ImmutableList<T>*>::IsEmpty(currentOut))
                {
                    ImmutableList<T>* currentOutValue = currentOut->First();

                    if (equals(currentOutValue->First(),currentIn->value))
                    {
                        found = true;
                        currentOut->First() = Prepend(currentOutValue, currentIn->First(), a);
                        break;
                    }

                    currentOut = currentOut->GetTail();
                }
                if (!found)
                {
                    out = ImmutableList<ImmutableList<T>*>::Prepend(out, OfSingle(currentIn->value, a), a);
                }
                currentIn = currentIn->next;
            }

            return out;
        }

        // Info:        Create groups where FEquals returns true for sets of adjacent element. If the list is sorted, this is equivalent to GroupBy.
        // Parameters:  equals - the value
        template<class FEquals>
        ImmutableList<ImmutableList<T>*> * GroupByAdjacentOnCurrentList(FEquals equals, ArenaAllocator * a)
        {
            auto out = ImmutableList<ImmutableList<T>*>::Empty();
            auto tail = out;
            if(!IsEmpty(this))
            {
                auto current = this;
                auto set = current;
                auto setTail = set;
                while(current)
                {
                    if (!equals(current->First(), set->First()))
                    {
                        Assert(!IsEmpty(set) && !IsEmpty(setTail));
                        setTail->next = nullptr;
                        out = ImmutableList<ImmutableList<T>*>::Append(out, set, a, &tail);

                        set = current;
                    }
                    setTail = current;
                    current = current->GetTail();
                }
                Assert(!IsEmpty(set) && !IsEmpty(setTail));
                setTail->next = nullptr;
                out = ImmutableList<ImmutableList<T>*>::Append(out, set, a, &tail);
            }
            return out;
        }
    };

    // Info:        Return a list containing the given single value
    // Parameters:  value - the value
    template<typename T>
    ImmutableList<const T *> * ToImmutableList(const T * value, ArenaAllocator * a)
    {
        return ImmutableList<const T *>::OfSingle(value,a);
    }

    template<class T, class TAllocator, int chunkSize = 4>
    class ImmutableArrayBuilder
    {
    private:
        TAllocator *allocator;
        T * arrayData;
        size_t currentIndex;
        size_t arraySize;
        bool fAutoDeleteArray;

    public:
        ImmutableArrayBuilder(TAllocator *alloc) : allocator(alloc), arrayData(nullptr), currentIndex(0), arraySize(0), fAutoDeleteArray(true)
        {
        }

        ~ImmutableArrayBuilder()
        {
            if (fAutoDeleteArray)
            {
                AllocatorDeleteArray(TAllocator, allocator, arraySize, arrayData);
            }
        }

        void Append(T newEntry)
        {
            // Generate new chunk
            if (currentIndex == arraySize)
            {
                T * newChunk = AllocatorNewArray(TAllocator, allocator, T, arraySize + chunkSize);
                memcpy_s(newChunk, (arraySize + chunkSize) * sizeof(T), arrayData, arraySize * sizeof(T));

                if (arrayData)
                {
                    AllocatorDeleteArray(TAllocator, allocator, arraySize, arrayData);
                }

                arrayData = newChunk;
                arraySize = arraySize + chunkSize;
            }

            Assert(arrayData != nullptr);
            arrayData[currentIndex] = newEntry;
            currentIndex++;
        }

        size_t GetCount()
        {
            return currentIndex;
        }

        T * Get()
        {
            return arrayData;
        }

        void DisableAutoDelete()
        {
            fAutoDeleteArray = false;
        }
    };

    template<int chunkSize>
    class ImmutableStringBuilder
    {
    private:
        struct StringChunk
        {
            LPCWSTR dataPtr[chunkSize];
            StringChunk *next;

            StringChunk() : next(nullptr)
            {
            }
        };

        StringChunk *head;
        StringChunk *tail;
        int currentIndex;
        size_t stringSize;

        // tracking allocated strings based on non-Append(String) calls
        struct AllocatedStringChunk
        {
            LPCWSTR dataPtr;
            AllocatedStringChunk *next;

            AllocatedStringChunk() : next(nullptr)
            {
            }
        };
        AllocatedStringChunk* allocatedStringChunksHead;

    public:
        ImmutableStringBuilder() : head(nullptr), tail(nullptr), currentIndex(chunkSize), stringSize(1), allocatedStringChunksHead(nullptr)
        {
        }

        ~ImmutableStringBuilder()
        {
            // unallocate strings
            AllocatedStringChunk* allocatedStringChunk = this->allocatedStringChunksHead;
            AllocatedStringChunk* nextAllocatedStringChunk;
            while (allocatedStringChunk != nullptr)
            {
                nextAllocatedStringChunk = allocatedStringChunk->next;

                // TODO: Should be HeapDeleteArray, but don't know size
                HeapDelete(allocatedStringChunk->dataPtr,
                           AllocatorDeleteFlags::UnknownSize);

                HeapDelete(allocatedStringChunk);

                allocatedStringChunk = nextAllocatedStringChunk;
            }

            while (head != nullptr)
            {
                auto current = head;
                head = head->next;
                HeapDelete(current);
            }
        }

        void AppendInt32(int32 value);

        void AppendUInt64(uint64 value);

        void AppendWithCopy(_In_z_ LPCWSTR str);

        void AppendBool(bool value)
        {
            this->Append(value ? _u("true") : _u("false"));
        }

        void Append(LPCWSTR str)
        {
            // silently ignore nullptr usage pattern, to avoid cluttering codebase
            if (str == nullptr)
                return;

            size_t newStrSize = stringSize + wcslen(str);
            if (newStrSize < stringSize)
            {
                // Overflow
                Js::Throw::OutOfMemory();
            }

            // Generate new chunk
            if (currentIndex == chunkSize)
            {
                StringChunk *newChunk = HeapNewNoThrow(StringChunk);
                IfNullThrowOutOfMemory(newChunk);

                if (tail == nullptr)
                {
                    Assert(head == nullptr);
                    head = newChunk;
                    tail = newChunk;
                }
                else
                {
                    tail->next = newChunk;
                    tail = newChunk;
                }
                currentIndex = 0;
            }

            Assert(tail != nullptr);
            tail->dataPtr[currentIndex] = str;
            currentIndex++;
            stringSize = newStrSize;
        }

        template<class TAllocator>
        LPCWSTR Get(TAllocator *allocator)
        {
            char16 *str = AllocatorNewArray(TAllocator, allocator, char16, stringSize);
            str[0] = _u('\0');

            auto current = head;
            while (current != nullptr)
            {
                int lastIndex = (current == tail) ? currentIndex : chunkSize;
                for (int index = 0; index < lastIndex; index++)
                {
                    wcscat_s(str, stringSize, current->dataPtr[index]);
                }

                current = current->next;
            }

           return str;
        }

        // Free a string returned by Get()
        template<class TAllocator>
        void FreeString(LPCWSTR str)
        {
            ImmutableList<chunkSize>::FreeString(allocator, str, stringSize);
        }

        template<class TAllocator>
        static void FreeString(TAllocator *allocator, LPCWSTR str, size_t strLength)
        {
            AssertMsg(allocator != nullptr, "allocator != nullptr");
            AssertMsg(str != nullptr, "str != nullptr");
            AllocatorDeleteArray(TAllocator, allocator, strLength, str);
        }
    };

    typedef ImmutableStringBuilder<8> DefaultImmutableStringBuilder;
}

