//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

static JsErrorCode ReadModuleAttributes(JsValueRef attributes, bool* hasAttributes, std::string* canonicalOut, std::string* typeOut);
static char* WrapJsonModuleSource(LPCSTR jsonText);
static char* BuildJsonParseSource(LPCSTR jsonText, const char* prefix, const char* suffix);
static bool ValidateJsonModuleSource(JsModuleRecord moduleRecord, LPCSTR jsonParseSource);

static bool IsDynamicModuleState(ModuleState state)
{
    return state == DynamicImportModule || state == EvaluatedModule;
}

// Parses module source directly on an already-registered record. Used for
// attribute-bearing fetches whose map key embeds the attributes.
static void ParseModuleSourceOnRecord(JsModuleRecord module, LPCSTR fileContent, unsigned int contentLength)
{
    JsValueRef errorObject = JS_INVALID_REFERENCE;
    ModuleParseScope parseScope;
    ChakraRTInterface::JsParseModuleSource(module, WScriptJsrt::GetNextSourceContext(),
        (LPBYTE)fileContent, contentLength, JsParseModuleSourceFlags_DataIsUTF8, &errorObject);
}

static bool IsAbsolutePath(LPCSTR specifier)
{
    if (specifier == nullptr || specifier[0] == '\0')
    {
        return false;
    }
    if (specifier[0] == '/' || specifier[0] == '\\')
    {
        return true;
    }
    return (isalpha((unsigned char)specifier[0]) && specifier[1] == ':');
}

JsErrorCode WScriptJsrt::ModuleEntryPoint(LPCSTR fileName, LPCSTR fileContent, LPCSTR fullName)
{
    return LoadModuleFromString(fileName, fileContent, fullName, true);
}

JsErrorCode WScriptJsrt::LoadModuleFromString(LPCSTR fileName, LPCSTR fileContent, LPCSTR fullName, bool isFile)
{
    DWORD_PTR dwSourceCookie = WScriptJsrt::GetNextSourceContext();
    JsModuleRecord requestModule = JS_INVALID_REFERENCE;
    LPCSTR moduleRecordKey = fullName ? fullName : fileName;
    JsErrorCode errorCode = JsNoError;

    {
        HostGlobalsLock lock;
        auto moduleRecordEntry = moduleRecordMap.find(std::string(moduleRecordKey));

        if (moduleRecordEntry == moduleRecordMap.end())
        {
            JsValueRef specifier = nullptr;
            if (isFile && fullName)
            {
                errorCode = ChakraRTInterface::JsCreateString(
                    fullName, strlen(fullName), &specifier);
            }
            if (errorCode == JsNoError)
            {
                errorCode = ChakraRTInterface::JsInitializeModuleRecord(
                    nullptr, specifier, &requestModule);
            }
            if (errorCode == JsNoError)
            {
                if (fullName)
                {
                    GetDir(fullName, &moduleDirMap[requestModule]);
                }

                moduleRecordMap[std::string(moduleRecordKey)] = requestModule;
                moduleErrMap[requestModule] = RootModule;
            }
        }
        else
        {
            requestModule = moduleRecordEntry->second;
        }
    }
    IfJsrtErrorFailLogAndRetErrorCode(errorCode);
    JsValueRef errorObject = JS_INVALID_REFERENCE;

    unsigned int fileContentLength = (fileContent == nullptr) ? 0 : (unsigned int)strlen(fileContent);
    {
        ModuleParseScope parseScope;
        errorCode = ChakraRTInterface::JsParseModuleSource(requestModule, dwSourceCookie, (LPBYTE)fileContent,
            fileContentLength, JsParseModuleSourceFlags_DataIsUTF8, &errorObject);
    }

    ModuleState stateAtParse = RootModule;
    {
        HostGlobalsLock lock;
        stateAtParse = moduleErrMap[requestModule];
    }
    const bool rootAtParse = (stateAtParse == RootModule);
    const bool dynamicAtParse = (stateAtParse == DynamicImportModule);

    if ((errorCode != JsNoError) && errorObject != JS_INVALID_REFERENCE && fileContent != nullptr && !HostConfigFlags::flags.IgnoreScriptErrorCode && rootAtParse)
    {
        ChakraRTInterface::JsSetException(errorObject);
        {
            HostGlobalsLock lock;
            moduleErrMap[requestModule] = ErroredModule;
        }
        return errorCode;
    }
    if (errorCode != JsNoError && !rootAtParse && !dynamicAtParse)
    {
        NoteModuleGraphFailure();
    }
    return JsNoError;
}

