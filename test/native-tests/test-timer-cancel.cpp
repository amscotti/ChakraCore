//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused C++ JsRT regression test for JsSetTimerContinuationCancelCallback (R-4):
// 1. Additive install/unset round-trip (and context-less error handling).
// 2. Settled Atomics.waitAsync(1500ms) + Atomics.notify fires cancel callback with
//    the exact task handle, allowing immediate queue removal and JsRelease.
// 3. Backwards compatibility when only JsSetTimerContinuationCallback is set
//    (no cancel callback registered).

#include "ChakraCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>

#define CHECK_EC(expr) \
    do { \
        JsErrorCode ec__ = (expr); \
        if (ec__ != JsNoError) { \
            fprintf(stderr, "FAIL: %s returned error 0x%x at line %d\n", #expr, (unsigned)ec__, __LINE__); \
            return 1; \
        } \
    } while (0)

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s at line %d\n", (msg), __LINE__); \
            return 1; \
        } \
    } while (0)

struct HostState
{
    std::vector<JsValueRef> timers;
    std::vector<JsValueRef> microtasks;
    unsigned scheduleCount = 0;
    unsigned cancelCount = 0;
    unsigned canceledMatchCount = 0;
};

static void CHAKRA_CALLBACK OnTimerSchedule(JsValueRef task, unsigned int delayMs, void* state)
{
    (void)delayMs;
    HostState* st = static_cast<HostState*>(state);
    if (st == nullptr || task == JS_INVALID_REFERENCE) abort();
    st->scheduleCount++;
    if (JsAddRef(task, nullptr) != JsNoError) abort();
    st->timers.push_back(task);
}

static void CHAKRA_CALLBACK OnTimerCancel(JsValueRef task, void* state)
{
    HostState* st = static_cast<HostState*>(state);
    if (st == nullptr || task == JS_INVALID_REFERENCE) abort();
    st->cancelCount++;
    auto it = std::find(st->timers.begin(), st->timers.end(), task);
    if (it != st->timers.end())
    {
        st->canceledMatchCount++;
        JsRelease(*it, nullptr);
        st->timers.erase(it);
    }
}

static void CHAKRA_CALLBACK OnPromise(JsValueRef task, void* state)
{
    HostState* st = static_cast<HostState*>(state);
    if (st == nullptr || task == JS_INVALID_REFERENCE) abort();
    if (JsAddRef(task, nullptr) != JsNoError) abort();
    st->microtasks.push_back(task);
}

static int RunUtf8(const char* code)
{
    JsValueRef fname = JS_INVALID_REFERENCE;
    JsValueRef src = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateString("test_timer_cancel.js", 20, &fname));
    CHECK_EC(JsCreateString(code, strlen(code), &src));
    CHECK_EC(JsRun(src, JS_SOURCE_CONTEXT_NONE, fname, JsParseScriptAttributeNone, nullptr));
    return 0;
}

static int DrainMicrotasks(HostState* st)
{
    JsValueRef global = JS_INVALID_REFERENCE;
    CHECK_EC(JsGetGlobalObject(&global));
    while (!st->microtasks.empty())
    {
        std::vector<JsValueRef> batch;
        batch.swap(st->microtasks);
        for (size_t i = 0; i < batch.size(); i++)
        {
            JsValueRef unused = JS_INVALID_REFERENCE;
            JsErrorCode ec = JsCallFunction(batch[i], &global, 1, &unused);
            JsRelease(batch[i], nullptr);
            if (ec != JsNoError) return 1;
        }
    }
    return 0;
}

int main()
{
    // Verify no-current-context returns JsErrorNoCurrentContext cleanly.
    CHECK(JsSetTimerContinuationCancelCallback(OnTimerCancel, nullptr) == JsErrorNoCurrentContext,
        "expected JsErrorNoCurrentContext when no current context");

    JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef ctx = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
    CHECK_EC(JsCreateContext(rt, &ctx));
    CHECK_EC(JsSetCurrentContext(ctx));

    HostState st;
    CHECK_EC(JsSetPromiseContinuationCallback(OnPromise, &st));
    CHECK_EC(JsSetTimerContinuationCallback(OnTimerSchedule, &st));
    CHECK_EC(JsSetTimerContinuationCancelCallback(OnTimerCancel, &st));

    // Enable SAB waitAsync tests only if SharedArrayBuffer global is available in this runtime.
    CHECK(RunUtf8(
        "var __status = 'init';"
        "if (typeof SharedArrayBuffer === 'function' && Atomics.waitAsync) {"
        "  var ia = new Int32Array(new SharedArrayBuffer(4));"
        "  var r = Atomics.waitAsync(ia, 0, 0, 1500);"
        "  r.value.then(function(v) { __status = v; });"
        "  Atomics.notify(ia, 0, 1);"
        "} else { __status = 'no-sab'; }") == 0, "script run failed");

    CHECK(DrainMicrotasks(&st) == 0, "drain microtasks failed");

    JsValueRef global = JS_INVALID_REFERENCE;
    JsPropertyIdRef prop = JS_INVALID_REFERENCE;
    JsValueRef val = JS_INVALID_REFERENCE;
    CHECK_EC(JsGetGlobalObject(&global));
    CHECK_EC(JsCreatePropertyId("__status", 8, &prop));
    CHECK_EC(JsGetProperty(global, prop, &val));
    char buf[64] = {};
    size_t len = 0;
    CHECK_EC(JsCopyString(val, buf, sizeof(buf) - 1, &len));
    std::string status(buf, len);

    if (status != "no-sab")
    {
        CHECK(status == "ok", "expected ok resolution");
        CHECK(st.scheduleCount == 1, "expected 1 scheduled timer");
        CHECK(st.cancelCount == 1, "expected 1 cancel notification");
        CHECK(st.canceledMatchCount == 1, "expected 1 matching task canceled");
        CHECK(st.timers.empty(), "expected timer queue to be completely empty after notify");
    }

    // Backwards compatibility: unset cancel callback and settle again -> no crash, task stays in queue.
    CHECK_EC(JsSetTimerContinuationCancelCallback(nullptr, nullptr));
    HostState stCompat;
    CHECK_EC(JsSetPromiseContinuationCallback(OnPromise, &stCompat));
    CHECK_EC(JsSetTimerContinuationCallback(OnTimerSchedule, &stCompat));
    if (status != "no-sab")
    {
        CHECK(RunUtf8(
            "var ia2 = new Int32Array(new SharedArrayBuffer(4));"
            "Atomics.waitAsync(ia2, 0, 0, 1500);"
            "Atomics.notify(ia2, 0, 1);") == 0, "compat waitAsync failed");
        CHECK(DrainMicrotasks(&stCompat) == 0, "compat microtask drain failed");
        CHECK(stCompat.scheduleCount == 1, "compat scheduled 1");
        CHECK(stCompat.cancelCount == 0, "compat cancelCount must be 0 when unset");
        CHECK(stCompat.timers.size() == 1, "compat leaves task queued for host");
        for (JsValueRef t : stCompat.timers) JsRelease(t, nullptr);
        stCompat.timers.clear();
    }

    CHECK_EC(JsSetTimerContinuationCallback(nullptr, nullptr));
    CHECK_EC(JsSetPromiseContinuationCallback(nullptr, nullptr));
    CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
    CHECK_EC(JsDisposeRuntime(rt));
    printf("PASS\n");
    return 0;
}
