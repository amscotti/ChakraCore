//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "PlatformAgnostic/ChakraICU.h"
#ifdef __valid
#undef __valid
#endif
#if defined(__APPLE__)
#ifdef ctime
#undef ctime
#define CTIME_UNDEFED
#endif
#endif
#include <atomic>
#include <new>
#include <vector>
#include <ctime>
#include <ratio>
#include <chrono>
#ifdef CTIME_UNDEFED
#define ctime PAL_ctime
#undef CTIME_UNDEFED
#endif
#if !defined(_WIN32)
#include <unistd.h> // ::_exit (raw, non-PAL process termination for WScript.Quit)
#endif

#if defined(_X86_) || defined(_M_IX86)
#define CPU_ARCH_TEXT "x86"
#elif defined(_AMD64_) || defined(_IA64_) || defined(_M_AMD64) || defined(_M_IA64)
#define CPU_ARCH_TEXT "x86_64"
#elif defined(_ARM_) || defined(_M_ARM)
#define CPU_ARCH_TEXT "ARM"
#elif defined(_ARM64_) || defined(_M_ARM64)
#define CPU_ARCH_TEXT "ARM64"
#endif

// do not change the order below
// otherwise, i.e. android system can be marked as posix? etc..
#ifdef _WIN32
#define DEST_PLATFORM_TEXT "win32"
#else // ! _WIN32
#if defined(__APPLE__)
#ifdef __IOS__
#define DEST_PLATFORM_TEXT "ios"
#else // ! iOS
#define DEST_PLATFORM_TEXT "darwin"
#endif // iOS ?
#elif defined(__ANDROID__)
#define DEST_PLATFORM_TEXT "android"
#elif defined(__linux__)
#define DEST_PLATFORM_TEXT "posix"
#elif defined(__FreeBSD__) || defined(__unix__)
#define DEST_PLATFORM_TEXT "bsd"
#endif // FreeBSD or unix ?
#endif // _WIN32 ?

#pragma prefast(disable:26444, "This warning unfortunately raises false positives when auto is used for declaring the type of an iterator in a loop.")
#ifdef HAS_ICU
#define INTL_LIBRARY_TEXT "icu"
#elif defined(_WIN32)
#define INTL_LIBRARY_TEXT "winglob"
#else
#define INTL_LIBRARY_TEXT ""
#endif

MessageQueue* WScriptJsrt::messageQueue = nullptr;
std::map<std::string, JsModuleRecord>  WScriptJsrt::moduleRecordMap;
std::map<JsModuleRecord, std::string>  WScriptJsrt::moduleDirMap;
std::map<JsModuleRecord, ModuleState>  WScriptJsrt::moduleErrMap;
std::map<DWORD_PTR, std::string> WScriptJsrt::scriptDirMap;
std::atomic<DWORD_PTR> WScriptJsrt::sourceContext{ 0 };
FileNode * SourceMap::root = nullptr;

#define ERROR_MESSAGE_TO_STRING(errorCode, errorMessage, errorMessageString)        \
    JsErrorCode errorCode = JsNoError;                                              \
    do                                                                              \
    {                                                                               \
        const char *outOfMemoryString =                                             \
                                    "Failed to convert wide string. Out of memory?";\
                                                                                    \
        char *errorMessageNarrow;                                                   \
        if (FAILED(WideStringToNarrowDynamic(errorMessage, &errorMessageNarrow)))   \
        {                                                                           \
            errorCode = ChakraRTInterface::JsCreateString(outOfMemoryString,        \
                strlen(outOfMemoryString), &errorMessageString);                    \
        }                                                                           \
        else                                                                        \
        {                                                                           \
            errorCode = ChakraRTInterface::JsCreateString(errorMessageNarrow,       \
                strlen(errorMessageNarrow), &errorMessageString);                   \
            free(errorMessageNarrow);                                               \
        }                                                                           \
    }                                                                               \
    while(0)

#include "WScriptJsrtState.h"
#include "WScriptSerializer.h"
#include "WScriptTest262Agents.h"
#include "WScriptModuleHost.h"

bool WScriptJsrt::CreateArgumentsObject(JsValueRef *argsObject)
{
    LPWSTR *argv = HostConfigFlags::argsVal;
    JsValueRef retArr;

    Assert(argsObject);
    *argsObject = nullptr;

    IfJsrtErrorFail(ChakraRTInterface::JsCreateArray(HostConfigFlags::argsCount, &retArr), false);

    for (int i = 0; i < HostConfigFlags::argsCount; i++)
    {
        JsValueRef value;
        JsValueRef index;

        char *argNarrow;
        if (FAILED(WideStringToNarrowDynamic(argv[i], &argNarrow)))
        {
            return false;
        }
        JsErrorCode errCode  = ChakraRTInterface::JsCreateString(
            argNarrow,
            strlen(argNarrow), &value);
        free(argNarrow);
        IfJsrtErrorFail(errCode, false);

        IfJsrtErrorFail(ChakraRTInterface::JsDoubleToNumber(i, &index), false);
        IfJsrtErrorFail(ChakraRTInterface::JsSetIndexedProperty(retArr, index, value), false);
    }

    *argsObject = retArr;

    return true;
}

JsValueRef __stdcall WScriptJsrt::EchoCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    for (unsigned int i = 1; i < argumentCount; i++)
    {
        JsValueRef strValue;
        JsErrorCode error = ChakraRTInterface::JsConvertValueToString(arguments[i], &strValue);
        if (error == JsNoError)
        {
            AutoString str(strValue);
            if (str.GetError() == JsNoError)
            {
                if (i > 1)
                {
                    wprintf(_u(" "));
                }
                charcount_t len;
                LPWSTR ws = str.GetWideString(&len);
                LPWSTR wsNoNull = new WCHAR[((size_t)len) + 1];
                charcount_t newIndex = 0;
                for (charcount_t j = 0; j < len; j++)
                {
                    if (ws[j] != _u('\0'))
                    {
                        wsNoNull[newIndex++] = ws[j];
                    }
                }
                wsNoNull[newIndex] = _u('\0');
                wprintf(_u("%s"), wsNoNull);
                delete[] wsNoNull;
            }
        }

        if (error == JsErrorScriptException)
        {
            return nullptr;
        }
    }

    wprintf(_u("\n"));
    fflush(stdout);

    JsValueRef undefinedValue;
    if (ChakraRTInterface::JsGetUndefinedValue(&undefinedValue) == JsNoError)
    {
        return undefinedValue;
    }
    else
    {
        return nullptr;
    }
}

// Raw process termination for WScript.Quit so wedged/sibling agent threads
// cannot observe a half-torn-down PAL/runtime.
static DECLSPEC_NORETURN void RawExitProcess(int exitCode)
{
    fflush(stdout);
    fflush(stderr);

#if defined(_WIN32)
    ExitProcess(exitCode);
#else
    ::_exit(exitCode & 0xff);
#endif
}

JsValueRef __stdcall WScriptJsrt::QuitCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    int exitCode = 0;

    if (argumentCount > 1)
    {
        double exitCodeDouble;
        IfJsrtErrorFail(ChakraRTInterface::JsNumberToDouble(arguments[1], &exitCodeDouble), JS_INVALID_REFERENCE);
        exitCode = (int)exitCodeDouble;
    }

    RawExitProcess(exitCode);
}