JsValueRef __stdcall WScriptJsrt::GetModuleNamespace(JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    JsErrorCode errorCode = JsNoError;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    LPCWSTR errorMessage = _u("");
    char fullPath[_MAX_PATH];

    if (argumentCount < 2)
    {
        errorCode = JsErrorInvalidArgument;
        errorMessage = _u("Need an argument for WScript.GetModuleNamespace");
    }
    else
    {
        AutoString specifierStr(arguments[1]);
        errorCode = specifierStr.GetError();

        if (errorCode == JsNoError)
        {
            if (_fullpath(fullPath, specifierStr.GetString(), _MAX_PATH) == nullptr)
            {
                errorCode = JsErrorInvalidArgument;
            }
            else
            {
                JsModuleRecord record;
                {
                    HostGlobalsLock lock;
                    auto moduleEntry = moduleRecordMap.find(fullPath);
                    if (moduleEntry == moduleRecordMap.end())
                    {
                        record = JS_INVALID_REFERENCE;
                    }
                    else
                    {
                        record = moduleEntry->second;
                    }
                }
                if (record == JS_INVALID_REFERENCE)
                {
                    errorCode = JsErrorInvalidArgument;
                    errorMessage = _u("Need to supply a path for an already loaded module for WScript.GetModuleNamespace");
                }
                else
                {
                    errorCode = ChakraRTInterface::JsGetModuleNamespace(record, &returnValue);
                    if (errorCode == JsErrorModuleNotEvaluated)
                    {
                        errorMessage = _u("GetModuleNamespace called with un-evaluated module");
                    }
                }
            }
        }
    }

    SetExceptionIf(errorCode, errorMessage);
    return returnValue;
}

