//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "ChakraCore.h"
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <cstring>

#define FAIL_CHECK(cmd)                     \
    do                                      \
    {                                       \
        JsErrorCode errCode = cmd;          \
        if (errCode != JsNoError)           \
        {                                   \
            printf("Error %d at '%s'\n",    \
                errCode, #cmd);             \
            return 1;                       \
        }                                   \
    } while(0)

using namespace std;

// S6d: JsSetTimerContinuationCallback install/uninstall round-trip. The
// callback is optional and additive; SharedArrayBuffer/Atomics are flag-gated
// off in this sample, so nothing schedules delayed tasks — the round-trip
// checks the per-context set/unset contract only.
static void TimerContinuationCallback(JsValueRef task, unsigned int delayMilliseconds, void *callbackState)
{
    (void)task;
    (void)delayMilliseconds;
    (void)callbackState;
}

int main()
{
    JsRuntimeHandle runtime;
    JsContextRef context;
    JsValueRef result;
    unsigned currentSourceContext = 0;

    const char* script = "(()=>{return \'SUCCESS\';})()";

    // Create a runtime.
    JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &runtime);

    // Create an execution context.
    JsCreateContext(runtime, &context);

    // Now set the current execution context.
    JsSetCurrentContext(context);

    // Optional timer continuation callback: set, run a script, unset.
    FAIL_CHECK(JsSetTimerContinuationCallback(TimerContinuationCallback, nullptr));
    FAIL_CHECK(JsSetTimerContinuationCallback(nullptr, nullptr));

    JsValueRef fname;
    FAIL_CHECK(JsCreateString("sample", strlen("sample"), &fname));

    JsValueRef scriptSource;
    FAIL_CHECK(JsCreateExternalArrayBuffer((void*)script, (unsigned int)strlen(script),
        nullptr, nullptr, &scriptSource));
    // Run the script.
    FAIL_CHECK(JsRun(scriptSource, currentSourceContext++, fname, JsParseScriptAttributeNone, &result));

    // Convert your script result to String in JavaScript; redundant if your script returns a String
    JsValueRef resultJSString;
    FAIL_CHECK(JsConvertValueToString(result, &resultJSString));

    // Project script result back to C++.
    char *resultSTR = nullptr;
    size_t stringLength;
    FAIL_CHECK(JsCopyString(resultJSString, nullptr, 0, &stringLength));
    resultSTR = (char*) malloc(stringLength + 1);
    FAIL_CHECK(JsCopyString(resultJSString, resultSTR, stringLength, nullptr));
    resultSTR[stringLength] = 0;

    printf("Result -> %s \n", resultSTR);
    free(resultSTR);

    // Dispose runtime
    JsSetCurrentContext(JS_INVALID_REFERENCE);
    JsDisposeRuntime(runtime);

    return 0;
}