JsValueRef __stdcall WScriptJsrt::LoadScriptFileCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return LoadScriptFileHelper(callee, arguments, argumentCount, false);
}

// needed because of calling convention differences between _stdcall and _cdecl
void CHAKRA_CALLBACK WScriptJsrt::FinalizeFree(void* addr)
{
    free(addr);
}

JsValueRef WScriptJsrt::LoadScriptFileHelper(JsValueRef callee, JsValueRef *arguments, unsigned short argumentCount, bool isSourceModule)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("");

    if (argumentCount < 2 || argumentCount > 4)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Need more or fewer arguments for WScript.LoadScript");
    }
    else
    {
        LPCSTR fileContent;
        AutoString fileName(arguments[1]);
        IfJsrtErrorSetGo(fileName.GetError());

        AutoString scriptInjectType;
        if (argumentCount > 2)
        {
            IfJsrtErrorSetGo(scriptInjectType.Initialize(arguments[2]));
        }

        if (errorCode == JsNoError)
        {
            hr = Helpers::LoadScriptFromFile(*fileName, fileContent);
            if (FAILED(hr))
            {
                {
                    StderrLock lock;
                    fprintf(stderr, "Couldn't load file '%s'\n", fileName.GetString());
                    fflush(stderr);
                }
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));
                return returnValue;
            }

            returnValue = LoadScript(callee, *fileName, fileContent, *scriptInjectType ? *scriptInjectType : "self", isSourceModule, WScriptJsrt::FinalizeFree, true);
        }
    }

Error:

    SetExceptionIf(errorCode, errorMessage);
    return returnValue;
}

void WScriptJsrt::SetExceptionIf(JsErrorCode errorCode, LPCWSTR errorMessage)
{
    if (errorCode != JsNoError)
    {
        // If the exception is already set - no need to create a new exception.
        bool hasException = false;
        if (!(ChakraRTInterface::JsHasException(&hasException) == JsNoError && hasException))
        {
            JsValueRef errorObject;
            JsValueRef errorMessageString;

            if (wcscmp(errorMessage, _u("")) == 0)
            {
                errorMessage = ConvertErrorCodeToMessage(errorCode);
            }

            ERROR_MESSAGE_TO_STRING(errCode, errorMessage, errorMessageString);

            ChakraRTInterface::JsCreateError(errorMessageString, &errorObject);
            ChakraRTInterface::JsSetException(errorObject);
        }
    }
}

JsValueRef __stdcall WScriptJsrt::LoadScriptCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return LoadScriptHelper(callee, isConstructCall, arguments, argumentCount, callbackState, false);
}

JsValueRef __stdcall WScriptJsrt::LoadModuleCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    return LoadScriptHelper(callee, isConstructCall, arguments, argumentCount, callbackState, true);
}

JsValueRef WScriptJsrt::LoadScriptHelper(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState, bool isSourceModule)
{
    HRESULT hr = E_FAIL;
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("");
    JsValueRef returnValue = JS_INVALID_REFERENCE;

    if (argumentCount < 2 || argumentCount > 4)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Need more or fewer arguments for WScript.LoadScript");
    }
    else
    {
        AutoString fileContent;
        char *fileNameNarrow = nullptr;
        AutoString fileName;
        AutoString scriptInjectType;
        char fileNameBuffer[MAX_PATH];
        bool isFile = true;

        IfJsrtErrorSetGo(fileContent.Initialize(arguments[1]));

        if (argumentCount > 2)
        {
            IfJsrtErrorSetGo(scriptInjectType.Initialize(arguments[2]));

            if (argumentCount > 3)
            {
                IfJsrtErrorSetGo(fileName.Initialize(arguments[3]));
                fileNameNarrow = *fileName;
            }
        }

        if (!fileNameNarrow)
        {
            isFile = false;
            if (isSourceModule)
            {
                const int nextId = (int)sourceContext.load();
                sprintf_s(fileNameBuffer, MAX_PATH, "moduleScript%i.js", nextId);
                fileNameNarrow = fileNameBuffer;
            }
        }

        if (*fileContent)
        {
            fileContent.MakePersistent();
            returnValue = LoadScript(callee, fileNameNarrow, *fileContent, *scriptInjectType ? *scriptInjectType : "self", isSourceModule, WScriptJsrt::FinalizeFree, isFile);
        }
    }

Error:
    SetExceptionIf(errorCode, errorMessage);
    return returnValue;
}