JsValueRef __stdcall WScriptJsrt::RegisterModuleSourceCallback(JsValueRef callee, bool isConstructCall,
    JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
{
    HRESULT hr = E_FAIL;
    JsValueRef returnValue = JS_INVALID_REFERENCE;
    JsErrorCode errorCode = JsNoError;

    if (argumentCount < 3)
    {
        IfJsrtErrorSetGo(ChakraRTInterface::JsGetUndefinedValue(&returnValue));
    }
    else
    {
        AutoString fileName;
        AutoString data;
        IfJsrtErrorSetGo(fileName.Initialize(arguments[1]));
        IfJsrtErrorSetGo(data.Initialize(arguments[2]));

        SourceMap::Add(fileName, data);
    }

Error:
    return returnValue;
}

WScriptJsrt::ModuleMessage::ModuleMessage(JsModuleRecord module, JsValueRef specifier, std::string* filePathPtr, std::string* mapKeyPtr, bool isJson, bool direct)
    : MessageBase(0), moduleRecord(module), specifier(specifier), isJsonModule(isJson), directParse(direct), hasModuleRef(false), hasSpecifierRef(false)
{
    filePath = nullptr;
    mapKey = nullptr;
    if (moduleRecord != nullptr)
    {
        hasModuleRef = (ChakraRTInterface::JsAddRef(moduleRecord, nullptr) == JsNoError);
    }
    if (specifier != nullptr)
    {
        if (filePathPtr != nullptr)
        {
            filePath = new std::string(*filePathPtr);
        }
        if (mapKeyPtr != nullptr)
        {
            mapKey = new std::string(*mapKeyPtr);
        }
        hasSpecifierRef = (ChakraRTInterface::JsAddRef(specifier, nullptr) == JsNoError);
    }
}

WScriptJsrt::ModuleMessage::~ModuleMessage()
{
    if (hasModuleRef && moduleRecord != nullptr)
    {
        ChakraRTInterface::JsRelease(moduleRecord, nullptr);
    }
    if (specifier != nullptr)
    {
        delete filePath;
        delete mapKey;
        if (hasSpecifierRef)
        {
            ChakraRTInterface::JsRelease(specifier, nullptr);
        }
    }
}

HRESULT WScriptJsrt::ModuleMessage::Call(LPCSTR fileName)
{
    JsErrorCode errorCode = JsNoError;
    JsValueRef result = JS_INVALID_REFERENCE;
    HRESULT hr;
    if (specifier == nullptr)
    {
        ModuleState state = RootModule;
        {
            HostGlobalsLock lock;
            auto it = moduleErrMap.find(moduleRecord);
            if (it != moduleErrMap.end())
            {
                state = it->second;
            }
            if (state == RootModule)
            {
                moduleErrMap[moduleRecord] = EvaluatedModule;
            }
        }
        if (state != ErroredModule)
        {
            errorCode = ChakraRTInterface::JsModuleEvaluation(moduleRecord, &result);
            if (errorCode != JsNoError)
            {
                ReportNegativePhaseMarker(false);
                PrintException(fileName, errorCode);
            }
            else if (state == RootModule)
            {
                // Static module evaluation completed (either cleanly or after
                // ReportModuleCompletionCallback consumed the resolution witness).
                ClearModuleGraphFailureWitness();
            }
        }
    }
    else
    {
        bool isDynamicImport = false;
        {
            HostGlobalsLock lock;
            JsModuleRecord targetRecord = directParse ? moduleRecord : nullptr;
            if (targetRecord == nullptr && mapKey != nullptr)
            {
                auto it = moduleRecordMap.find(*mapKey);
                if (it != moduleRecordMap.end())
                {
                    targetRecord = it->second;
                }
            }
            if (targetRecord != nullptr)
            {
                auto errIt = moduleErrMap.find(targetRecord);
                if (errIt != moduleErrMap.end() && errIt->second == DynamicImportModule)
                {
                    isDynamicImport = true;
                }
            }
        }

        LPCSTR fileContent = nullptr;
        AutoString specifierStr(specifier);
        errorCode = specifierStr.GetError();
        if (errorCode == JsNoError)
        {
            hr = Helpers::LoadScriptFromFile(*specifierStr, fileContent, nullptr, filePath, true);

            if (FAILED(hr))
            {
                if (!isDynamicImport)
                {
                    NoteModuleGraphFailure();
                }
                if (!HostConfigFlags::flags.MuteHostErrorMsgIsEnabled)
                {
                    bool reportLoadFailure;
                    {
                        HostGlobalsLock lock;
                        auto actualModuleRecord = moduleRecordMap.find(*mapKey);
                        reportLoadFailure = (actualModuleRecord == moduleRecordMap.end() ||
                                             moduleErrMap[actualModuleRecord->second] == RootModule);
                    }
                    if (reportLoadFailure)
                    {
                        fprintf(stderr, "Couldn't load file '%s'\n", specifierStr.GetString());
                    }
                }
                if (directParse)
                {
                    ParseModuleSourceOnRecord(moduleRecord, nullptr, 0);
                }
                else
                {
                    LoadScript(nullptr, filePath == nullptr ? *specifierStr : filePath->c_str(), nullptr, "module", true, WScriptJsrt::FinalizeFree, false);
                }
                goto Error;
            }
            if (isJsonModule)
            {
                char* jsonValidationSource = BuildJsonParseSource(fileContent, "JSON.parse('", "');");
                bool jsonValid = (jsonValidationSource != nullptr) &&
                    ValidateJsonModuleSource(moduleRecord, jsonValidationSource);
                free((void*)jsonValidationSource);
                char* jsonModuleSource = WrapJsonModuleSource(fileContent);
                free((void*)fileContent);
                fileContent = nullptr;
                if (jsonModuleSource == nullptr || jsonValidationSource == nullptr)
                {
                    if (!isDynamicImport)
                    {
                        NoteModuleGraphFailure();
                    }
                    hr = E_OUTOFMEMORY;
                    if (directParse)
                    {
                        ParseModuleSourceOnRecord(moduleRecord, nullptr, 0);
                    }
                    else
                    {
                        LoadScript(nullptr, filePath == nullptr ? *specifierStr : filePath->c_str(), nullptr, "module", true, WScriptJsrt::FinalizeFree, false);
                    }
                    free((void*)jsonModuleSource);
                    goto Error;
                }
                if (!jsonValid)
                {
                    if (!isDynamicImport)
                    {
                        NoteModuleGraphFailure();
                    }
                    if (directParse)
                    {
                        ParseModuleSourceOnRecord(moduleRecord, nullptr, 0);
                    }
                    else
                    {
                        LoadScript(nullptr, filePath == nullptr ? *specifierStr : filePath->c_str(), nullptr, "module", true, WScriptJsrt::FinalizeFree, false);
                    }
                    free((void*)jsonModuleSource);
                    goto Error;
                }
                fileContent = jsonModuleSource;
            }
            if (directParse)
            {
                unsigned int contentLength = (fileContent == nullptr) ? 0 : (unsigned int)strlen(fileContent);
                ParseModuleSourceOnRecord(moduleRecord, fileContent, contentLength);
                free((void*)fileContent);
            }
            else
            {
                LoadScript(nullptr, filePath == nullptr ? *specifierStr : filePath->c_str(), fileContent, "module", true, WScriptJsrt::FinalizeFree, true);
            }
        }
    }
Error:
    return errorCode;
}

JsErrorCode WScriptJsrt::ReportModuleCompletionCallback(JsModuleRecord module, JsValueRef exception)
{
    if (exception != nullptr)
    {
        JsValueRef specifier = JS_INVALID_REFERENCE;
        ChakraRTInterface::JsGetModuleHostInfo(module, JsModuleHostInfo_Url, &specifier);
        ReportNegativePhaseMarker(false);
        PrintException(AutoString(specifier).GetString(), JsErrorCode::JsErrorScriptException, exception);
    }
    return JsNoError;
}

static JsErrorCode ReadModuleAttributes(JsValueRef attributes, bool* hasAttributes, std::string* canonicalOut, std::string* typeOut)
{
    *hasAttributes = false;
    canonicalOut->clear();
    typeOut->clear();

    JsValueType valueType = JsUndefined;
    JsErrorCode valueTypeError = ChakraRTInterface::JsGetValueType(attributes, &valueType);
    IfJsrtErrorFail(valueTypeError, valueTypeError);
    if (valueType == JsUndefined)
    {
        return JsNoError;
    }
    *hasAttributes = true;

    JsValueRef propertyNames = JS_INVALID_REFERENCE;
    IfJsrtErrorFail(ChakraRTInterface::JsGetOwnPropertyNames(attributes, &propertyNames), JsErrorInvalidArgument);
    JsPropertyIdRef lengthId = nullptr;
    IfJsrtErrorFail(ChakraRTInterface::JsCreatePropertyId("length", 6, &lengthId), JsErrorInvalidArgument);
    JsValueRef lengthValue = JS_INVALID_REFERENCE;
    IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(propertyNames, lengthId, &lengthValue), JsErrorInvalidArgument);
    int length = 0;
    IfJsrtErrorFail(ChakraRTInterface::JsNumberToInt(lengthValue, &length), JsErrorInvalidArgument);

    auto escape = [](const char* text, std::string* out) {
        for (const char* p = text; *p != '\0'; p++)
        {
            if (*p == '\\' || *p == '\n' || *p == '\x1e' || *p == '=')
            {
                out->push_back('\\');
            }
            out->push_back(*p);
        }
    };

    for (int i = 0; i < length; i++)
    {
        JsValueRef indexValue = JS_INVALID_REFERENCE;
        IfJsrtErrorFail(ChakraRTInterface::JsIntToNumber(i, &indexValue), JsErrorInvalidArgument);
        JsValueRef keyValue = JS_INVALID_REFERENCE;
        IfJsrtErrorFail(ChakraRTInterface::JsGetIndexedProperty(propertyNames, indexValue, &keyValue), JsErrorInvalidArgument);
        AutoString keyString;
        if (keyString.Initialize(keyValue) != JsNoError)
        {
            return keyString.GetError();
        }
        JsPropertyIdRef keyId = nullptr;
        IfJsrtErrorFail(ChakraRTInterface::JsCreatePropertyId(keyString.GetString(), strlen(keyString.GetString()), &keyId), JsErrorInvalidArgument);
        JsValueRef value = JS_INVALID_REFERENCE;
        IfJsrtErrorFail(ChakraRTInterface::JsGetProperty(attributes, keyId, &value), JsErrorInvalidArgument);
        AutoString valueString;
        if (valueString.Initialize(value) != JsNoError)
        {
            return valueString.GetError();
        }
        if (i > 0)
        {
            canonicalOut->push_back('\x1e');
        }
        escape(keyString.GetString(), canonicalOut);
        canonicalOut->push_back('=');
        escape(valueString.GetString(), canonicalOut);
        if (strcmp(keyString.GetString(), "type") == 0 && typeOut->empty())
        {
            *typeOut = valueString.GetString();
        }
    }
    return JsNoError;
}

