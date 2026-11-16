//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "ChakraCore.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define JSRT_P3_CHECK(cond, msg)                                              \
    do                                                                        \
    {                                                                         \
        if (!(cond))                                                          \
        {                                                                     \
            fprintf(stderr, "JSRT_P3_FAIL (%s:%d): %s\n",                     \
                    __FILE__, __LINE__, (msg));                               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define JSRT_P3_CHECK_EC(cmd)                                                 \
    do                                                                        \
    {                                                                         \
        JsErrorCode p3_ec = (cmd);                                            \
        if (p3_ec != JsNoError)                                               \
        {                                                                     \
            fprintf(stderr, "JSRT_P3_FAIL (%s:%d): %s returned %d\n",         \
                    __FILE__, __LINE__, #cmd, (int)p3_ec);                    \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace JsrtPhase3Tests
{
    struct ExternalAbState
    {
        int finalizeCount;
        unsigned int magic;
        unsigned char bytes[16];
    };

    static void CHAKRA_CALLBACK ExternalAbFinalizer(void* callbackState)
    {
        ExternalAbState* state = static_cast<ExternalAbState*>(callbackState);
        if (state == nullptr || state->magic != 0xA5A55A5A)
        {
            abort();
        }
        state->finalizeCount++;
    }

#if defined(__clang__) || defined(__GNUC__)
    __attribute__((noinline))
#endif
    static JsErrorCode CreateUnrootedExternalAb(ExternalAbState* state)
    {
        // Advance past slot 0 on a newly mapped Recycler page so stale stack
        // slots from an earlier runtime cannot conservatively pin this object.
        for (int i = 0; i < 8; i++)
        {
            JsValueRef filler = JS_INVALID_REFERENCE;
            JsErrorCode fillerEc = JsCreateArrayBuffer(16, &filler);
            if (fillerEc != JsNoError)
            {
                return fillerEc;
            }
        }
        JsValueRef ab = JS_INVALID_REFERENCE;
        return JsCreateExternalArrayBuffer(
            state->bytes,
            (unsigned int)sizeof(state->bytes),
            ExternalAbFinalizer,
            state,
            &ab);
    }

#if defined(__clang__) || defined(__GNUC__)
    __attribute__((noinline))
#endif
    static void ScrubStack()
    {
        volatile unsigned char buf[8192];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = 0;
        }
    }

    static int RunScriptUtf8(const char* script, JsSourceContext sourceContext, JsValueRef* outResult)
    {
        JsValueRef fname = JS_INVALID_REFERENCE;
        JsValueRef source = JS_INVALID_REFERENCE;
        JsValueRef result = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreateString("phase3-test", 11, &fname));
        JSRT_P3_CHECK_EC(JsCreateExternalArrayBuffer(
            const_cast<char*>(script), (unsigned int)strlen(script),
            nullptr, nullptr, &source));
        JSRT_P3_CHECK_EC(JsRun(source, sourceContext, fname, JsParseScriptAttributeNone, &result));
        if (outResult != nullptr)
        {
            *outResult = result;
        }
        return 0;
    }

    static int CopyJsString(JsValueRef strVal, std::string* out)
    {
        size_t len = 0;
        JSRT_P3_CHECK_EC(JsCopyString(strVal, nullptr, 0, &len));
        out->assign(len, '\0');
        if (len > 0)
        {
            JSRT_P3_CHECK_EC(JsCopyString(strVal, &(*out)[0], len, nullptr));
        }
        return 0;
    }

    static int TestExternalArrayBufferLifetimes()
    {
        // 1a. Normal GC finalization without JsDisposeRuntime.
        ExternalAbState gcState = { 0, 0xA5A55A5A, { 1, 2, 3 } };
        {
            JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
            JsContextRef ctx = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
            JSRT_P3_CHECK_EC(JsCreateContext(rt, &ctx));
            JSRT_P3_CHECK_EC(JsSetCurrentContext(ctx));

            JSRT_P3_CHECK_EC(CreateUnrootedExternalAb(&gcState));
            for (int gcIter = 0; gcIter < 5 && gcState.finalizeCount == 0; gcIter++)
            {
                ScrubStack();
                JSRT_P3_CHECK_EC(JsCollectGarbage(rt));
            }
            JSRT_P3_CHECK(gcState.finalizeCount == 1,
                "normal GC must finalize unrooted external ArrayBuffer once");

            JSRT_P3_CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
            JSRT_P3_CHECK_EC(JsDisposeRuntime(rt));
            JSRT_P3_CHECK(gcState.finalizeCount == 1,
                "runtime disposal must not double-finalize GC'd external ArrayBuffer");
        }

        // 1b. JsDetachArrayBuffer on external AB + JsDisposeRuntime single-finalizer,
        //     ArrayBuffer.prototype.transfer() single-finalizer, and 0-byte nullptr transfer.
        ExternalAbState detachState = { 0, 0xA5A55A5A, { 10, 20, 30 } };
        ExternalAbState transferState = { 0, 0xA5A55A5A, { 40, 50, 60 } };
        ExternalAbState nullZeroState = { 0, 0xA5A55A5A, { 0 } };
        {
            JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
            JsContextRef ctx = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
            JSRT_P3_CHECK_EC(JsCreateContext(rt, &ctx));
            JSRT_P3_CHECK_EC(JsSetCurrentContext(ctx));

            JsValueRef abDetach = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateExternalArrayBuffer(
                detachState.bytes, (unsigned int)sizeof(detachState.bytes),
                ExternalAbFinalizer, &detachState, &abDetach));
            JSRT_P3_CHECK_EC(JsDetachArrayBuffer(abDetach));
            JSRT_P3_CHECK(JsDetachArrayBuffer(abDetach) == JsErrorInvalidArgument,
                "second JsDetachArrayBuffer must fail with JsErrorInvalidArgument");

            BYTE* detachedPtr = reinterpret_cast<BYTE*>(0x1);
            unsigned int detachedLen = 999;
            JSRT_P3_CHECK_EC(JsGetArrayBufferStorage(abDetach, &detachedPtr, &detachedLen));
            JSRT_P3_CHECK(detachedPtr == nullptr && detachedLen == 0,
                "detached external ArrayBuffer must report NULL storage and 0 length");

            JsValueRef global = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsGetGlobalObject(&global));

            JsValueRef abTransfer = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateExternalArrayBuffer(
                transferState.bytes, (unsigned int)sizeof(transferState.bytes),
                ExternalAbFinalizer, &transferState, &abTransfer));
            JsPropertyIdRef xferProp = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreatePropertyId("extTransferAb", 13, &xferProp));
            JSRT_P3_CHECK_EC(JsSetProperty(global, xferProp, abTransfer, true));

            JsValueRef abNullZero = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateExternalArrayBuffer(
                nullptr, 0, ExternalAbFinalizer, &nullZeroState, &abNullZero));
            JsPropertyIdRef nullProp = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreatePropertyId("extNullZeroAb", 13, &nullProp));
            JSRT_P3_CHECK_EC(JsSetProperty(global, nullProp, abNullZero, true));

            if (RunScriptUtf8(
                    "var moved = extTransferAb.transfer(8);"
                    "if (!extTransferAb.detached || moved.byteLength !== 8 || new Uint8Array(moved)[0] !== 40) throw new Error('transfer failed');"
                    "var movedNull = extNullZeroAb.transfer(4);"
                    "if (!extNullZeroAb.detached || movedNull.byteLength !== 4) throw new Error('null transfer failed');"
                    "moved = null; movedNull = null;",
                    JS_SOURCE_CONTEXT_NONE, nullptr) != 0)
            {
                return 1;
            }

            JSRT_P3_CHECK(transferState.finalizeCount == 1,
                "ArrayBuffer.prototype.transfer must invoke external finalizer once via detached state");
            JSRT_P3_CHECK(nullZeroState.finalizeCount == 1,
                "0-byte nullptr external ArrayBuffer transfer must invoke finalizer once");

            JSRT_P3_CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
            JSRT_P3_CHECK_EC(JsDisposeRuntime(rt));

            JSRT_P3_CHECK(detachState.finalizeCount == 1,
                "JsDetachArrayBuffer + JsDisposeRuntime must invoke external finalizer exactly once");
            JSRT_P3_CHECK(transferState.finalizeCount == 1,
                "JsDisposeRuntime must not double-finalize transferred external ArrayBuffer");
            JSRT_P3_CHECK(nullZeroState.finalizeCount == 1,
                "JsDisposeRuntime must not double-finalize transferred 0-byte external ArrayBuffer");
        }
        return 0;
    }

    static int TestTypedArrayAbi()
    {
        JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
        JsContextRef ctx = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
        JSRT_P3_CHECK_EC(JsCreateContext(rt, &ctx));
        JSRT_P3_CHECK_EC(JsSetCurrentContext(ctx));

        struct Case
        {
            JsTypedArrayType type;
            int elemSize;
        };
        const Case cases[] = {
            { JsArrayTypeFloat16, 2 },
            { JsArrayTypeBigInt64, 8 },
            { JsArrayTypeBigUint64, 8 },
        };

        JsValueRef backingAb = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreateArrayBuffer(64, &backingAb));

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            const JsTypedArrayType wantType = cases[i].type;
            const int wantElemSize = cases[i].elemSize;

            // Standalone JsCreateTypedArray
            JsValueRef ta = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateTypedArray(wantType, JS_INVALID_REFERENCE, 0, 4, &ta));

            JsValueType vt = JsUndefined;
            JSRT_P3_CHECK_EC(JsGetValueType(ta, &vt));
            JSRT_P3_CHECK(vt == JsTypedArray, "JsGetValueType must return JsTypedArray");

            JsTypedArrayType gotType = JsArrayTypeInt8;
            JsValueRef gotBuf = JS_INVALID_REFERENCE;
            unsigned int gotOffset = 999;
            unsigned int gotByteLen = 999;
            JSRT_P3_CHECK_EC(JsGetTypedArrayInfo(ta, &gotType, &gotBuf, &gotOffset, &gotByteLen));
            JSRT_P3_CHECK(gotType == wantType, "JsGetTypedArrayInfo arrayType mismatch");
            JSRT_P3_CHECK(gotBuf != JS_INVALID_REFERENCE, "JsGetTypedArrayInfo arrayBuffer null");
            JSRT_P3_CHECK(gotOffset == 0, "JsGetTypedArrayInfo byteOffset mismatch");
            JSRT_P3_CHECK(gotByteLen == (unsigned int)(4 * wantElemSize), "JsGetTypedArrayInfo byteLength mismatch");

            BYTE* storPtr = nullptr;
            unsigned int storLen = 0;
            JsTypedArrayType storType = JsArrayTypeInt8;
            int storElemSize = 0;
            JSRT_P3_CHECK_EC(JsGetTypedArrayStorage(ta, &storPtr, &storLen, &storType, &storElemSize));
            JSRT_P3_CHECK(storPtr != nullptr, "JsGetTypedArrayStorage buffer null");
            JSRT_P3_CHECK(storLen == (unsigned int)(4 * wantElemSize), "JsGetTypedArrayStorage bufferLength mismatch");
            JSRT_P3_CHECK(storType == wantType, "JsGetTypedArrayStorage typedArrayType mismatch");
            JSRT_P3_CHECK(storElemSize == wantElemSize, "JsGetTypedArrayStorage elementSize mismatch");

            // ArrayBuffer-backed JsCreateTypedArray with non-zero byteOffset
            JsValueRef view = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateTypedArray(wantType, backingAb, 16, 3, &view));
            JSRT_P3_CHECK_EC(JsGetTypedArrayInfo(view, &gotType, &gotBuf, &gotOffset, &gotByteLen));
            JSRT_P3_CHECK(gotType == wantType && gotOffset == 16 && gotByteLen == (unsigned int)(3 * wantElemSize),
                "ArrayBuffer-backed TypedArray info mismatch");
            JSRT_P3_CHECK_EC(JsGetTypedArrayStorage(view, &storPtr, &storLen, &storType, &storElemSize));
            JSRT_P3_CHECK(storPtr != nullptr && storLen == (unsigned int)(3 * wantElemSize) &&
                storType == wantType && storElemSize == wantElemSize,
                "ArrayBuffer-backed TypedArray storage mismatch");

            // External indexed properties round-trip
            uint64_t extData[2] = { 0x1122334455667788ULL, 0x8877665544332211ULL };
            JsValueRef extObj = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreateObject(&extObj));
            JSRT_P3_CHECK_EC(JsSetIndexedPropertiesToExternalData(extObj, extData, wantType, 2));
            void* outExtPtr = nullptr;
            JsTypedArrayType outExtType = JsArrayTypeInt8;
            unsigned int outExtLen = 0;
            JSRT_P3_CHECK_EC(JsGetIndexedPropertiesExternalData(extObj, &outExtPtr, &outExtType, &outExtLen));
            JSRT_P3_CHECK(outExtPtr == extData && outExtType == wantType && outExtLen == 2,
                "JsGetIndexedPropertiesExternalData mismatch");
        }

        // JS-constructed Float16Array, BigInt64Array, BigUint64Array inspected via JsRT
        JsValueRef jsArrays = JS_INVALID_REFERENCE;
        if (RunScriptUtf8(
                "({ f16: new Float16Array([1.5, 2.5]), bi64: new BigInt64Array([-1n, 2n]), bu64: new BigUint64Array([3n, 4n]) })",
                JS_SOURCE_CONTEXT_NONE, &jsArrays) != 0)
        {
            return 1;
        }
        struct JsCase
        {
            const char* name;
            JsTypedArrayType wantType;
            int wantElemSize;
        };
        const JsCase jsCases[] = {
            { "f16", JsArrayTypeFloat16, 2 },
            { "bi64", JsArrayTypeBigInt64, 8 },
            { "bu64", JsArrayTypeBigUint64, 8 },
        };
        for (size_t i = 0; i < sizeof(jsCases) / sizeof(jsCases[0]); i++)
        {
            JsPropertyIdRef prop = JS_INVALID_REFERENCE;
            JsValueRef ta = JS_INVALID_REFERENCE;
            JSRT_P3_CHECK_EC(JsCreatePropertyId(jsCases[i].name, strlen(jsCases[i].name), &prop));
            JSRT_P3_CHECK_EC(JsGetProperty(jsArrays, prop, &ta));

            JsTypedArrayType gotType = JsArrayTypeInt8;
            JSRT_P3_CHECK_EC(JsGetTypedArrayInfo(ta, &gotType, nullptr, nullptr, nullptr));
            JSRT_P3_CHECK(gotType == jsCases[i].wantType, "JS-created TypedArray info type mismatch");

            BYTE* ptr = nullptr;
            unsigned int len = 0;
            JsTypedArrayType storType = JsArrayTypeInt8;
            int elemSize = 0;
            JSRT_P3_CHECK_EC(JsGetTypedArrayStorage(ta, &ptr, &len, &storType, &elemSize));
            JSRT_P3_CHECK(ptr != nullptr && len == (unsigned int)(2 * jsCases[i].wantElemSize) &&
                storType == jsCases[i].wantType && elemSize == jsCases[i].wantElemSize,
                "JS-created TypedArray storage mismatch");
        }

        // Invalid JsTypedArrayType must return JsErrorInvalidArgument
        JsValueRef badTa = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK(JsCreateTypedArray((JsTypedArrayType)99, JS_INVALID_REFERENCE, 0, 1, &badTa) == JsErrorInvalidArgument,
            "invalid JsTypedArrayType must fail with JsErrorInvalidArgument");

        JSRT_P3_CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
        JSRT_P3_CHECK_EC(JsDisposeRuntime(rt));
        return 0;
    }

    struct ContinuationState
    {
        bool timerFired;
        unsigned int lastDelayMs;
        std::vector<JsValueRef> timerTasks;
        std::vector<JsValueRef> microtasks;
    };

    static void CHAKRA_CALLBACK TestTimerContinuation(JsValueRef task, unsigned int delayMilliseconds, void* callbackState)
    {
        ContinuationState* st = static_cast<ContinuationState*>(callbackState);
        if (st == nullptr || task == JS_INVALID_REFERENCE)
        {
            abort();
        }
        st->timerFired = true;
        st->lastDelayMs = delayMilliseconds;
        if (JsAddRef(task, nullptr) != JsNoError)
        {
            abort();
        }
        st->timerTasks.push_back(task);
    }

    static void CHAKRA_CALLBACK TestPromiseContinuation(JsValueRef task, void* callbackState)
    {
        ContinuationState* st = static_cast<ContinuationState*>(callbackState);
        if (st == nullptr || task == JS_INVALID_REFERENCE)
        {
            abort();
        }
        if (JsAddRef(task, nullptr) != JsNoError)
        {
            abort();
        }
        st->microtasks.push_back(task);
    }

    static int PumpContinuations(ContinuationState* st, JsValueRef global)
    {
        while (!st->microtasks.empty())
        {
            std::vector<JsValueRef> batch;
            batch.swap(st->microtasks);
            for (size_t i = 0; i < batch.size(); i++)
            {
                JsValueRef unused = JS_INVALID_REFERENCE;
                JsErrorCode ec = JsCallFunction(batch[i], &global, 1, &unused);
                JsRelease(batch[i], nullptr);
                if (ec != JsNoError)
                {
                    for (size_t j = i + 1; j < batch.size(); j++)
                    {
                        JsRelease(batch[j], nullptr);
                    }
                    return 1;
                }
            }
        }
        return 0;
    }

    static int TestTimerContinuationCallback()
    {
        JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
        JsContextRef ctx = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
        JSRT_P3_CHECK_EC(JsCreateContext(rt, &ctx));
        JSRT_P3_CHECK_EC(JsSetCurrentContext(ctx));

        ContinuationState st = { false, 0, {}, {} };
        JSRT_P3_CHECK_EC(JsSetPromiseContinuationCallback(TestPromiseContinuation, &st));
        JSRT_P3_CHECK_EC(JsSetTimerContinuationCallback(TestTimerContinuation, &st));

        JsValueRef global = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsGetGlobalObject(&global));

        if (RunScriptUtf8(
                "var __waitStatus = 'pending';"
                "var sab = new SharedArrayBuffer(4);"
                "var ia = new Int32Array(sab);"
                "var r = Atomics.waitAsync(ia, 0, 0, 5);"
                "if (!r.async) throw new Error('expected async wait');"
                "r.value.then(function(v) { __waitStatus = v; });",
                JS_SOURCE_CONTEXT_NONE, nullptr) != 0)
        {
            return 1;
        }

        JSRT_P3_CHECK(st.timerFired, "JsSetTimerContinuationCallback must fire for finite Atomics.waitAsync");
        JSRT_P3_CHECK(st.lastDelayMs == 5, "timer continuation delayMilliseconds mismatch");
        JSRT_P3_CHECK(!st.timerTasks.empty(), "timer task list must be non-empty");

        while (!st.timerTasks.empty())
        {
            std::vector<JsValueRef> tasks;
            tasks.swap(st.timerTasks);
            for (size_t i = 0; i < tasks.size(); i++)
            {
                JsValueRef unused = JS_INVALID_REFERENCE;
                JsErrorCode ec = JsCallFunction(tasks[i], &global, 1, &unused);
                JsRelease(tasks[i], nullptr);
                JSRT_P3_CHECK(ec == JsNoError, "timer continuation task execution failed");
            }
            JSRT_P3_CHECK(PumpContinuations(&st, global) == 0, "microtask pump failed");
        }

        JsPropertyIdRef prop = JS_INVALID_REFERENCE;
        JsValueRef statusVal = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreatePropertyId("__waitStatus", 12, &prop));
        JSRT_P3_CHECK_EC(JsGetProperty(global, prop, &statusVal));
        std::string statusStr;
        JSRT_P3_CHECK(CopyJsString(statusVal, &statusStr) == 0, "copy __waitStatus failed");
        JSRT_P3_CHECK(statusStr == "timed-out", "Atomics.waitAsync timer callback must resolve promise to timed-out");

        JSRT_P3_CHECK_EC(JsSetTimerContinuationCallback(nullptr, nullptr));
        JSRT_P3_CHECK_EC(JsSetPromiseContinuationCallback(nullptr, nullptr));
        JSRT_P3_CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
        JSRT_P3_CHECK_EC(JsDisposeRuntime(rt));
        return 0;
    }

    struct PendingModuleFetch
    {
        JsModuleRecord record;
        std::string specifier;
        std::string source;
    };

    struct ModuleHostState
    {
        bool fetchWithAttrsCalled;
        bool sawJsonAttribute;
        bool sawUndefinedAttrsForPlain;
        bool notifyReadyCalled;
        bool completionErrored;
        std::vector<PendingModuleFetch> pendingFetches;
        std::vector<JsModuleRecord> readyModules;
        std::vector<JsValueRef> microtasks;
    };

    static ModuleHostState* g_moduleState = nullptr;

    static JsErrorCode CHAKRA_CALLBACK TestFetchFromScriptWithAttrs(
        JsSourceContext dwReferencingSourceContext,
        JsValueRef specifier,
        JsValueRef attributes,
        JsModuleRecord* dependentModuleRecord)
    {
        if (g_moduleState == nullptr || dependentModuleRecord == nullptr)
        {
            return JsErrorInvalidArgument;
        }
        *dependentModuleRecord = JS_INVALID_REFERENCE;
        if (dwReferencingSourceContext != 7)
        {
            return JsErrorInvalidArgument;
        }
        g_moduleState->fetchWithAttrsCalled = true;

        std::string specStr;
        if (CopyJsString(specifier, &specStr) != 0)
        {
            return JsErrorInvalidArgument;
        }

        JsValueType attrType = JsUndefined;
        if (JsGetValueType(attributes, &attrType) != JsNoError)
        {
            return JsErrorInvalidArgument;
        }

        std::string moduleSource;
        if (specStr == "./virtual.json")
        {
            if (attrType != JsObject)
            {
                return JsErrorInvalidArgument;
            }
            JsPropertyIdRef typeProp = JS_INVALID_REFERENCE;
            JsValueRef typeVal = JS_INVALID_REFERENCE;
            if (JsCreatePropertyId("type", 4, &typeProp) != JsNoError ||
                JsGetProperty(attributes, typeProp, &typeVal) != JsNoError)
            {
                return JsErrorInvalidArgument;
            }
            std::string typeStr;
            if (CopyJsString(typeVal, &typeStr) != 0 || typeStr != "json")
            {
                return JsErrorInvalidArgument;
            }
            g_moduleState->sawJsonAttribute = true;
            moduleSource = "export default { ok: 42 };";
        }
        else if (specStr == "./plain.js")
        {
            if (attrType != JsUndefined)
            {
                return JsErrorInvalidArgument;
            }
            g_moduleState->sawUndefinedAttrsForPlain = true;
            moduleSource = "export const val = 99;";
        }
        else
        {
            return JsErrorInvalidArgument;
        }

        JsModuleRecord rec = JS_INVALID_REFERENCE;
        JsErrorCode ec = JsInitializeModuleRecord(nullptr, specifier, &rec);
        if (ec != JsNoError)
        {
            return ec;
        }
        if (JsAddRef(rec, nullptr) != JsNoError)
        {
            return JsErrorFatal;
        }
        g_moduleState->pendingFetches.push_back({ rec, specStr, moduleSource });
        *dependentModuleRecord = rec;
        return JsNoError;
    }

    static JsErrorCode CHAKRA_CALLBACK TestNotifyModuleReady(JsModuleRecord referencingModule, JsValueRef exceptionVar)
    {
        if (g_moduleState == nullptr)
        {
            return JsErrorFatal;
        }
        g_moduleState->notifyReadyCalled = true;
        if (exceptionVar != nullptr)
        {
            g_moduleState->completionErrored = true;
            return JsNoError;
        }
        if (JsAddRef(referencingModule, nullptr) != JsNoError)
        {
            return JsErrorFatal;
        }
        g_moduleState->readyModules.push_back(referencingModule);
        return JsNoError;
    }

    static JsErrorCode CHAKRA_CALLBACK TestReportModuleCompletion(JsModuleRecord module, JsValueRef exception)
    {
        (void)module;
        if (g_moduleState != nullptr && exception != nullptr)
        {
            g_moduleState->completionErrored = true;
        }
        return JsNoError;
    }

    static void CHAKRA_CALLBACK TestModulePromiseContinuation(JsValueRef task, void* callbackState)
    {
        ModuleHostState* st = static_cast<ModuleHostState*>(callbackState);
        if (st == nullptr || task == JS_INVALID_REFERENCE)
        {
            abort();
        }
        if (JsAddRef(task, nullptr) != JsNoError)
        {
            abort();
        }
        st->microtasks.push_back(task);
    }

    static int TestFetchImportedModuleFromScriptWithAttributes()
    {
        JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
        JsContextRef ctx = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
        JSRT_P3_CHECK_EC(JsCreateContext(rt, &ctx));
        JSRT_P3_CHECK_EC(JsSetCurrentContext(ctx));

        ModuleHostState st = { false, false, false, false, false, {}, {}, {} };
        g_moduleState = &st;

        JSRT_P3_CHECK_EC(JsSetPromiseContinuationCallback(TestModulePromiseContinuation, &st));
        JSRT_P3_CHECK_EC(JsSetModuleHostInfo(
            nullptr, JsModuleHostInfo_FetchImportedModuleFromScriptWithAttributesCallback,
            reinterpret_cast<void*>(&TestFetchFromScriptWithAttrs)));
        JSRT_P3_CHECK_EC(JsSetModuleHostInfo(
            nullptr, JsModuleHostInfo_NotifyModuleReadyCallback,
            reinterpret_cast<void*>(&TestNotifyModuleReady)));
        JSRT_P3_CHECK_EC(JsSetModuleHostInfo(
            nullptr, JsModuleHostInfo_ReportModuleCompletionCallback,
            reinterpret_cast<void*>(&TestReportModuleCompletion)));

        // Round-trip query check via JsGetModuleHostInfo
        JsModuleRecord probeRec = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsInitializeModuleRecord(nullptr, JS_INVALID_REFERENCE, &probeRec));
        void* roundTripCb = nullptr;
        JSRT_P3_CHECK_EC(JsGetModuleHostInfo(
            probeRec, JsModuleHostInfo_FetchImportedModuleFromScriptWithAttributesCallback, &roundTripCb));
        JSRT_P3_CHECK(roundTripCb == reinterpret_cast<void*>(&TestFetchFromScriptWithAttrs),
            "JsGetModuleHostInfo round-trip for FetchImportedModuleFromScriptWithAttributesCallback mismatch");

        if (RunScriptUtf8(
                "var __modJsonOk = 0;"
                "var __modPlainVal = 0;"
                "import('./virtual.json', { with: { type: 'json' } }).then(function(ns) { __modJsonOk = ns.default.ok; });"
                "import('./plain.js').then(function(ns) { __modPlainVal = ns.val; });",
                (JsSourceContext)7, nullptr) != 0)
        {
            g_moduleState = nullptr;
            return 1;
        }

        // Pump pending module parses -> NotifyModuleReady -> JsModuleEvaluation -> promise continuations
        JsValueRef global = JS_INVALID_REFERENCE;
        JSRT_P3_CHECK_EC(JsGetGlobalObject(&global));

        while (!st.pendingFetches.empty() || !st.readyModules.empty() || !st.microtasks.empty())
        {
            if (!st.pendingFetches.empty())
            {
                std::vector<PendingModuleFetch> fetches;
                fetches.swap(st.pendingFetches);
                for (size_t i = 0; i < fetches.size(); i++)
                {
                    JsValueRef errObj = JS_INVALID_REFERENCE;
                    JsErrorCode ec = JsParseModuleSource(
                        fetches[i].record,
                        (JsSourceContext)(100 + i),
                        reinterpret_cast<BYTE*>(const_cast<char*>(fetches[i].source.c_str())),
                        (unsigned int)fetches[i].source.size(),
                        JsParseModuleSourceFlags_DataIsUTF8,
                        &errObj);
                    JsRelease(fetches[i].record, nullptr);
                    JSRT_P3_CHECK(ec == JsNoError, "JsParseModuleSource failed in dynamic import round-trip");
                }
            }
            if (!st.readyModules.empty())
            {
                std::vector<JsModuleRecord> ready;
                ready.swap(st.readyModules);
                for (size_t i = 0; i < ready.size(); i++)
                {
                    JsValueRef evalRes = JS_INVALID_REFERENCE;
                    JsErrorCode ec = JsModuleEvaluation(ready[i], &evalRes);
                    JsRelease(ready[i], nullptr);
                    JSRT_P3_CHECK(ec == JsNoError, "JsModuleEvaluation failed in dynamic import round-trip");
                }
            }
            if (!st.microtasks.empty())
            {
                std::vector<JsValueRef> tasks;
                tasks.swap(st.microtasks);
                for (size_t i = 0; i < tasks.size(); i++)
                {
                    JsValueRef unused = JS_INVALID_REFERENCE;
                    JsErrorCode ec = JsCallFunction(tasks[i], &global, 1, &unused);
                    JsRelease(tasks[i], nullptr);
                    JSRT_P3_CHECK(ec == JsNoError, "module import promise continuation failed");
                }
            }
        }

        JSRT_P3_CHECK(st.fetchWithAttrsCalled && st.sawJsonAttribute && st.sawUndefinedAttrsForPlain,
            "FetchImportedModuleFromScriptWithAttributesCallBack round-trip checks did not all fire");
        JSRT_P3_CHECK(st.notifyReadyCalled && !st.completionErrored,
            "module readiness/completion check failed");

        JsPropertyIdRef jsonProp = JS_INVALID_REFERENCE;
        JsPropertyIdRef plainProp = JS_INVALID_REFERENCE;
        JsValueRef jsonVal = JS_INVALID_REFERENCE;
        JsValueRef plainVal = JS_INVALID_REFERENCE;
        int jsonNum = 0;
        int plainNum = 0;
        JSRT_P3_CHECK_EC(JsCreatePropertyId("__modJsonOk", 11, &jsonProp));
        JSRT_P3_CHECK_EC(JsCreatePropertyId("__modPlainVal", 13, &plainProp));
        JSRT_P3_CHECK_EC(JsGetProperty(global, jsonProp, &jsonVal));
        JSRT_P3_CHECK_EC(JsGetProperty(global, plainProp, &plainVal));
        JSRT_P3_CHECK_EC(JsNumberToInt(jsonVal, &jsonNum));
        JSRT_P3_CHECK_EC(JsNumberToInt(plainVal, &plainNum));
        JSRT_P3_CHECK(jsonNum == 42 && plainNum == 99,
            "dynamic import() with/without attributes resolved unexpected namespace values");

        g_moduleState = nullptr;
        JSRT_P3_CHECK_EC(JsSetPromiseContinuationCallback(nullptr, nullptr));
        JSRT_P3_CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
        JSRT_P3_CHECK_EC(JsDisposeRuntime(rt));
        return 0;
    }

    static int RunAll()
    {
        if (TestExternalArrayBufferLifetimes() != 0)
        {
            return 1;
        }
        if (TestTypedArrayAbi() != 0)
        {
            return 1;
        }
        if (TestTimerContinuationCallback() != 0)
        {
            return 1;
        }
        if (TestFetchImportedModuleFromScriptWithAttributes() != 0)
        {
            return 1;
        }
        return 0;
    }
}