JsValueRef WScriptJsrt::LoadScript(JsValueRef callee, LPCSTR fileName,
    LPCSTR fileContent, LPCSTR scriptInjectType, bool isSourceModule, JsFinalizeCallback finalizeCallback, bool isFile)
{
    HRESULT hr = E_FAIL;
    JsErrorCode errorCode = JsNoError;
    LPCWSTR errorMessage = _u("Internal error.");
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsContextRef currentContext = JS_INVALID_REFERENCE;
    JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
    void *callbackArg = (finalizeCallback != nullptr ? (void*)fileContent : nullptr);
    bool fileContentOwnedByExternalBuffer = false;
    bool restoreContext = false;

    char fullPath[_MAX_PATH];

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetCurrentContext(&currentContext));
    IfJsrtErrorSetGo(ChakraRTInterface::JsGetRuntime(currentContext, &runtime));

    if (fileName == nullptr)
    {
        fileName = "script.js";
    }

    if (_fullpath(fullPath, fileName, _MAX_PATH) == nullptr)
    {
        goto Error;
    }

    if (isSourceModule || (strcmp(scriptInjectType, "module") == 0))
    {
        errorCode = LoadModuleFromString(fileName, fileContent, fullPath, isFile);
    }
    else if (strcmp(scriptInjectType, "self") == 0)
    {
        JsContextRef calleeContext;
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetContextOfObject(callee, &calleeContext));

        IfJsrtErrorSetGo(ChakraRTInterface::JsSetCurrentContext(calleeContext));
        restoreContext = true;

        JsValueRef scriptSource;
        IfJsrtErrorSetGo(ChakraRTInterface::JsCreateExternalArrayBuffer((void*)fileContent,
            (unsigned int)strlen(fileContent), finalizeCallback, callbackArg, &scriptSource));
        fileContentOwnedByExternalBuffer = true;
        JsValueRef fname;
        IfJsrtErrorSetGo(ChakraRTInterface::JsCreateString(fullPath,
            strlen(fullPath), &fname));
        JsSourceContext nextSourceContext = GetNextSourceContext();
        RegisterScriptDir(nextSourceContext, fullPath);

        if (HostConfigFlags::flags.UseParserStateCacheIsEnabled)
        {
            JsValueRef parserState;
            IfJsrtErrorSetGo(ChakraRTInterface::JsSerializeParserState(scriptSource, &parserState, JsParseScriptAttributeNone));
            errorCode = ChakraRTInterface::JsRunScriptWithParserState(scriptSource, nextSourceContext, fname, JsParseScriptAttributeNone, parserState, &returnValue);
        }
        else
        {
            errorCode = ChakraRTInterface::JsRun(scriptSource, nextSourceContext, fname, JsParseScriptAttributeNone, &returnValue);
        }

        if (errorCode == JsNoError)
        {
            errorCode = ChakraRTInterface::JsGetGlobalObject(&returnValue);
        }

        restoreContext = false;
        IfJsrtErrorSetGo(ChakraRTInterface::JsSetCurrentContext(currentContext));
    }
    else if (strcmp(scriptInjectType, "samethread") == 0)
    {
        JsValueRef newContext = JS_INVALID_REFERENCE;

        IfJsrtErrorSetGo(ChakraRTInterface::JsCreateContext(runtime, &newContext));

#if ENABLE_TTD
        IfJsrtErrorSetGo(ChakraRTInterface::JsSetObjectBeforeCollectCallback(newContext, nullptr, WScriptJsrt::JsContextBeforeCollectCallback));
#endif

        IfJsrtErrorSetGo(ChakraRTInterface::JsSetCurrentContext(newContext));
        restoreContext = true;

        IfJsErrorFailLog(ChakraRTInterface::JsSetPromiseContinuationCallback(PromiseContinuationCallback, (void*)GetThreadMessageQueue()));
        IfJsErrorFailLog(ChakraRTInterface::JsSetTimerContinuationCallback(TimerContinuationCallback, (void*)GetThreadMessageQueue()));
        IfJsErrorFailLog(ChakraRTInterface::JsSetTimerContinuationCancelCallback(TimerContinuationCancelCallback, (void*)GetThreadMessageQueue()));

        Initialize();

        JsValueRef scriptSource;
        IfJsrtErrorSetGo(ChakraRTInterface::JsCreateExternalArrayBuffer((void*)fileContent,
            (unsigned int)strlen(fileContent), finalizeCallback, callbackArg, &scriptSource));
        fileContentOwnedByExternalBuffer = true;
        JsValueRef fname;
        IfJsrtErrorSetGo(ChakraRTInterface::JsCreateString(fullPath,
            strlen(fullPath), &fname));
        JsSourceContext nextSourceContext = GetNextSourceContext();
        RegisterScriptDir(nextSourceContext, fullPath);

        if (HostConfigFlags::flags.UseParserStateCacheIsEnabled)
        {
            JsValueRef parserState;
            IfJsrtErrorSetGo(ChakraRTInterface::JsSerializeParserState(scriptSource, &parserState, JsParseScriptAttributeNone));
            errorCode = ChakraRTInterface::JsRunScriptWithParserState(scriptSource, nextSourceContext, fname, JsParseScriptAttributeNone, parserState, &returnValue);
        }
        else
        {
            errorCode = ChakraRTInterface::JsRun(scriptSource, nextSourceContext, fname, JsParseScriptAttributeNone, &returnValue);
        }

        if (errorCode == JsNoError)
        {
            errorCode = ChakraRTInterface::JsGetGlobalObject(&returnValue);
        }

        ChakraRTInterface::JsSetCurrentContext(currentContext);
        restoreContext = false;
    }
    else if (strcmp(scriptInjectType, "crossthread") == 0)
    {
        auto& threadData = GetRuntimeThreadLocalData().threadData;
        if (threadData == nullptr)
        {
            threadData = new RuntimeThreadData();
        }

        RuntimeThreadData* child = new RuntimeThreadData();
        child->initialSource = fileContent;
        threadData->children.push_back(child);
        child->parent = threadData;

        ResetEvent(threadData->hevntInitialScriptCompleted);

        child->hThread = ::CreateThread(NULL, NULL, [](void* param) -> DWORD
        {
            return ((RuntimeThreadData*)param)->ThreadProc();
        }, (void*)child, NULL, NULL);

        if (child->hThread == nullptr)
        {
            threadData->children.pop_back();
            delete child;

            errorCode = JsErrorOutOfMemory;
            errorMessage = _u("Failed to create the agent thread.");
            goto Error;
        }

        {
            HANDLE spawnWaitHandles[] = { threadData->hevntInitialScriptCompleted, child->hThread };
            WaitForMultipleObjects(_countof(spawnWaitHandles), spawnWaitHandles, FALSE, INFINITE);
        }
    }
    else
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Unsupported argument type inject type.");
    }

Error:
    if (restoreContext && currentContext != JS_INVALID_REFERENCE)
    {
        ChakraRTInterface::JsSetCurrentContext(currentContext);
        restoreContext = false;
    }

    if (!fileContentOwnedByExternalBuffer && finalizeCallback != nullptr && fileContent != nullptr)
    {
        finalizeCallback((void*)fileContent);
    }

    JsValueRef value = returnValue;
    if (errorCode != JsNoError)
    {
        SetExceptionIf(errorCode, errorMessage);
        ChakraRTInterface::JsDoubleToNumber(errorCode, &value);
    }

    _flushall();

    return value;
}

JsValueRef WScriptJsrt::SetTimeoutCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    LPCWSTR errorMessage = _u("invalid call to WScript.SetTimeout");
    JsErrorCode errorCode = JsNoError;
    HRESULT hr = S_OK;

    JsValueRef function;
    JsValueRef timerId;
    unsigned int time;
    double tmp;
    CallbackMessage *msg = nullptr;

    if (argumentCount != 3)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }

    function = arguments[1];

    IfJsrtErrorSetGo(ChakraRTInterface::JsNumberToDouble(arguments[2], &tmp));

    time = static_cast<int>(tmp);
    msg = new (std::nothrow) CallbackMessage(time, function);
    if (msg == nullptr)
    {
        errorCode = JsErrorOutOfMemory;
        goto Error;
    }
    if (!msg->HasValidRef())
    {
        errorCode = msg->GetAddRefError();
        delete msg;
        msg = nullptr;
        goto Error;
    }
    GetThreadMessageQueue()->InsertSorted(msg);

    IfJsrtErrorSetGo(ChakraRTInterface::JsDoubleToNumber(static_cast<double>(msg->GetId()), &timerId));
    return timerId;

Error:
    SetExceptionIf(errorCode, errorMessage);
    return JS_INVALID_REFERENCE;
}

JsValueRef WScriptJsrt::ClearTimeoutCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    LPCWSTR errorMessage = _u("invalid call to WScript.ClearTimeout");
    JsErrorCode errorCode = JsNoError;
    HRESULT hr = S_OK;

    if (argumentCount != 2)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }

    unsigned int timerId;
    double tmp;
    JsValueRef undef;

    if (ChakraRTInterface::JsNumberToDouble(arguments[1], &tmp) == JsNoError)
    {
        timerId = static_cast<int>(tmp);
        GetThreadMessageQueue()->RemoveById(timerId);
    }

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&undef));
    return undef;

Error:
    SetExceptionIf(errorCode, errorMessage);
    return JS_INVALID_REFERENCE;
}

template <class DebugOperationFunc>
static JsErrorCode QueueDebugOperation(JsValueRef function, const DebugOperationFunc& operation)
{
    WScriptJsrt::CallbackMessage* msg = WScriptJsrt::CallbackMessage::Create(function, operation);
    if (msg == nullptr)
    {
        return JsErrorOutOfMemory;
    }
    if (!msg->HasValidRef())
    {
        JsErrorCode err = msg->GetAddRefError();
        delete msg;
        return err;
    }
    WScriptJsrt::PushMessage(msg);
    return JsNoError;
}