static char* WrapJsonModuleSource(LPCSTR jsonText)
{
    return BuildJsonParseSource(jsonText, "export default JSON.parse('", "');");
}

static bool ValidateJsonModuleSource(JsModuleRecord moduleRecord, LPCSTR jsonParseSource)
{
    JsValueRef sourceRef = nullptr;
    JsValueRef sourceName = nullptr;
    if (ChakraRTInterface::JsCreateString(jsonParseSource, strlen(jsonParseSource), &sourceRef) != JsNoError ||
        ChakraRTInterface::JsCreateString("json-module-parse", strlen("json-module-parse"), &sourceName) != JsNoError)
    {
        return true;
    }
    if (ChakraRTInterface::JsRun(sourceRef, JS_SOURCE_CONTEXT_NONE, sourceName,
        JsParseScriptAttributeNone, nullptr) == JsNoError)
    {
        return true;
    }
    JsValueRef exception = nullptr;
    if (ChakraRTInterface::JsGetAndClearException(&exception) != JsNoError || exception == nullptr)
    {
        return true;
    }
    ChakraRTInterface::JsSetModuleHostInfo(moduleRecord, JsModuleHostInfo_Exception, exception);
    return false;
}

static char* BuildJsonParseSource(LPCSTR jsonText, const char* prefix, const char* suffix)
{
    static const char lineSeparatorEscape[] = "\\u2028";
    static const char paragraphSeparatorEscape[] = "\\u2029";
    size_t length = strlen(jsonText);
    size_t capacity = strlen(prefix) + 2 * length + strlen(suffix) + 1;
    char* out = (char*)malloc(capacity);
    if (out == nullptr)
    {
        return nullptr;
    }
    char* cursor = out;
    memcpy(cursor, prefix, strlen(prefix));
    cursor += strlen(prefix);
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)jsonText[i];
        if (c == '\\' || c == '\'')
        {
            *cursor++ = '\\';
            *cursor++ = (char)c;
        }
        else if (c == '\n')
        {
            *cursor++ = '\\';
            *cursor++ = 'n';
        }
        else if (c == '\r')
        {
            *cursor++ = '\\';
            *cursor++ = 'r';
        }
        else if (c == 0xE2 && i + 2 < length &&
            (unsigned char)jsonText[i + 1] == 0x80 &&
            ((unsigned char)jsonText[i + 2] == 0xA8 || (unsigned char)jsonText[i + 2] == 0xA9))
        {
            const char* escape = ((unsigned char)jsonText[i + 2] == 0xA8) ? lineSeparatorEscape : paragraphSeparatorEscape;
            memcpy(cursor, escape, 6);
            cursor += 6;
            i += 2;
        }
        else
        {
            *cursor++ = (char)c;
        }
    }
    memcpy(cursor, suffix, strlen(suffix));
    cursor += strlen(suffix);
    *cursor = '\0';
    return out;
}

