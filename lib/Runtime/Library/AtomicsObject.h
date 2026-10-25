//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//  Implements Atomics according to http://tc39.github.io/ecmascript_sharedmem/shmem.html
//----------------------------------------------------------------------------

#pragma once
namespace Js
{
    class AtomicsObject
    {
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo Add;
            static FunctionInfo And;
            static FunctionInfo CompareExchange;
            static FunctionInfo Exchange;
            static FunctionInfo IsLockFree;
            static FunctionInfo Load;
            static FunctionInfo Or;
            static FunctionInfo Pause;
            static FunctionInfo Store;
            static FunctionInfo Sub;
            static FunctionInfo Wait;
            static FunctionInfo WaitAsync;
            static FunctionInfo Notify;
            static FunctionInfo Xor;
        };

        static Var EntryAdd(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAnd(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryCompareExchange(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryExchange(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryIsLockFree(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryLoad(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryOr(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryPause(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryStore(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySub(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryWait(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryWaitAsync(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryNotify(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryXor(RecyclableObject* function, CallInfo callInfo, ...);

    private:
        // ES2020+ implements the read-modify-write family on both shared and
        // non-shared integer TypedArrays. `requireShared` retains the
        // shared-only brand for Atomics.wait.
        static ArrayBufferBase* ValidateIntegerTypedArray(Var typedArray, ScriptContext *scriptContext, bool onlyInt32, bool requireShared, bool writeAccess = false);
        static uint32 ValidateAtomicAccess(Var typedArray, Var requestIndex, ScriptContext *scriptContext);
        static void CheckAtomicAccessStillValid(TypedArrayBase *typedArray, uint32 accessIndex, ScriptContext *scriptContext, const char16 *funcName);

        static TypedArrayBase * ValidateAndGetTypedArray(Var typedArray, Var index, _Out_ uint32 *accessIndex, ScriptContext *scriptContext, bool onlyInt32 = false, bool requireShared = false, bool writeAccess = false);
        static Var CreateWaitAsyncResult(ScriptContext *scriptContext, bool async, Var value);
    };
}