JsValueRef WScriptJsrt::AttachCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    LPCWSTR errorMessage = _u("WScript.Attach requires a function, like WScript.Attach(foo);");
    JsErrorCode errorCode = JsNoError;
    HRESULT hr = S_OK;
    JsValueType argumentType = JsUndefined;
    if (argumentCount != 2)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }
    IfJsrtErrorSetGo(ChakraRTInterface::JsGetValueType(arguments[1], &argumentType));
    if (argumentType != JsFunction)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }
    IfJsrtErrorSetGo(QueueDebugOperation(arguments[1], [](WScriptJsrt::CallbackMessage& msg)
    {
        JsContextRef currentContext = JS_INVALID_REFERENCE;
        ChakraRTInterface::JsGetCurrentContext(&currentContext);
        JsRuntimeHandle currentRuntime = JS_INVALID_RUNTIME_HANDLE;
        ChakraRTInterface::JsGetRuntime(currentContext, &currentRuntime);

        Debugger* debugger = Debugger::GetDebugger(currentRuntime);
        debugger->StartDebugging(currentRuntime);
        debugger->SourceRunDown();

        return msg.CallFunction("");
    }));
Error:
    SetExceptionIf(errorCode, errorMessage);
    return JS_INVALID_REFERENCE;
}

JsValueRef WScriptJsrt::DetachCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    LPCWSTR errorMessage = _u("WScript.Detach requires a function, like WScript.Detach(foo);");
    JsErrorCode errorCode = JsNoError;
    HRESULT hr = S_OK;
    JsValueType argumentType = JsUndefined;
    if (argumentCount != 2)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }
    IfJsrtErrorSetGo(ChakraRTInterface::JsGetValueType(arguments[1], &argumentType));
    if (argumentType != JsFunction)
    {
        errorCode = JsErrorInvalidArgument;
        goto Error;
    }
    IfJsrtErrorSetGo(QueueDebugOperation(arguments[1], [](WScriptJsrt::CallbackMessage& msg)
    {
        JsContextRef currentContext = JS_INVALID_REFERENCE;
        ChakraRTInterface::JsGetCurrentContext(&currentContext);
        JsRuntimeHandle currentRuntime = JS_INVALID_RUNTIME_HANDLE;
        ChakraRTInterface::JsGetRuntime(currentContext, &currentRuntime);
        if (Debugger::debugger != nullptr)
        {
            Debugger* debugger = Debugger::GetDebugger(currentRuntime);
            debugger->StopDebugging(currentRuntime);
        }
        return msg.CallFunction("");
    }));
Error:
    SetExceptionIf(errorCode, errorMessage);
    return JS_INVALID_REFERENCE;
}

JsValueRef WScriptJsrt::DumpFunctionPositionCallback(JsValueRef callee, bool isConstructCall, JsValueRef * arguments, unsigned short argumentCount, void * callbackState)
{
    JsValueRef functionPosition = JS_INVALID_REFERENCE;

    if (argumentCount > 1)
    {
        if (ChakraRTInterface::JsDiagGetFunctionPosition(arguments[1], &functionPosition) != JsNoError)
        {
            IfJsErrorFailLogAndRet(ChakraRTInterface::JsGetUndefinedValue(&functionPosition));
        }

        if (Debugger::debugger != nullptr)
        {
            Debugger::debugger->DumpFunctionPosition(functionPosition);
        }
    }

    return JS_INVALID_REFERENCE;
}

JsValueRef WScriptJsrt::RequestAsyncBreakCallback(JsValueRef callee, bool isConstructCall,
    JsValueRef * arguments, unsigned short argumentCount, void * callbackState)
{
    if (Debugger::debugger != nullptr && !Debugger::debugger->IsDetached())
    {
        IfJsErrorFailLogAndRet(ChakraRTInterface::JsDiagRequestAsyncBreak(Debugger::GetRuntime()));
    }
    else
    {
        Helpers::LogError(_u("RequestAsyncBreak can only be called when debugger is attached"));
    }

    return JS_INVALID_REFERENCE;
}

bool WScriptJsrt::CreateNamedFunction(const char* nameString, JsNativeFunction callback,
    JsValueRef* functionVar)
{
    JsValueRef nameVar;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        nameString, strlen(nameString), &nameVar), false);
    IfJsrtErrorFail(ChakraRTInterface::JsCreateNamedFunction(nameVar, callback,
        nullptr, functionVar), false);
    return true;
}

bool WScriptJsrt::InstallObjectsOnObject(JsValueRef object, const char* name,
    JsNativeFunction nativeFunction)
{
    JsValueRef propertyValueRef;
    JsPropertyIdRef propertyId;
    IfJsrtErrorFail(CreatePropertyIdFromString(name, &propertyId), false);
    if (!CreateNamedFunction(name, nativeFunction, &propertyValueRef))
    {
        return false;
    }
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(object, propertyId,
        propertyValueRef, true), false);
    return true;
}

bool WScriptJsrt::Initialize()
{
    HRESULT hr = S_OK;
    char CH_BINARY_LOCATION[2048];
#ifdef CHAKRA_STATIC_LIBRARY
    const char* LINK_TYPE = "static";
#else
    const char* LINK_TYPE = "shared";
#endif
#ifdef HAS_ICU
    int icuVersion = PlatformAgnostic::ICUHelpers::GetICUMajorVersion();
#else
    int icuVersion = -1;
#endif

    JsValueRef wscript;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateObject(&wscript), false);

    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "monotonicNow", MonotonicNowCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Echo", EchoCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Quit", QuitCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "LoadScriptFile", LoadScriptFileCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "LoadScript", LoadScriptCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "LoadModule", LoadModuleCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "SetTimeout", SetTimeoutCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "ClearTimeout", ClearTimeoutCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Attach", AttachCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Detach", DetachCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "DumpFunctionPosition", DumpFunctionPositionCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "RequestAsyncBreak", RequestAsyncBreakCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "LoadBinaryFile", LoadBinaryFileCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "LoadTextFile", LoadTextFileCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Flag", FlagCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "RegisterModuleSource", RegisterModuleSourceCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "GetModuleNamespace", GetModuleNamespace));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "GetProxyProperties", GetProxyPropertiesCallback));

    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "SerializeObject", SerializeObject));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Deserialize", Deserialize));

    // Platform
    JsValueRef platformObject;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateObject(&platformObject), false);
    JsPropertyIdRef platformProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("Platform", &platformProperty), false);

    // Set CPU arch
    JsPropertyIdRef archProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("ARCH", &archProperty), false);
    JsValueRef archValue;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        CPU_ARCH_TEXT, strlen(CPU_ARCH_TEXT), &archValue), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, archProperty,
        archValue, true), false);

    // Set Build Type
    JsPropertyIdRef buildProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("BUILD_TYPE", &buildProperty), false);
    JsValueRef buildValue;
#ifdef _DEBUG
#define BUILD_TYPE_STRING_CH "Debug" // (O0)
#elif defined(ENABLE_DEBUG_CONFIG_OPTIONS)
#define BUILD_TYPE_STRING_CH "Test" // (O3 with debug config options)
#else
#define BUILD_TYPE_STRING_CH "Release" // (O3)
#endif
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        BUILD_TYPE_STRING_CH, strlen(BUILD_TYPE_STRING_CH), &buildValue), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, buildProperty,
        buildValue, true), false);