JsErrorCode WScriptJsrt::FetchImportedModuleHelperWithAttributes(JsModuleRecord referencingModule,
    JsValueRef specifier, JsValueRef attributes, _Out_ JsModuleRecord* dependentModuleRecord, LPCSTR refdir)
{
    JsModuleRecord moduleRecord = JS_INVALID_REFERENCE;
    AutoString specifierStr;
    *dependentModuleRecord = nullptr;

    bool isDynamicImport = (referencingModule == nullptr || t_moduleParseDepth == 0);
    if (!isDynamicImport)
    {
        HostGlobalsLock lock;
        auto it = moduleErrMap.find(referencingModule);
        if (it != moduleErrMap.end() && IsDynamicModuleState(it->second))
        {
            isDynamicImport = true;
        }
    }

    auto noteStaticGraphFailure = [isDynamicImport]() {
        if (!isDynamicImport)
        {
            NoteModuleGraphFailure();
        }
    };

    if (specifierStr.Initialize(specifier) != JsNoError)
    {
        noteStaticGraphFailure();
        return specifierStr.GetError();
    }

    bool hasAttributes = false;
    std::string canonicalAttributes;
    std::string attributeType;
    JsErrorCode attributesError = ReadModuleAttributes(attributes, &hasAttributes, &canonicalAttributes, &attributeType);
    if (attributesError != JsNoError)
    {
        noteStaticGraphFailure();
        return attributesError;
    }

    char fullPath[_MAX_PATH];
    std::string specifierFullPath = (refdir != nullptr && !IsAbsolutePath(*specifierStr)) ? refdir : "";
    specifierFullPath += *specifierStr;
    if (_fullpath(fullPath, specifierFullPath.c_str(), _MAX_PATH) == nullptr)
    {
        noteStaticGraphFailure();
        return JsErrorInvalidArgument;
    }

    std::string moduleKey(fullPath);
    if (hasAttributes)
    {
        moduleKey += '\x1e';
        moduleKey += canonicalAttributes;
    }

    {
        HostGlobalsLock lock;
        auto moduleEntry = moduleRecordMap.find(moduleKey);
        if (moduleEntry != moduleRecordMap.end())
        {
            *dependentModuleRecord = moduleEntry->second;
            return JsNoError;
        }

        bool isJsonModule = (attributeType == "json");
        if (hasAttributes && !attributeType.empty() && !isJsonModule)
        {
            noteStaticGraphFailure();
            return JsErrorInvalidArgument;
        }

        JsErrorCode errorCode = ChakraRTInterface::JsInitializeModuleRecord(referencingModule, specifier, &moduleRecord);
        if (errorCode == JsNoError)
        {
            GetDir(fullPath, &moduleDirMap[moduleRecord]);
            std::string pathKey = moduleKey;
            moduleRecordMap[pathKey] = moduleRecord;
            moduleErrMap[moduleRecord] = isDynamicImport ? DynamicImportModule : ImportedModule;
        }
        else
        {
            noteStaticGraphFailure();
            return errorCode;
        }
    }

    bool isJsonModule = (attributeType == "json");
    std::string pathKey = moduleKey;
    std::string filePathKey(fullPath);
    ModuleMessage* moduleMessage = WScriptJsrt::ModuleMessage::CreateWithAttributes(moduleRecord, specifier, &filePathKey, &pathKey, isJsonModule);
    if (moduleMessage == nullptr || !moduleMessage->HasValidRefs())
    {
        delete moduleMessage;
        noteStaticGraphFailure();
        return JsErrorOutOfMemory;
    }
    WScriptJsrt::PushMessage(moduleMessage);
    *dependentModuleRecord = moduleRecord;
    return JsNoError;
}

