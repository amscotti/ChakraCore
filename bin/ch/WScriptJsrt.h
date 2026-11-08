//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once
#include <list>

enum ModuleState
{
    RootModule,
    ImportedModule,
    ErroredModule
};

class WScriptJsrt
{
public:
    static bool Initialize();
    static bool Uninitialize();
    static JsErrorCode ModuleEntryPoint(LPCSTR fileName, LPCSTR fileContent, LPCSTR fullName);

    class CallbackMessage : public MessageBase
    {
        JsValueRef m_function;

        CallbackMessage(CallbackMessage const&);

    public:
        CallbackMessage(unsigned int time, JsValueRef function);
        ~CallbackMessage();

        HRESULT Call(LPCSTR fileName);
        HRESULT CallFunction(LPCSTR fileName);
        template <class Func>
        static CallbackMessage* Create(JsValueRef function, const Func& func, unsigned int time = 0)
        {
            return new CustomMessage<Func, CallbackMessage>(time, function, func);
        }
    };

    class ModuleMessage : public MessageBase
    {
    private:
        JsModuleRecord moduleRecord;
        JsValueRef specifier;
        std::string* filePath;
        std::string* mapKey;
        bool isJsonModule;
        bool directParse;

        ModuleMessage(JsModuleRecord module, JsValueRef specifier, std::string* filePathPtr, std::string* mapKeyPtr, bool isJson, bool direct);

    public:
        ~ModuleMessage();

        virtual HRESULT Call(LPCSTR fileName) override;

        static ModuleMessage* Create(JsModuleRecord module, JsValueRef specifier, std::string* fullPath = nullptr, bool isJson = false)
        {
            return new ModuleMessage(module, specifier, fullPath, fullPath, isJson, false);
        }

        // Fetch messages for requests carrying import attributes: the record
        // is registered under `mapKey` (which embeds the attributes), so the
        // parse step must target the stored record directly instead of
        // re-deriving it from the file path.
        static ModuleMessage* CreateWithAttributes(JsModuleRecord module, JsValueRef specifier, std::string* filePath, std::string* mapKey, bool isJson)
        {
            return new ModuleMessage(module, specifier, filePath, mapKey, isJson, true);
        }

    };

    static void AddMessageQueue(MessageQueue *messageQueue);
    // Module fetch/notification messages go to the CURRENT thread's queue:
    // the static main queue on the main thread, the agent's own queue on a
    // crossthread agent thread (S6e). Before R1 this pushed everything onto
    // the main queue unconditionally, so a dynamic import issued inside an
    // agent raced the main loop's queue and its record was parsed/evaluated
    // on the wrong thread — the import promise then never settled.
    static void PushMessage(MessageBase *message) { GetThreadMessageQueue()->InsertSorted(message); }

    // Negative-phase classification (host flag -NegativePhaseReport). The
    // marker lets a test runner re-judge negative scenarios by the stage the
    // failure actually originated from, replacing output-shape heuristics;
    // classification mapping and limits in doc/c1-negative-phase.md.
    static void NoteModuleGraphFailure();
    static void ReportNegativePhaseMarker(bool parseStageFailure);

    static JsErrorCode FetchImportedModule(_In_ JsModuleRecord referencingModule, _In_ JsValueRef specifier, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord);
    static JsErrorCode FetchImportedModuleFromScript(_In_ DWORD_PTR dwReferencingSourceContext, _In_ JsValueRef specifier, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord);
    static JsErrorCode FetchImportedModuleWithAttributes(_In_ JsModuleRecord referencingModule, _In_ JsValueRef specifier, _In_ JsValueRef attributes, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord);
    static JsErrorCode FetchImportedModuleFromScriptWithAttributes(_In_ JsSourceContext dwReferencingSourceContext, _In_ JsValueRef specifier, _In_ JsValueRef attributes, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord);
    static JsErrorCode NotifyModuleReadyCallback(_In_opt_ JsModuleRecord referencingModule, _In_opt_ JsValueRef exceptionVar);
    static JsErrorCode ReportModuleCompletionCallback(JsModuleRecord module, JsValueRef exception);
    static JsErrorCode CALLBACK InitializeImportMetaCallback(_In_opt_ JsModuleRecord referencingModule, _In_opt_ JsValueRef importMetaVar);
    static void CALLBACK PromiseContinuationCallback(JsValueRef task, void *callbackState);
    static void CALLBACK TimerContinuationCallback(JsValueRef task, unsigned int delayMilliseconds, void *callbackState);
    static void CALLBACK PromiseRejectionTrackerCallback(JsValueRef promise, JsValueRef reason, bool handled, void *callbackState);