#undef BUILD_TYPE_STRING_CH

    // Set Link Type [static / shared]
    JsPropertyIdRef linkProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("LINK_TYPE", &linkProperty), false);
    JsValueRef linkValue;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        LINK_TYPE, strlen(LINK_TYPE), &linkValue), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, linkProperty,
      linkValue, true), false);

    // Set Binary Location
    JsValueRef binaryPathValue;
    PlatformAgnostic::SystemInfo::GetBinaryLocation(CH_BINARY_LOCATION, sizeof(CH_BINARY_LOCATION));

    JsPropertyIdRef binaryPathProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("BINARY_PATH", &binaryPathProperty), false);

    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        CH_BINARY_LOCATION,
        strlen(CH_BINARY_LOCATION), &binaryPathValue), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(
        platformObject, binaryPathProperty, binaryPathValue, true), false);

    // Set destination OS
    JsPropertyIdRef osProperty;
    IfJsrtErrorFail(CreatePropertyIdFromString("OS", &osProperty), false);
    JsValueRef osValue;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(
        DEST_PLATFORM_TEXT, strlen(DEST_PLATFORM_TEXT), &osValue), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, osProperty,
        osValue, true), false);

    // set Internationalization library
    JsPropertyIdRef intlLibraryProp;
    IfJsrtErrorFail(CreatePropertyIdFromString("INTL_LIBRARY", &intlLibraryProp), false);
    JsValueRef intlLibraryStr;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateString(INTL_LIBRARY_TEXT, strlen(INTL_LIBRARY_TEXT), &intlLibraryStr), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, intlLibraryProp, intlLibraryStr, true), false);
    JsPropertyIdRef icuVersionProp;
    IfJsrtErrorFail(CreatePropertyIdFromString("ICU_VERSION", &icuVersionProp), false);
    JsValueRef icuVersionNum;
    IfJsrtErrorFail(ChakraRTInterface::JsIntToNumber(icuVersion, &icuVersionNum), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(platformObject, icuVersionProp, icuVersionNum, true), false);

    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(wscript, platformProperty,
        platformObject, true), false);

    JsValueRef argsObject;

    if (!CreateArgumentsObject(&argsObject))
    {
        return false;
    }

    JsPropertyIdRef argsName;
    IfJsrtErrorFail(CreatePropertyIdFromString("Arguments", &argsName), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(wscript, argsName, argsObject, true), false);

    JsPropertyIdRef wscriptName;
    IfJsrtErrorFail(CreatePropertyIdFromString("WScript", &wscriptName), false);

    JsValueRef global;
    IfJsrtErrorFail(ChakraRTInterface::JsGetGlobalObject(&global), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(global, wscriptName, wscript, true), false);

    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(global, "print", EchoCallback));

    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(global, "read", LoadTextFileCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(global, "readbuffer", LoadBinaryFileCallback));
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(global, "readline", ReadLineStdinCallback));

    JsValueRef console;
    IfJsrtErrorFail(ChakraRTInterface::JsCreateObject(&console), false);
    IfFalseGo(WScriptJsrt::InstallObjectsOnObject(console, "log", EchoCallback));

    JsPropertyIdRef consoleName;
    IfJsrtErrorFail(CreatePropertyIdFromString("console", &consoleName), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetProperty(global, consoleName, console, true), false);

    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_FetchImportedModuleCallback, (void*)WScriptJsrt::FetchImportedModule), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_FetchImportedModuleFromScriptCallback, (void*)WScriptJsrt::FetchImportedModuleFromScript), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_FetchImportedModuleWithAttributesCallback, (void*)WScriptJsrt::FetchImportedModuleWithAttributes), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_FetchImportedModuleFromScriptWithAttributesCallback, (void*)WScriptJsrt::FetchImportedModuleFromScriptWithAttributes), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_NotifyModuleReadyCallback, (void*)WScriptJsrt::NotifyModuleReadyCallback), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_InitializeImportMetaCallback, (void*)WScriptJsrt::InitializeImportMetaCallback), false);
    IfJsrtErrorFail(ChakraRTInterface::JsSetModuleHostInfo(nullptr, JsModuleHostInfo_ReportModuleCompletionCallback, (void*)WScriptJsrt::ReportModuleCompletionCallback), false);

    if (HostConfigFlags::flags.Test262)
    {
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Broadcast", BroadcastCallback));
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "ReceiveBroadcast", ReceiveBroadcastCallback));
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Report", ReportCallback));
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "GetReport", GetReportCallback));
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Leaving", LeavingCallback));
        IfFalseGo(WScriptJsrt::InstallObjectsOnObject(wscript, "Sleep", SleepCallback));

        const char Test262[] =
            #include "262.js"
        ;

        JsValueRef Test262ScriptRef;
        IfJsrtErrorFailLogAndRetFalse(ChakraRTInterface::JsCreateString(Test262, strlen(Test262), &Test262ScriptRef));

        JsValueRef fname;
        IfJsrtErrorFailLogAndRetFalse(ChakraRTInterface::JsCreateString("262", strlen("262"), &fname));
        IfJsrtErrorFailLogAndRetFalse(ChakraRTInterface::JsRun(Test262ScriptRef, WScriptJsrt::GetNextSourceContext(), fname, JsParseScriptAttributeNone, nullptr));
    }

Error:
    return hr == S_OK;
}

bool WScriptJsrt::Uninitialize()
{
    auto& threadData = GetRuntimeThreadLocalData().threadData;
    const bool isMainThread = (threadData == nullptr || threadData->parent == nullptr);

    if (threadData && !threadData->children.empty())
    {
        for (auto i = threadData->children.begin(); i != threadData->children.end(); i++)
        {
            SetEvent((*i)->hevntShutdown);
        }

        std::vector<DWORD> waitResults;
        waitResults.reserve(threadData->children.size());
        for (auto i = threadData->children.begin(); i != threadData->children.end(); i++)
        {
            waitResults.push_back(WaitForSingleObject((*i)->hThread, INFINITE));
        }

        size_t waitIndex = 0;
        for (auto i = threadData->children.begin(); i != threadData->children.end(); i++)
        {
            if (waitResults[waitIndex++] == WAIT_OBJECT_0)
            {
                // Release any unconsumed broadcast content handles if the child
                // exited before creating a context (parent's context is active).
                (*i)->DrainBroadcastQueue(false);
                delete *i;
            }
        }

        threadData->children.clear();
    }

    if (isMainThread)
    {
        HostGlobalsLock lock;
        moduleRecordMap.clear();
        moduleDirMap.clear();
        moduleErrMap.clear();
        scriptDirMap.clear();
    }

    return true;
}

#if ENABLE_TTD
void CALLBACK WScriptJsrt::JsContextBeforeCollectCallback(JsRef contextRef, void *data)
{
    ChakraRTInterface::JsTTDNotifyContextDestroy(contextRef);
}
#endif

JsValueRef __stdcall WScriptJsrt::LoadTextFileCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;
    const char* fileContent = nullptr;

    if (argumentCount < 2)
    {
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));
    }
    else
    {
        AutoString fileName;

        IfJsrtErrorSetGo(fileName.Initialize(arguments[1]));

        if (errorCode == JsNoError)
        {
            UINT lengthBytes = 0;
            hr = Helpers::LoadScriptFromFile(*fileName, fileContent, &lengthBytes);

            if (FAILED(hr))
            {
                {
                    StderrLock lock;
                    fprintf(stderr, "Couldn't load file '%s'\n", fileName.GetString());
                    fflush(stderr);
                }
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));
                return returnValue;
            }

            IfJsrtErrorSetGo(ChakraRTInterface::JsCreateString(
                fileContent, lengthBytes, &returnValue));
        }
    }

Error:
    if (fileContent)
    {
        free((void*)fileContent);
    }
    return returnValue;
}