JsErrorCode WScriptJsrt::FetchImportedModuleHelper(JsModuleRecord referencingModule,
    JsValueRef specifier, _Out_ JsModuleRecord* dependentModuleRecord, LPCSTR refdir)
{
    JsModuleRecord moduleRecord = JS_INVALID_REFERENCE;
    AutoString specifierStr;
    *dependentModuleRecord = nullptr;

    bool isDynamicImport = (referencingModule == nullptr || t_moduleParseDepth == 0);
    if (!isDynamicImport)
    {
        HostGlobalsLock lock;
        auto it = moduleErrMap.find(referencingModule);
        if (it != moduleErrMap.end() && IsDynamicModuleState(it->second))
        {
            isDynamicImport = true;
        }
    }

    auto noteStaticGraphFailure = [isDynamicImport]() {
        if (!isDynamicImport)
        {
            NoteModuleGraphFailure();
        }
    };

    if (specifierStr.Initialize(specifier) != JsNoError)
    {
        noteStaticGraphFailure();
        return specifierStr.GetError();
    }

    char fullPath[_MAX_PATH];
    std::string specifierFullPath = (refdir != nullptr && !IsAbsolutePath(*specifierStr)) ? refdir : "";
    specifierFullPath += *specifierStr;
    if (_fullpath(fullPath, specifierFullPath.c_str(), _MAX_PATH) == nullptr)
    {
        noteStaticGraphFailure();
        return JsErrorInvalidArgument;
    }

    {
        HostGlobalsLock lock;
        auto moduleEntry = moduleRecordMap.find(std::string(fullPath));
        if (moduleEntry != moduleRecordMap.end())
        {
            *dependentModuleRecord = moduleEntry->second;
            return JsNoError;
        }

        JsErrorCode errorCode = ChakraRTInterface::JsInitializeModuleRecord(referencingModule, specifier, &moduleRecord);
        if (errorCode != JsNoError)
        {
            noteStaticGraphFailure();
            return errorCode;
        }
        GetDir(fullPath, &moduleDirMap[moduleRecord]);
        std::string pathKey = std::string(fullPath);
        moduleRecordMap[pathKey] = moduleRecord;
        moduleErrMap[moduleRecord] = isDynamicImport ? DynamicImportModule : ImportedModule;
    }

    std::string pathKey = std::string(fullPath);
    ModuleMessage* moduleMessage = WScriptJsrt::ModuleMessage::Create(referencingModule, specifier, &pathKey);
    if (moduleMessage == nullptr || !moduleMessage->HasValidRefs())
    {
        delete moduleMessage;
        noteStaticGraphFailure();
        return JsErrorOutOfMemory;
    }
    WScriptJsrt::PushMessage(moduleMessage);
    *dependentModuleRecord = moduleRecord;
    return JsNoError;
}

