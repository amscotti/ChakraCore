//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class JavascriptIterator
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo SymbolIterator;
            static FunctionInfo NewInstance;
            static FunctionInfo From;
            static FunctionInfo Map;
            static FunctionInfo Filter;
            static FunctionInfo Take;
            static FunctionInfo Drop;
            static FunctionInfo FlatMap;
            static FunctionInfo Reduce;
            static FunctionInfo ToArray;
            static FunctionInfo ForEach;
            static FunctionInfo Some;
            static FunctionInfo Every;
            static FunctionInfo Find;
            static FunctionInfo GetterConstructor;
            static FunctionInfo SetterConstructor;
            static FunctionInfo GetterToStringTag;
            static FunctionInfo SetterToStringTag;
        };

        static Var EntrySymbolIterator(RecyclableObject* function, CallInfo callInfo, ...);
        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryFrom(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryMap(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryFilter(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryTake(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryDrop(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryFlatMap(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryReduce(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToArray(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryForEach(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySome(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryEvery(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryFind(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterConstructor(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetterConstructor(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterToStringTag(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetterToStringTag(RecyclableObject* function, CallInfo callInfo, ...);
    };

    enum class IteratorHelperKind
    {
        Map,
        Filter,
        Take,
        Drop,
        FlatMap,
        Wrap,
    };

    // Iterator Helper and WrapForValidIterator objects. Both hold an
    // underlying Iterator Record ([[Iterator]], cached [[NextMethod]],
    // [[Done]]) and, for helpers, the callback and the inner record used
    // by flatMap. All JS references are held in Field() members of this
    // Recycler-allocated DynamicObject subclass, following the
    // JavascriptAsyncFromSyncIterator pattern, so the recycler traces the
    // underlying iterator(s) and callback for exactly the helper's life.
    // `started` stays false until the first next() (suspended-start).
    // `executing` mirrors the generator brand while a started helper runs:
    // reentrant next/return then throws TypeError. A suspended-start
    // return() sets `completed` before IteratorClose and does not set
    // `executing`, so reentry observes a completed generator and returns
    // a done result (ECMA-262 %IteratorHelperPrototype%.return).
    class JavascriptIteratorHelper : public DynamicObject
    {
    private:
        Field(RecyclableObject*) iterator;
        Field(Var) nextMethod;
        Field(Var) callback;
        Field(RecyclableObject*) innerIterator;
        Field(Var) innerNextMethod;
        Field(double) counter;
        Field(double) remaining;
        Field(IteratorHelperKind) kind;
        Field(bool) iteratorDone;
        Field(bool) innerDone;
        Field(bool) hasInner;
        Field(bool) completed;
        Field(bool) executing;
        Field(bool) started;

    protected:
        DEFINE_VTABLE_CTOR(JavascriptIteratorHelper, DynamicObject);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptIteratorHelper);

    public:
        JavascriptIteratorHelper(DynamicType* type, IteratorHelperKind kind, RecyclableObject* iterator, Var nextMethod, Var callback, double remaining);

        IteratorHelperKind GetKind() const { return kind; }
        bool IsWrap() const { return kind == IteratorHelperKind::Wrap; }

        class EntryInfo
        {
        public:
            static FunctionInfo Next;
            static FunctionInfo Return;
            static FunctionInfo WrapNext;
            static FunctionInfo WrapReturn;
        };

        static Var EntryNext(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryReturn(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryWrapNext(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryWrapReturn(RecyclableObject* function, CallInfo callInfo, ...);

    private:
        // Advance one step. Returns true and sets *outValue, or returns
        // false when exhausted. Abrupt completions propagate as the
        // engine's JavascriptException; EntryNext owns closing/completion.
        bool Advance(ScriptContext* scriptContext, Var* outValue);
        bool StepUnderlying(ScriptContext* scriptContext, Var* outValue);
        bool StepInner(ScriptContext* scriptContext, Var* outValue);
        void CloseUnderlyingNormal(ScriptContext* scriptContext);
        void CloseInnerNormal(ScriptContext* scriptContext);
        void CloseAllSwallow(ScriptContext* scriptContext);

        friend class JavascriptIterator;
    };

    template <> bool VarIsImpl<JavascriptIteratorHelper>(RecyclableObject* obj);
} // namespace Js