static int JsFgets(char* buf, int size, FILE* file)
{
    int n = size - 1;
    if (n < 0)
        return -1;

    bool crflag = false;
    int c, i = 0;

    for (i = 0; i < n && (c = getc(file)) != EOF; i++) {
        buf[i] = (char)c;
        if (c == '\n') {
            i++;
            break;
        }
        if (crflag) {
            ungetc(c, file);
            break;
        }
        crflag = (c == '\r');
    }

    buf[i] = '\0';
    return i;
}

JsValueRef __stdcall WScriptJsrt::ReadLineStdinCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    const int BUFSIZE = 256;
    FILE* from = stdin;
    int buflength = 0;
    int bufsize = BUFSIZE;
    char* buf = static_cast<char*>(malloc(bufsize));
    char* tmp;
    int gotlength = 0;

    if (!buf)
    {
        goto Error;
    }

    while ((gotlength = JsFgets(buf + buflength, bufsize - buflength, from)) > 0)
    {
        buflength += gotlength;

        if (buflength >= 2 && buf[buflength - 2] == '\r' && buf[buflength - 1] == '\n')
        {
            buf[buflength - 1] = '\0';
            buf[buflength - 2] = '\0';
            buflength -= 2;
            break;
        }
        else if (buflength >= 1 && buf[buflength - 1] == '\n')
        {
            buf[buflength - 1] = '\0';
            buflength -= 1;
            break;
        }
        else if (buflength < bufsize - 1)
        {
            break;
        }

        if (bufsize > INT_MAX / 2)
        {
            goto Error;
        }
        bufsize *= 2;
        if (bufsize > buflength)
        {
            tmp = static_cast<char*>(realloc(buf, bufsize));
        }
        else
        {
            goto Error;
        }

        if (!tmp)
        {
            goto Error;
        }

        buf = tmp;
    }

    if (buflength == 0)
    {
        if (feof(from))
        {
            goto Error;
        }
        else
        {
            JsValueRef emptyStringObject;
            IfJsrtErrorSetGo(ChakraRTInterface::JsCreateString(buf, buflength, &emptyStringObject));
            free(buf);
            return emptyStringObject;
        }
    }

    JsValueRef stringObject;
    IfJsrtErrorSetGo(ChakraRTInterface::JsCreateString(buf, buflength, &stringObject));
    free(buf);
    return stringObject;

Error:
    if (buf)
    {
        free(buf);
    }
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::LoadBinaryFileCallback(JsValueRef callee,
    bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;
    bool isHeapAlloc = true;

    if (argumentCount < 2)
    {
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));
    }
    else
    {
        const char *fileContent;
        AutoString fileName;

        IfJsrtErrorSetGo(fileName.Initialize(arguments[1]));

        if (errorCode == JsNoError)
        {
            UINT lengthBytes = 0;

            hr = Helpers::LoadBinaryFile(*fileName, fileContent, lengthBytes);

            if (FAILED(hr))
            {
                {
                    StderrLock lock;
                    fprintf(stderr, "Couldn't load file '%s'\n", fileName.GetString());
                    fflush(stderr);
                }
                IfJsrtErrorSetGoLabel(ChakraRTInterface::JsGetUndefinedValue(&returnValue), Error);
                return returnValue;
            }

            JsValueRef arrayBuffer;
            IfJsrtErrorSetGoLabel(ChakraRTInterface::JsCreateArrayBuffer(lengthBytes, &arrayBuffer), ErrorStillFree);
            BYTE* buffer;
            unsigned int bufferLength;
            IfJsrtErrorSetGoLabel(ChakraRTInterface::JsGetArrayBufferStorage(arrayBuffer, &buffer, &bufferLength), ErrorStillFree);
            if (bufferLength < lengthBytes)
            {
                StderrLock lock;
                fwprintf(stderr, _u("Array buffer size is insufficient to store the binary file.\n"));
                fflush(stderr);
            }
            else
            {
                if (memcpy_s(buffer, bufferLength, (BYTE*)fileContent, lengthBytes) == 0)
                {
                    returnValue = arrayBuffer;
                }
            }
ErrorStillFree:
            if (isHeapAlloc)
            {
                HeapFree(GetProcessHeap(), 0, (void*)fileContent);
            }
        }
    }

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::FlagCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));

#if ENABLE_DEBUG_CONFIG_OPTIONS
    if (argumentCount > 1)
    {
        AutoString cmd;
        IfJsrtErrorSetGo(cmd.Initialize(arguments[1]));
        char16* argv[] = { nullptr, cmd.GetWideString() };
        ChakraRTInterface::SetConfigFlags(2, argv, nullptr);
    }
#endif

Error:
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::GetProxyPropertiesCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsValueRef undefined = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&undefined));

    returnValue = undefined;

    if (argumentCount > 1)
    {
        bool isProxy = false;
        JsValueRef target;
        JsValueRef handler;
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetProxyProperties(arguments[1], &isProxy, &target, &handler));

        if (isProxy)
        {
            JsPropertyIdRef targetProperty;
            JsPropertyIdRef handlerProperty;
            JsPropertyIdRef revokedProperty;

            IfJsrtErrorSetGo(CreatePropertyIdFromString("target", &targetProperty));
            IfJsrtErrorSetGo(CreatePropertyIdFromString("handler", &handlerProperty));
            IfJsrtErrorSetGo(CreatePropertyIdFromString("revoked", &revokedProperty));
            IfJsrtErrorSetGo(ChakraRTInterface::JsCreateObject(&returnValue));

            JsValueRef revoked = JS_INVALID_REFERENCE;

            if (target == JS_INVALID_REFERENCE)
            {
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetTrueValue(&revoked));
                target = undefined;
                handler = undefined;
            }
            else
            {
                IfJsrtErrorSetGo(ChakraRTInterface::JsGetFalseValue(&revoked));
            }

            IfJsrtErrorSetGo(ChakraRTInterface::JsSetProperty(returnValue, handlerProperty, handler, true));
            IfJsrtErrorSetGo(ChakraRTInterface::JsSetProperty(returnValue, targetProperty, target, true));
            IfJsrtErrorSetGo(ChakraRTInterface::JsSetProperty(returnValue, revokedProperty, revoked, true));
        }
    }
Error:
    return returnValue;
}

static void SplitFileNameAndExt(
    LPCSTR fileName,
    CHAR (&shortFileName)[_MAX_PATH],
    CHAR (&ext)[_MAX_EXT],
    LPCSTR fallbackName = "(unknown)")
{
    shortFileName[0] = '\0';
    ext[0] = '\0';
    if (fileName != nullptr && fileName[0] != '\0')
    {
        if (_splitpath_s(fileName, nullptr, 0, nullptr, 0, shortFileName, _countof(shortFileName), ext, _countof(ext)) == 0)
        {
            return;
        }
    }
    if (fallbackName != nullptr && fallbackName[0] != '\0')
    {
        strcpy_s(shortFileName, _countof(shortFileName), fallbackName);
    }
    ext[0] = '\0';
}