JsErrorCode WScriptJsrt::FetchImportedModule(_In_ JsModuleRecord referencingModule,
    _In_ JsValueRef specifier, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord)
{
    std::string dir;
    bool hasDir = false;
    {
        HostGlobalsLock lock;
        auto moduleDirEntry = moduleDirMap.find(referencingModule);
        if (moduleDirEntry != moduleDirMap.end())
        {
            dir = moduleDirEntry->second;
            hasDir = true;
        }
    }
    if (hasDir)
    {
        return FetchImportedModuleHelper(referencingModule, specifier, dependentModuleRecord, dir.c_str());
    }

    return FetchImportedModuleHelper(referencingModule, specifier, dependentModuleRecord);
}

JsErrorCode WScriptJsrt::FetchImportedModuleFromScript(_In_ JsSourceContext dwReferencingSourceContext,
    _In_ JsValueRef specifier, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord)
{
    std::string dir;
    bool hasDir = false;
    {
        HostGlobalsLock lock;
        auto scriptDirEntry = scriptDirMap.find(static_cast<DWORD_PTR>(dwReferencingSourceContext));
        if (scriptDirEntry != scriptDirMap.end())
        {
            dir = scriptDirEntry->second;
            hasDir = true;
        }
    }
    if (hasDir)
    {
        return FetchImportedModuleHelper(nullptr, specifier, dependentModuleRecord, dir.c_str());
    }

    return FetchImportedModuleHelper(nullptr, specifier, dependentModuleRecord);
}