    static LPCWSTR ConvertErrorCodeToMessage(JsErrorCode errorCode)
    {
        switch (errorCode)
        {
        case (JsErrorCode::JsErrorInvalidArgument) :
            return _u("TypeError: InvalidArgument");
        case (JsErrorCode::JsErrorNullArgument) :
            return _u("TypeError: NullArgument");
        case (JsErrorCode::JsErrorArgumentNotObject) :
            return _u("TypeError: ArgumentNotAnObject");
        case (JsErrorCode::JsErrorOutOfMemory) :
            return _u("OutOfMemory");
        case (JsErrorCode::JsErrorScriptException) :
            return _u("ScriptError");
        case (JsErrorCode::JsErrorScriptCompile) :
            return _u("SyntaxError");
        case (JsErrorCode::JsErrorFatal) :
            return _u("FatalError");
        case (JsErrorCode::JsErrorInExceptionState) :
            return _u("ErrorInExceptionState");
        case (JsErrorCode::JsErrorBadSerializedScript):
            return _u("ErrorBadSerializedScript ");
        default:
            AssertMsg(false, "Unexpected JsErrorCode");
            return nullptr;
        }
    }

#if ENABLE_TTD
    static void CALLBACK JsContextBeforeCollectCallback(JsRef contextRef, void *data);
#endif

    static bool PrintException(LPCSTR fileName, JsErrorCode jsErrorCode, JsValueRef exception = nullptr);
    static JsValueRef LoadScript(JsValueRef callee, LPCSTR fileName, LPCSTR fileContent, LPCSTR scriptInjectType, bool isSourceModule, JsFinalizeCallback finalizeCallback, bool isFile);
    static DWORD_PTR GetNextSourceContext();
    static JsValueRef LoadScriptFileHelper(JsValueRef callee, JsValueRef *arguments, unsigned short argumentCount, bool isSourceModule);
    static JsValueRef LoadScriptHelper(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState, bool isSourceModule);
    static bool InstallObjectsOnObject(JsValueRef object, const char* name, JsNativeFunction nativeFunction);
    static void FinalizeFree(void * addr);
    static void RegisterScriptDir(DWORD_PTR sourceContext, LPCSTR fullDirNarrow);
private:
    static void SetExceptionIf(JsErrorCode errorCode, LPCWSTR errorMessage);
    static bool CreateArgumentsObject(JsValueRef *argsObject);
    static bool CreateNamedFunction(const char*, JsNativeFunction callback, JsValueRef* functionVar);
    static void GetDir(LPCSTR fullPathNarrow, std::string *fullDirNarrow);
    static JsValueRef CALLBACK EchoCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK QuitCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK LoadScriptFileCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK LoadScriptCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK LoadModuleCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK GetModuleNamespace(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK MonotonicNowCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK SetTimeoutCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK ClearTimeoutCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK AttachCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK DetachCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK DumpFunctionPositionCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK RequestAsyncBreakCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);

    static JsErrorCode CALLBACK LoadModuleFromString(LPCSTR fileName, LPCSTR fileContent, LPCSTR fullName = nullptr, bool isFile = false);

    static JsValueRef CALLBACK LoadBinaryFileCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK LoadTextFileCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK RegisterModuleSourceCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK FlagCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK ReadLineStdinCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    
    static JsValueRef CALLBACK BroadcastCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK ReceiveBroadcastCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK ReportCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK GetReportCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK LeavingCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK SleepCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK GetProxyPropertiesCallback(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);

    static JsValueRef CALLBACK SerializeObject(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);
    static JsValueRef CALLBACK Deserialize(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState);

    static JsErrorCode FetchImportedModuleHelper(JsModuleRecord referencingModule, JsValueRef specifier, _Out_ JsModuleRecord* dependentModuleRecord, LPCSTR refdir = nullptr);
    static JsErrorCode FetchImportedModuleHelperWithAttributes(JsModuleRecord referencingModule, JsValueRef specifier, JsValueRef attributes, _Out_ JsModuleRecord* dependentModuleRecord, LPCSTR refdir = nullptr);

    // Message queue for the current thread: the per-child agent queue when
    // running on a crossthread agent thread (S6e), otherwise the main host
    // queue. Prevents WScript.SetTimeout/ClearTimeout and samethread script
    // loads issued from an agent from racing the main thread's queue.
    static MessageQueue* GetThreadMessageQueue();

    // Process-global module/script bookkeeping. Since S6e, agent threads
    // mutate these concurrently with the main thread: every access in
    // WScriptJsrt.cpp takes the host-globals critical section, and
    // sourceContext is additionally interlocked (see GetNextSourceContext).
    static MessageQueue *messageQueue;
    static DWORD_PTR sourceContext;
    static std::map<std::string, JsModuleRecord> moduleRecordMap;
    static std::map<JsModuleRecord, std::string> moduleDirMap;
    static std::map<JsModuleRecord, ModuleState> moduleErrMap;
    static std::map<DWORD_PTR, std::string> scriptDirMap;
};