bool WScriptJsrt::PrintException(LPCSTR fileName, JsErrorCode jsErrorCode, JsValueRef exception)
{
    LPCWSTR errorTypeString = ConvertErrorCodeToMessage(jsErrorCode);
    JsValueRef metaData = JS_INVALID_REFERENCE;

    if (exception == nullptr)
    {
        if (ChakraRTInterface::JsGetAndClearExceptionWithMetadata(&metaData) == JsNoError)
        {
            JsPropertyIdRef exceptionId = JS_INVALID_REFERENCE;
            IfJsrtErrorFail(CreatePropertyIdFromString("exception", &exceptionId), false);
            IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(metaData, exceptionId, &exception), false);
        }
        else
        {
            IfJsrtErrorFail(ChakraRTInterface::JsGetAndClearException(&exception), false);
        }
    }

    if (HostConfigFlags::flags.MuteHostErrorMsgIsEnabled)
    {
        return false;
    }

    if (exception != nullptr)
    {
        if (jsErrorCode == JsErrorCode::JsErrorScriptCompile || jsErrorCode == JsErrorCode::JsErrorScriptException)
        {
            AutoString errorMessage;

            if (errorMessage.Initialize(exception) != JsNoError)
            {
                {
                    StderrLock lock;
                    fwprintf(stderr, _u("ERROR attempting to coerce error to string, using alternate handler\n"));
                    fflush(stderr);
                }
                bool hasException = false;
                ChakraRTInterface::JsHasException(&hasException);
                if (hasException)
                {
                    JsValueRef throwAway = JS_INVALID_REFERENCE;
                    ChakraRTInterface::JsGetAndClearException(&throwAway);
                }
                JsPropertyIdRef messagePropertyId = JS_INVALID_REFERENCE;
                IfJsrtErrorFail(CreatePropertyIdFromString("message", &messagePropertyId), false);
                JsValueRef message = JS_INVALID_REFERENCE;
                IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(exception, messagePropertyId, &message), false);
                IfJsrtErrorFail(errorMessage.Initialize(message), false);

                if (jsErrorCode != JsErrorCode::JsErrorScriptCompile)
                {
                    CHAR shortFileName[_MAX_PATH] = { 0 };
                    CHAR ext[_MAX_EXT] = { 0 };
                    SplitFileNameAndExt(fileName, shortFileName, ext, "(unknown)");
                    const WCHAR *errMsgWide = errorMessage.GetWideString();
                    if (errMsgWide == nullptr)
                    {
                        errMsgWide = _u("");
                    }

                    if (metaData != JS_INVALID_REFERENCE)
                    {
                        JsPropertyIdRef linePropertyId = JS_INVALID_REFERENCE;
                        JsValueRef lineProperty = JS_INVALID_REFERENCE;
                        JsPropertyIdRef columnPropertyId = JS_INVALID_REFERENCE;
                        JsValueRef columnProperty = JS_INVALID_REFERENCE;

                        int line;
                        int column;

                        IfJsrtErrorFail(CreatePropertyIdFromString("line", &linePropertyId), false);
                        IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(metaData, linePropertyId, &lineProperty), false);
                        IfJsrtErrorFail(ChakraRTInterface::JsNumberToInt(lineProperty, &line), false);

                        IfJsrtErrorFail(CreatePropertyIdFromString("column", &columnPropertyId), false);
                        IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(metaData, columnPropertyId, &columnProperty), false);
                        IfJsrtErrorFail(ChakraRTInterface::JsNumberToInt(columnProperty, &column), false);
                        {
                            StderrLock lock;
                            fwprintf(stderr, _u("%ls\n"), errMsgWide);
                            fprintf(stderr, "        at code (%s%s:%d:%d)\n",
                                shortFileName, ext, line + 1, column + 1);
                            fflush(stderr);
                        }
                    }
                    else
                    {
                        StderrLock lock;
                        fwprintf(stderr, _u("%ls\n"), errMsgWide);
                        fprintf(stderr, "\tat code (%s%s:\?\?:\?\?)\n", shortFileName, ext);
                        fflush(stderr);
                    }
                    return true;
                }
            }

            if (jsErrorCode == JsErrorCode::JsErrorScriptCompile)
            {
                JsPropertyIdRef linePropertyId = JS_INVALID_REFERENCE;
                JsValueRef lineProperty = JS_INVALID_REFERENCE;
                JsPropertyIdRef columnPropertyId = JS_INVALID_REFERENCE;
                JsValueRef columnProperty = JS_INVALID_REFERENCE;

                int line;
                int column;

                bool hasPosition =
                    CreatePropertyIdFromString("line", &linePropertyId) == JsNoError &&
                    ChakraRTInterface::JsGetProperty(exception, linePropertyId, &lineProperty) == JsNoError &&
                    ChakraRTInterface::JsNumberToInt(lineProperty, &line) == JsNoError &&
                    CreatePropertyIdFromString("column", &columnPropertyId) == JsNoError &&
                    ChakraRTInterface::JsGetProperty(exception, columnPropertyId, &columnProperty) == JsNoError &&
                    ChakraRTInterface::JsNumberToInt(columnProperty, &column) == JsNoError;

                CHAR shortFileName[_MAX_PATH] = { 0 };
                CHAR ext[_MAX_EXT] = { 0 };
                SplitFileNameAndExt(fileName, shortFileName, ext, "(unknown)");
                const WCHAR *errMsgWide = errorMessage.GetWideString();
                if (errMsgWide == nullptr)
                {
                    errMsgWide = _u("");
                }
                if (hasPosition)
                {
                    StderrLock lock;
                    fwprintf(stderr, _u("%ls\n"), errMsgWide);
                    fprintf(stderr, "\tat code (%s%s:%d:%d)\n",
                        shortFileName, ext, (int)line + 1,
                        (int)column + 1);
                    fflush(stderr);
                }
                else
                {
                    StderrLock lock;
                    fwprintf(stderr, _u("%ls\n"), errMsgWide);
                    fprintf(stderr, "\tat code (%s%s:?)\n",
                        shortFileName, ext);
                    fflush(stderr);
                }
            }
            else
            {
                JsValueType propertyType = JsUndefined;
                JsPropertyIdRef stackPropertyId = JS_INVALID_REFERENCE;
                JsValueRef stackProperty = JS_INVALID_REFERENCE;
                AutoString errorStack;

                JsErrorCode errorCode = CreatePropertyIdFromString("stack", &stackPropertyId);

                if (errorCode == JsErrorCode::JsNoError)
                {
                    errorCode = ChakraRTInterface::JsGetProperty(exception, stackPropertyId, &stackProperty);
                    if (errorCode == JsErrorCode::JsNoError)
                    {
                        errorCode = ChakraRTInterface::JsGetValueType(stackProperty, &propertyType);
                    }
                }

                if (errorCode != JsErrorCode::JsNoError || propertyType == JsUndefined)
                {
                    CHAR shortFileName[_MAX_PATH] = { 0 };
                    CHAR ext[_MAX_EXT] = { 0 };
                    SplitFileNameAndExt(fileName, shortFileName, ext, "(unknown)");
                    const WCHAR *errMsgWide = errorMessage.GetWideString();
                    if (errMsgWide == nullptr)
                    {
                        errMsgWide = _u("");
                    }

                    StderrLock lock;
                    fprintf(stderr, "thrown at %s%s:\n^\n", shortFileName, ext);
                    fwprintf(stderr, _u("%ls\n"), errMsgWide);
                    fflush(stderr);
                }
                else
                {
                    IfJsrtErrorFail(errorStack.Initialize(stackProperty), false);
                    StderrLock lock;
                    fwprintf(stderr, _u("%ls\n"), errorStack.GetWideString());
                    fflush(stderr);
                }
            }
        }
        else
        {
            StderrLock lock;
            fwprintf(stderr, _u("Error : %ls\n"), errorTypeString);
            fflush(stderr);
        }
        return true;
    }
    else
    {
        StderrLock lock;
        fwprintf(stderr, _u("Error : %ls\n"), errorTypeString);
        fflush(stderr);
    }
    return false;
}