JsErrorCode WScriptJsrt::FetchImportedModuleWithAttributes(_In_ JsModuleRecord referencingModule,
    _In_ JsValueRef specifier, _In_ JsValueRef attributes, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord)
{
    std::string dir;
    bool hasDir = false;
    {
        HostGlobalsLock lock;
        auto moduleDirEntry = moduleDirMap.find(referencingModule);
        if (moduleDirEntry != moduleDirMap.end())
        {
            dir = moduleDirEntry->second;
            hasDir = true;
        }
    }
    if (hasDir)
    {
        return FetchImportedModuleHelperWithAttributes(referencingModule, specifier, attributes, dependentModuleRecord, dir.c_str());
    }

    return FetchImportedModuleHelperWithAttributes(referencingModule, specifier, attributes, dependentModuleRecord);
}

JsErrorCode WScriptJsrt::FetchImportedModuleFromScriptWithAttributes(_In_ JsSourceContext dwReferencingSourceContext,
    _In_ JsValueRef specifier, _In_ JsValueRef attributes, _Outptr_result_maybenull_ JsModuleRecord* dependentModuleRecord)
{
    std::string dir;
    bool hasDir = false;
    {
        HostGlobalsLock lock;
        auto scriptDirEntry = scriptDirMap.find(static_cast<DWORD_PTR>(dwReferencingSourceContext));
        if (scriptDirEntry != scriptDirMap.end())
        {
            dir = scriptDirEntry->second;
            hasDir = true;
        }
    }
    if (hasDir)
    {
        return FetchImportedModuleHelperWithAttributes(nullptr, specifier, attributes, dependentModuleRecord, dir.c_str());
    }

    return FetchImportedModuleHelperWithAttributes(nullptr, specifier, attributes, dependentModuleRecord);
}

JsErrorCode WScriptJsrt::NotifyModuleReadyCallback(_In_opt_ JsModuleRecord referencingModule, _In_opt_ JsValueRef exceptionVar)
{
    if (exceptionVar != nullptr)
    {
        bool isDynamicImport = false;
        {
            HostGlobalsLock lock;
            auto it = moduleErrMap.find(referencingModule);
            if (it != moduleErrMap.end() && it->second == DynamicImportModule)
            {
                isDynamicImport = true;
            }
        }
        if (!isDynamicImport)
        {
            NoteModuleGraphFailure();
        }

        if (HostConfigFlags::flags.TraceHostCallbackIsEnabled)
        {
            JsValueRef specifier = JS_INVALID_REFERENCE;
            ChakraRTInterface::JsGetModuleHostInfo(referencingModule, JsModuleHostInfo_Url, &specifier);
            AutoString fileName;
            if (specifier != JS_INVALID_REFERENCE)
            {
                fileName.Initialize(specifier);
            }
            wprintf(_u("NotifyModuleReadyCallback(exception) %S\n"), fileName.GetString());
        }
    }

    bool errored;
    {
        HostGlobalsLock lock;
        errored = (moduleErrMap[referencingModule] == ErroredModule);
    }
    if (!errored)
    {
        WScriptJsrt::ModuleMessage* moduleMessage =
            WScriptJsrt::ModuleMessage::Create(referencingModule, nullptr);
        if (moduleMessage == nullptr || !moduleMessage->HasValidRefs())
        {
            delete moduleMessage;
            return JsErrorOutOfMemory;
        }
        WScriptJsrt::PushMessage(moduleMessage);
    }
    return JsNoError;
}

JsErrorCode __stdcall WScriptJsrt::InitializeImportMetaCallback(_In_opt_ JsModuleRecord referencingModule, _In_opt_ JsValueRef importMetaVar)
{
    if (importMetaVar != nullptr)
    {
        JsValueRef specifier = JS_INVALID_REFERENCE;
        ChakraRTInterface::JsGetModuleHostInfo(referencingModule, JsModuleHostInfo_Url, &specifier);

        JsPropertyIdRef urlPropId;
        if (JsNoError == CreatePropertyIdFromString("url", &urlPropId))
        {
            ChakraRTInterface::JsSetProperty(importMetaVar, urlPropId, specifier, false);
        }
    }

    return JsNoError;
}