void WScriptJsrt::AddMessageQueue(MessageQueue *_messageQueue)
{
    Assert(messageQueue == nullptr);

    messageQueue = _messageQueue;
}

WScriptJsrt::CallbackMessage::CallbackMessage(unsigned int time, JsValueRef function, bool isTimerContinuation)
    : MessageBase(time), m_function(function), m_hasRef(false), m_isTimerContinuation(isTimerContinuation), m_addRefError(JsNoError)
{
    if (m_function == JS_INVALID_REFERENCE)
    {
        m_addRefError = JsErrorNullArgument;
        return;
    }

    JsErrorCode error = ChakraRTInterface::JsAddRef(m_function, nullptr);
    if (error != JsNoError)
    {
        m_addRefError = error;
        m_function = JS_INVALID_REFERENCE;
        return;
    }

    m_hasRef = true;
}

WScriptJsrt::CallbackMessage::~CallbackMessage()
{
    bool hasException = false;
    ChakraRTInterface::JsHasException(&hasException);
    if (hasException)
    {
        ReportNegativePhaseMarker(false);
        WScriptJsrt::PrintException("", JsErrorScriptException);
    }
    if (m_hasRef && m_function != JS_INVALID_REFERENCE)
    {
        JsErrorCode errorCode = ChakraRTInterface::JsRelease(m_function, nullptr);
        Assert(errorCode == JsNoError);
    }
    m_hasRef = false;
    m_function = JS_INVALID_REFERENCE;
}

HRESULT WScriptJsrt::CallbackMessage::Call(LPCSTR fileName)
{
    return CallFunction(fileName);
}

HRESULT WScriptJsrt::CallbackMessage::CallFunction(LPCSTR fileName)
{
    if (!m_hasRef || m_function == JS_INVALID_REFERENCE)
    {
        return E_FAIL;
    }

    HRESULT hr = S_OK;

    JsValueRef global;
    JsValueRef result;
    JsValueRef stringValue;
    JsValueType type;
    JsErrorCode errorCode = JsNoError;

    IfJsrtErrorHR(ChakraRTInterface::JsGetGlobalObject(&global));
    IfJsrtErrorHR(ChakraRTInterface::JsGetValueType(m_function, &type));

    if (type == JsString)
    {
        IfJsrtErrorHR(ChakraRTInterface::JsConvertValueToString(m_function, &stringValue));

        JsValueRef fname;
        ChakraRTInterface::JsCreateString("", strlen(""), &fname);
        errorCode = ChakraRTInterface::JsRun(stringValue, JS_SOURCE_CONTEXT_NONE,
          fname, JsParseScriptAttributeArrayBufferIsUtf16Encoded,
          nullptr /*no result needed*/);
    }
    else
    {
        errorCode = ChakraRTInterface::JsCallFunction(m_function, &global, 1, &result);
    }

    if (errorCode != JsNoError)
    {
        hr = E_FAIL;
        ReportNegativePhaseMarker(false);
        PrintException(fileName, errorCode);
    }

Error:
    return hr;
}

void WScriptJsrt::PromiseContinuationCallback(JsValueRef task, void *callbackState)
{
    Assert(task != JS_INVALID_REFERENCE);
    Assert(callbackState != JS_INVALID_REFERENCE);
    MessageQueue * targetQueue = (MessageQueue *)callbackState;

    WScriptJsrt::CallbackMessage *msg = new (std::nothrow) WScriptJsrt::CallbackMessage(0, task);
    if (msg == nullptr || !msg->HasValidRef())
    {
        {
            StderrLock lock;
            fwprintf(stderr, _u("WARNING: failed to retain promise continuation task (err=0x%x)\n"),
                msg ? msg->GetAddRefError() : JsErrorOutOfMemory);
            fflush(stderr);
        }
        delete msg;
        return;
    }
    targetQueue->InsertSorted(msg, /*immediate*/ true);
}

void WScriptJsrt::TimerContinuationCallback(JsValueRef task, unsigned int delayMilliseconds, void *callbackState)
{
    Assert(task != JS_INVALID_REFERENCE);
    Assert(callbackState != JS_INVALID_REFERENCE);
    MessageQueue * targetQueue = (MessageQueue *)callbackState;

    WScriptJsrt::CallbackMessage *msg = new (std::nothrow) WScriptJsrt::CallbackMessage(delayMilliseconds, task, true);
    if (msg == nullptr || !msg->HasValidRef())
    {
        {
            StderrLock lock;
            fwprintf(stderr, _u("WARNING: failed to retain timer continuation task (err=0x%x)\n"),
                msg ? msg->GetAddRefError() : JsErrorOutOfMemory);
            fflush(stderr);
        }
        delete msg;
        return;
    }
    targetQueue->InsertSorted(msg);
}

void WScriptJsrt::TimerContinuationCancelCallback(JsValueRef task, void *callbackState)
{
    if (task == JS_INVALID_REFERENCE || callbackState == nullptr)
    {
        return;
    }
    MessageQueue * targetQueue = (MessageQueue *)callbackState;
    targetQueue->RemoveTimerTask(task);
}

static void ClearPendingPromiseTrackerException()
{
    bool hasException = false;
    if (ChakraRTInterface::JsHasException(&hasException) == JsNoError && hasException)
    {
        JsValueRef exception = JS_INVALID_REFERENCE;
        ChakraRTInterface::JsGetAndClearException(&exception);
    }
}

void WScriptJsrt::PromiseRejectionTrackerCallback(JsValueRef promise, JsValueRef reason, bool handled, void *callbackState)
{
    Assert(promise != JS_INVALID_REFERENCE);
    Assert(reason != JS_INVALID_REFERENCE);

    if (!handled)
    {
        wprintf(_u("Uncaught promise rejection\n"));
    }
    else
    {
        wprintf(_u("Promise rejection handled\n"));
    }

    JsPropertyIdRef stackPropertyID;
    JsErrorCode error = ChakraRTInterface::JsCreatePropertyId("stack", strlen("stack"), &stackPropertyID);
    if (error == JsNoError)
    {
        JsValueRef stack;
        error = ChakraRTInterface::JsGetProperty(reason, stackPropertyID, &stack);
        if (error == JsNoError)
        {
            JsValueRef stackStrValue;
            error = ChakraRTInterface::JsConvertValueToString(stack, &stackStrValue);
            if (error == JsNoError)
            {
                AutoString str(stackStrValue);
                if (str.GetError() == JsNoError && str.GetWideString() != nullptr)
                {
                    wprintf(_u("%ls\n"), str.GetWideString());
                }
            }
            else
            {
                ClearPendingPromiseTrackerException();
            }
        }
        else
        {
            ClearPendingPromiseTrackerException();
        }
    }

    if (error != JsNoError)
    {
        JsValueRef strValue;
        error = ChakraRTInterface::JsConvertValueToString(reason, &strValue);
        if (error == JsNoError)
        {
            AutoString str(strValue);
            if (str.GetError() == JsNoError && str.GetWideString() != nullptr)
            {
                wprintf(_u("%ls\n"), str.GetWideString());
            }
        }
        else
        {
            ClearPendingPromiseTrackerException();
        }
    }

    fflush(stdout);
}
