//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLanguagePch.h"
#include "Types/PropertyIndexRanges.h"
#include "Types/SimpleDictionaryPropertyDescriptor.h"
#include "Types/SimpleDictionaryTypeHandler.h"
#include "ModuleNamespace.h"
#include "Library/JavascriptPromise.h"

namespace Js
{
    const uint32 ModuleRecordBase::ModuleMagicNumber = *(const uint32*)"Mode";

    SourceTextModuleRecord::SourceTextModuleRecord(ScriptContext* scriptContext) :
        ModuleRecordBase(scriptContext->GetLibrary()),
        scriptContext(scriptContext),
        parseTree(nullptr),
        parser(nullptr),
        pSourceInfo(nullptr),
        rootFunction(nullptr),
        requestedModuleList(nullptr),
        importRecordList(nullptr),
        localExportRecordList(nullptr),
        indirectExportRecordList(nullptr),
        starExportRecordList(nullptr),
        childrenModuleSet(nullptr),
        parentModuleList(nullptr),
        localExportMapByExportName(nullptr),
        localExportMapByLocalName(nullptr),
        localExportIndexList(nullptr),
        normalizedSpecifier(nullptr),
        errorObject(nullptr),
        hostDefined(nullptr),
        exportedNames(nullptr),
        resolvedExportMap(nullptr),
        localExportSlots(nullptr),
        moduleId(InvalidModuleIndex),
        localSlotCount(InvalidSlotCount),
        promise(nullptr),
        importMetaObject(nullptr),
        localExportCount(0)
    {
        namespaceRecord.module = this;
        namespaceRecord.bindingName = PropertyIds::star_;
    }

    SourceTextModuleRecord* SourceTextModuleRecord::Create(ScriptContext* scriptContext)
    {
        Recycler* recycler = scriptContext->GetRecycler();
        SourceTextModuleRecord* childModuleRecord;
        childModuleRecord = RecyclerNewFinalized(recycler, Js::SourceTextModuleRecord, scriptContext);
        // There is no real reference to lifetime management in ecmascript
        // The life time of a module record should be controlled by the module registry as defined in WHATWG module loader spec
        // in practice the modulerecord lifetime should be the same as the scriptcontext so it could be retrieved for the same
        // site. Host might hold a reference to the module as well after initializing the module.
        // In our implementation, we'll use the moduleId in bytecode to identify the module.
        childModuleRecord->moduleId = scriptContext->GetLibrary()->EnsureModuleRecordList()->Add(childModuleRecord);

        return childModuleRecord;
    }

    void SourceTextModuleRecord::Finalize(bool isShutdown)
    {
        parseTree = nullptr;
        requestedModuleList = nullptr;
        importRecordList = nullptr;
        localExportRecordList = nullptr;
        indirectExportRecordList = nullptr;
        starExportRecordList = nullptr;
        parentModuleList = nullptr;
        if (!isShutdown)
        {
            AdeleteUnlessNull(scriptContext->GeneralAllocator(), parser);
            AdeleteUnlessNull(scriptContext->GeneralAllocator(), childrenModuleSet);
        }
    }

    HRESULT SourceTextModuleRecord::ParseSource(__in_bcount(sourceLength) byte* sourceText, uint32 sourceLength, SRCINFO * srcInfo, Var* exceptionVar, bool isUtf8)
    {
        Assert(!wasParsed || sourceText == nullptr);
        Assert(parser == nullptr);
        Assert(exceptionVar != nullptr);
        HRESULT hr = NOERROR;

        // Return if loading failure has been reported
        if (sourceText == nullptr && wasParsed)
        {
            return hr;
        }

        ScriptContext* scriptContext = GetScriptContext();
        CompileScriptException se;
        ArenaAllocator* allocator = scriptContext->GeneralAllocator();
        *exceptionVar = nullptr;
        if (!scriptContext->GetConfig()->IsES6ModuleEnabled())
        {
            JavascriptError *pError = scriptContext->GetLibrary()->CreateError();
            JavascriptError::SetErrorMessageProperties(pError, hr, _u("ES6Module not supported"), scriptContext);
            *exceptionVar = pError;
            return E_NOTIMPL;
        }

        // Mark module as root module if it currently has no parents
        // Note, if there are circular imports it may gain parents later
        if (parentModuleList == nullptr && promise == nullptr)
        {
            SetIsRootModule();
        }

        // Host indicates that the current module failed to load.
        if (sourceText == nullptr)
        {
            Assert(sourceLength == 0);
            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("Failed to load: %s\n"), this->GetSpecifierSz());
            hr = E_FAIL;

            // We cannot just use the buffer in the specifier string - need to make a copy here.
            const char16* moduleName = this->GetSpecifierSz();
            size_t length = wcslen(moduleName);
            char16* allocatedString = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, length + 1);
            wmemcpy_s(allocatedString, length + 1, moduleName, length);
            allocatedString[length] = _u('\0');

            JavascriptError *pError = scriptContext->GetLibrary()->CreateURIError();
            JavascriptError::SetErrorMessageProperties(pError, hr, allocatedString, scriptContext);
            *exceptionVar = pError;
        }
        else
        {
            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("ParseSource(%s)\n"), this->GetSpecifierSz());
            try
            {
                AUTO_NESTED_HANDLED_EXCEPTION_TYPE((ExceptionType)(ExceptionType_OutOfMemory | ExceptionType_StackOverflow));
                this->parser = Anew(allocator, Parser, scriptContext);
                srcInfo->moduleID = moduleId;

                LoadScriptFlag loadScriptFlag = (LoadScriptFlag)(LoadScriptFlag_Expression | LoadScriptFlag_Module |
                    LoadScriptFlag_disableAsmJs | (isUtf8 ? LoadScriptFlag_Utf8Source : LoadScriptFlag_None));

                Utf8SourceInfo* pResultSourceInfo = nullptr;
                this->parseTree = scriptContext->ParseScript(parser, sourceText,
                    sourceLength, srcInfo, &se, &pResultSourceInfo, Constants::ModuleCode,
                    loadScriptFlag, &sourceIndex, nullptr);
                this->pSourceInfo = pResultSourceInfo;
            }
            catch (Js::OutOfMemoryException)
            {
                hr = E_OUTOFMEMORY;
            }
            catch (Js::StackOverflowException)
            {
                hr = VBSERR_OutOfStack;
            }

            if (FAILED(hr))
            {
                se.ProcessError(nullptr, hr, nullptr);
            }
            else if (parseTree == nullptr)
            {
                hr = E_FAIL;
            }
            else
            {
                hr = PostParseProcess();
                if (hr == S_OK && this->errorObject != nullptr && this->hadNotifyHostReady)
                {
                    // This would be the case where the child module got error and current module has notified error already.
                    if (*exceptionVar == nullptr)
                    {
                        *exceptionVar = this->errorObject;
                    }

                    // Cleanup in case of error.
                    this->ReleaseParserResourcesForHierarchy();
                    return E_FAIL;
                }
            }
        }

        if (FAILED(hr))
        {
            if (*exceptionVar == nullptr)
            {
                if (this->errorObject != nullptr)
                {
                    // A failed dependency fetch (e.g. import attributes the host cannot serve) records the load error on
                    // the module; surface it instead of synthesizing one from an empty compile error.
                    *exceptionVar = this->errorObject;
                }
                else
                {
                    const WCHAR * sourceUrl = this->GetSpecifierSz();

                    *exceptionVar = JavascriptError::CreateFromCompileScriptException(scriptContext, &se, sourceUrl);
                }
            }
            // Cleanup in case of error.
            this->ReleaseParserResourcesForHierarchy();
            if (this->parser)
            {
                this->parseTree = nullptr;
                Adelete(allocator, this->parser);
                this->parser = nullptr;
            }
            if (this->errorObject == nullptr)
            {
                this->errorObject = *exceptionVar;
            }

            if (this->promise != nullptr)
            {
                SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(false, this->errorObject, this->scriptContext, this, false);
            }

            // Notify host if current module is root module and the host hasn't been notified
            if (isRootModule && !hadNotifyHostReady)
            {
                OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyHostAboutModuleReady %s (ParseSource error)\n"), this->GetSpecifierSz());
                LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
                {
                    scriptContext->GetHostScriptContext()->NotifyHostAboutModuleReady(this, this->errorObject);
                });

                hadNotifyHostReady = true;
            }

            // Notify parent if applicable
            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyParentAsNeeded\n"));
            NotifyParentsAsNeeded();
        }
        return hr;
    }

    void SourceTextModuleRecord::NotifyParentsAsNeeded()
    {
        if (notifying)
        {
            return;
        }
        notifying = true;
        // Notify the parent modules that this child module is either in fault state or finished.
        if (this->parentModuleList != nullptr)
        {
            parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
            {
                parentModule->OnChildModuleReady(this, this->errorObject);
            });
        }
        notifying = false;
        SetParentsNotified();
    }

    void SourceTextModuleRecord::ImportModuleListsFromParser()
    {
        Assert(scriptContext->GetConfig()->IsES6ModuleEnabled());
        ParseNodeModule* moduleParseNode = this->parseTree->AsParseNodeModule();
        SetRequestedModuleList(moduleParseNode->requestedModules);
        SetImportRecordList(moduleParseNode->importEntries);
        SetStarExportRecordList(moduleParseNode->starExportEntries);
        SetIndirectExportRecordList(moduleParseNode->indirectExportEntries);
        SetLocalExportRecordList(moduleParseNode->localExportEntries);
    }

    void SourceTextModuleRecord::ReleaseParserResources()
    {
        if (this->parser != nullptr)
        {
            this->parser->ReleaseTemporaryGuestArena();
        }
    }

    void SourceTextModuleRecord::ReleaseParserResourcesForHierarchy()
    {
        this->ReleaseParserResources();

        if (this->childrenModuleSet != nullptr)
        {
            this->childrenModuleSet->EachValue([=](SourceTextModuleRecord* childModuleRecord)
            {
                childModuleRecord->ReleaseParserResources();
            });
        }
    }

    HRESULT SourceTextModuleRecord::PostParseProcess()
    {
        HRESULT hr = NOERROR;
        SetWasParsed();
        ImportModuleListsFromParser();
        hr = ResolveExternalModuleDependencies();

        if (SUCCEEDED(hr))
        {
            hr = PrepareForModuleDeclarationInitialization();
        }
        else
        {
            // Cleanup in case of error.
            this->ReleaseParserResourcesForHierarchy();
        }

        return hr;
    }

    Var SourceTextModuleRecord::PostProcessDynamicModuleImport()
    {
        JavascriptPromise *promise = this->GetPromise();
        ScriptContext* scriptContext = GetScriptContext();
        AnalysisAssert(scriptContext != nullptr);
        if (promise == nullptr)
        {
            promise = JavascriptPromise::CreateEnginePromise(scriptContext);
            this->SetPromise(promise);
        }

        if (this->ParentsNotified())
        {
            HRESULT hr = NOERROR;
            if (!WasDeclarationInitialized())
            {
                if (ModuleDeclarationInstantiation())
                {
                    GenerateRootFunction();
                }

                if (this->errorObject != nullptr)
                {
                    // Cleanup in case of error.
                    this->ReleaseParserResourcesForHierarchy();
                    SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(false, this->errorObject, scriptContext, this, false);
                }
                else
                {
                    if (!hadNotifyHostReady && !WasEvaluated())
                    {
                        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyHostAboutModuleReady %s (PostProcessDynamicModuleImport)\n"), this->GetSpecifierSz());
                        LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
                        {
                            hr = scriptContext->GetHostScriptContext()->NotifyHostAboutModuleReady(this, this->errorObject);
                        });

                        hadNotifyHostReady = true;
                    }
                }
            }

            if (FAILED(hr))
            {
                // Cleanup in case of error.
                this->ReleaseParserResourcesForHierarchy();

                // We cannot just use the buffer in the specifier string - need to make a copy here.
                const char16* moduleName = this->GetSpecifierSz();
                size_t length = wcslen(moduleName);
                char16* allocatedString = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, length + 1);
                wmemcpy_s(allocatedString, length + 1, moduleName, length);
                allocatedString[length] = _u('\0');

                Js::JavascriptError * error = scriptContext->GetLibrary()->CreateURIError();
                JavascriptError::SetErrorMessageProperties(error, hr, allocatedString, scriptContext);
                return SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(false, error, scriptContext, this);
            }
        }

        return JavascriptPromise::CreatePassThroughPromise(this->promise, scriptContext);
    }

    HRESULT SourceTextModuleRecord::PrepareForModuleDeclarationInitialization()
    {
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("PrepareForModuleDeclarationInitialization(%s)\n"), this->GetSpecifierSz());
        HRESULT hr = NO_ERROR;

        if (ConfirmChildrenParsed())
        {
            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyParentsAsNeeded\n"));
            NotifyParentsAsNeeded();

            if (!WasDeclarationInitialized() && (isRootModule || promise != nullptr))
            {
                // TODO: move this as a promise call? if parser is called from a different thread
                // We'll need to call the bytecode gen in the main thread as we are accessing GC.
                ScriptContext* scriptContext = GetScriptContext();
                Assert(!scriptContext->GetThreadContext()->IsScriptActive() || this->promise != nullptr);

                if (ModuleDeclarationInstantiation())
                {
                    GenerateRootFunction();
                }

                if (this->errorObject != nullptr)
                {
                    // Cleanup in case of error.
                    this->ReleaseParserResourcesForHierarchy();
                }

                if (!hadNotifyHostReady && !WasEvaluated())
                {
                    OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyHostAboutModuleReady %s (PrepareForModuleDeclarationInitialization)\n"), this->GetSpecifierSz());
                    LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
                    {
                        hr = scriptContext->GetHostScriptContext()->NotifyHostAboutModuleReady(this, this->errorObject);
                    });

                    hadNotifyHostReady = true;
                }
            }
        }
        return hr;
    }

    HRESULT SourceTextModuleRecord::OnChildModuleReady(SourceTextModuleRecord* childModule, Var childException)
    {
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("OnChildModuleReady(%s)\n"), this->GetSpecifierSz(), childModule->GetSpecifierSz());
        HRESULT hr = NOERROR;
        if (childException != nullptr)
        {
            // propagate the error up as needed.
            if (this->errorObject == nullptr)
            {
                this->errorObject = childException;
            }

            // Cleanup in case of error.
            this->ReleaseParserResourcesForHierarchy();

            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyParentAsNeeded (childException)\n"), this->GetSpecifierSz());
            NotifyParentsAsNeeded();

            if (this->promise != nullptr)
            {
                SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(false, this->errorObject, this->scriptContext, this, false);
            }

            if (isRootModule && !hadNotifyHostReady)
            {
                OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyHostAboutModuleReady %s (OnChildModuleReady)\n"), this->GetSpecifierSz());
                LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
                {
                    hr = scriptContext->GetHostScriptContext()->NotifyHostAboutModuleReady(this, this->errorObject);
                });

                hadNotifyHostReady = true;
            }
        }
        else
        {
            hr = PrepareForModuleDeclarationInitialization();
        }
        return hr;
    }

    ModuleNamespace* SourceTextModuleRecord::GetNamespace()
    {
        Assert(localExportSlots != nullptr);
        Assert(PointerValue(localExportSlots[GetLocalExportSlotCount()]) == __super::GetNamespace());
        return (ModuleNamespace*)(void*)(localExportSlots[GetLocalExportSlotCount()]);
    }

    void SourceTextModuleRecord::SetNamespace(ModuleNamespace* moduleNamespace)
    {
        Assert(localExportSlots != nullptr);
        __super::SetNamespace(moduleNamespace);
        localExportSlots[GetLocalExportSlotCount()] = moduleNamespace;
    }


    ExportedNames* SourceTextModuleRecord::GetExportedNames(ExportModuleRecordList* exportStarSet)
    {
        const bool isRootGetExportedNames = (exportStarSet == nullptr);

        // this->exportedNames only caches root "GetExportedNames(nullptr)"
        if (isRootGetExportedNames && exportedNames != nullptr)
        {
            return exportedNames;
        }

        ArenaAllocator* allocator = scriptContext->GeneralAllocator();

        if (exportStarSet == nullptr)
        {
            exportStarSet = Anew(allocator, ExportModuleRecordList, allocator);
        }
        if (exportStarSet->Has(this))
        {
            return nullptr;
        }
        exportStarSet->Prepend(this);

        ExportedNames* tempExportedNames = nullptr;
        if (this->localExportRecordList != nullptr)
        {
            tempExportedNames = Anew(allocator, ExportedNames, allocator);
            this->localExportRecordList->Map([=](ModuleImportOrExportEntry exportEntry) {
                PropertyId exportNameId = EnsurePropertyIdForIdentifier(exportEntry.exportName);
                tempExportedNames->Prepend(exportNameId);
            });
        }
        if (this->indirectExportRecordList != nullptr)
        {
            if (tempExportedNames == nullptr)
            {
                tempExportedNames = Anew(allocator, ExportedNames, allocator);
            }
            this->indirectExportRecordList->Map([=](ModuleImportOrExportEntry exportEntry) {
                PropertyId exportedNameId = EnsurePropertyIdForIdentifier(exportEntry.exportName);
                tempExportedNames->Prepend(exportedNameId);
            });
        }
        if (this->starExportRecordList != nullptr)
        {
            if (tempExportedNames == nullptr)
            {
                tempExportedNames = Anew(allocator, ExportedNames, allocator);
            }
            this->starExportRecordList->Map([=](ModuleImportOrExportEntry exportEntry) {
                Assert(exportEntry.moduleRequest != nullptr);
                SourceTextModuleRecord* moduleRecord = nullptr;
                LPCOLESTR moduleRequestKey = SourceTextModuleRecord::GetModuleRequestKey(allocator, exportEntry.moduleRequest, exportEntry.attributes);
                if (this->childrenModuleSet->TryGetValue(moduleRequestKey, &moduleRecord))
                {
                    Assert(moduleRecord->WasParsed());
                    Assert(moduleRecord->WasDeclarationInitialized()); // we should be half way during initialization
                    Assert(!moduleRecord->WasEvaluated());
                    ExportedNames* starExportedNames = moduleRecord->GetExportedNames(exportStarSet);
                    // We are not rejecting ambiguous resolution at this time.
                    if (starExportedNames != nullptr)
                    {
                        starExportedNames->Map([&](PropertyId propertyId) {
                            if (propertyId != PropertyIds::default_ && !tempExportedNames->Has(propertyId))
                            {
                                tempExportedNames->Prepend(propertyId);
                            }
                        });
                    }
                }
#if DBG
                else
                {
                    AssertMsg(false, "dependent modules should have been initialized");
                }
#endif
            });
        }

        // this->exportedNames only caches root "GetExportedNames(nullptr)"
        if (isRootGetExportedNames)
        {
            exportedNames = tempExportedNames;
        }

        return tempExportedNames;
    }

    bool SourceTextModuleRecord::ResolveImport(PropertyId localName, ModuleNameRecord** importRecord)
    {
        *importRecord = nullptr;

        importRecordList->MapUntil([&](ModuleImportOrExportEntry& importEntry) {
            Js::PropertyId localNamePid = EnsurePropertyIdForIdentifier(importEntry.localName);
            if (localNamePid == localName)
            {
                SourceTextModuleRecord* childModule = this->GetChildModuleRecord(importEntry.moduleRequest, importEntry.attributes);
                Js::PropertyId importName = EnsurePropertyIdForIdentifier(importEntry.importName);
                if (importName == Js::PropertyIds::star_)
                {
                    *importRecord = childModule->GetNamespaceNameRecord();
                }
                else
                {
                    childModule->ResolveExport(importName, nullptr, importRecord);
                }
                return true;
            }
            return false;
        });

        return *importRecord != nullptr;
    }

    // return false when "ambiguous".
    // otherwise nullptr means "null" where we have circular reference/cannot resolve.
    bool SourceTextModuleRecord::ResolveExport(PropertyId exportName, ResolveSet* resolveSet, ModuleNameRecord** exportRecord)
    {
        ArenaAllocator* allocator = scriptContext->GeneralAllocator();
        if (resolvedExportMap == nullptr)
        {
            resolvedExportMap = Anew(allocator, ResolvedExportMap, allocator);
        }
        if (resolvedExportMap->TryGetReference(exportName, exportRecord))
        {
            return true;
        }
        // TODO: use per-call/loop allocator?
        if (resolveSet == nullptr)
        {
            resolveSet = Anew(allocator, ResolveSet, allocator);
        }

        *exportRecord = nullptr;
        bool hasCircularRef = false;
        resolveSet->MapUntil([&](ModuleNameRecord moduleNameRecord) {
            if (moduleNameRecord.module == this && moduleNameRecord.bindingName == exportName)
            {
                *exportRecord = nullptr;
                hasCircularRef = true;
                return true;
            }
            return false;
        });
        if (hasCircularRef)
        {
            Assert(*exportRecord == nullptr);
            return true;
        }
        resolveSet->Prepend(ModuleNameRecord(this, exportName));

        if (localExportRecordList != nullptr)
        {
            PropertyId localNameId = Js::Constants::NoProperty;
            localExportRecordList->MapUntil([&](ModuleImportOrExportEntry exportEntry) {
                PropertyId exportNameId = EnsurePropertyIdForIdentifier(exportEntry.exportName);
                if (exportNameId == exportName)
                {
                    localNameId = EnsurePropertyIdForIdentifier(exportEntry.localName);
                    return true;
                }
                return false;
            });
            if (localNameId != Js::Constants::NoProperty)
            {
                // Check to see if we are exporting something we imported from another module without using a re-export.
                // ex: import { foo } from 'module'; export { foo };
                ModuleRecordBase* sourceModule = this;
                ModuleNameRecord* importRecord = nullptr;
                if (this->importRecordList != nullptr
                    && this->ResolveImport(localNameId, &importRecord)
                    && importRecord != nullptr)
                {
                    sourceModule = importRecord->module;
                    localNameId = importRecord->bindingName;
                }
                resolvedExportMap->AddNew(exportName, { sourceModule, localNameId });
                // return the address from Map buffer.
                resolvedExportMap->TryGetReference(exportName, exportRecord);
                return true;
            }
        }

        if (indirectExportRecordList != nullptr)
        {
            bool isAmbiguous = false;
            indirectExportRecordList->MapUntil([&](ModuleImportOrExportEntry exportEntry) {
                PropertyId reexportNameId = EnsurePropertyIdForIdentifier(exportEntry.exportName);
                if (exportName != reexportNameId)
                {
                    return false;
                }

                PropertyId importNameId = EnsurePropertyIdForIdentifier(exportEntry.importName);
                SourceTextModuleRecord* childModuleRecord = GetChildModuleRecord(exportEntry.moduleRequest, exportEntry.attributes);
                if (childModuleRecord == nullptr)
                {
                    this->ReleaseParserResourcesForHierarchy();
                    JavascriptError::ThrowReferenceError(scriptContext, JSERR_CannotResolveModule, exportEntry.moduleRequest->Psz());
                }

                if (exportEntry.importName->GetPropertyId() == PropertyIds::star_)
                {
                    // export * as someName from "foo"
                    *exportRecord = childModuleRecord->GetNamespaceNameRecord();
                    return true;
                }

                isAmbiguous = !childModuleRecord->ResolveExport(importNameId, resolveSet, exportRecord);
                if (isAmbiguous)
                {
                    // ambiguous; don't need to search further
                    return true;
                }

                // found a resolution. done;
                if (*exportRecord != nullptr)
                {
                    return true;
                }
                return false;
            });
            if (isAmbiguous)
            {
                return false;
            }
            if (*exportRecord != nullptr)
            {
                return true;
            }
        }

        if (exportName == PropertyIds::default_)
        {
            return false;
        }

        bool ambiguousResolution = false;
        if (this->starExportRecordList != nullptr)
        {
            ModuleNameRecord* starResolution = nullptr;
            starExportRecordList->MapUntil([&](ModuleImportOrExportEntry starExportEntry) {
                ModuleNameRecord* currentResolution = nullptr;
                SourceTextModuleRecord* childModule = GetChildModuleRecord(starExportEntry.moduleRequest, starExportEntry.attributes);
                if (childModule == nullptr)
                {
                    this->ReleaseParserResourcesForHierarchy();
                    JavascriptError::ThrowReferenceError(GetScriptContext(), JSERR_CannotResolveModule, starExportEntry.moduleRequest->Psz());
                }
                if (childModule->errorObject != nullptr)
                {
                    this->ReleaseParserResourcesForHierarchy();
                    JavascriptExceptionOperators::Throw(childModule->errorObject, GetScriptContext());
                }

                // if ambiguous, return "ambiguous"
                if (!childModule->ResolveExport(exportName, resolveSet, &currentResolution))
                {
                    ambiguousResolution = true;
                    return true;
                }
                if (currentResolution != nullptr)
                {
                    if (starResolution == nullptr)
                    {
                        starResolution = currentResolution;
                    }
                    else
                    {
                        if (currentResolution->bindingName != starResolution->bindingName ||
                            currentResolution->module != starResolution->module)
                        {
                            ambiguousResolution = true;
                        }
                        return true;
                    }
                }
                return false;
            });
            if (!ambiguousResolution)
            {
                *exportRecord = starResolution;
            }
        }
        return !ambiguousResolution;
    }

    void SourceTextModuleRecord::SetParent(SourceTextModuleRecord* parentRecord, LPCOLESTR moduleRequestKey)
    {
        Assert(parentRecord != nullptr);
        parentRecord->EnsureChildModuleSet(GetScriptContext());
        if (!parentRecord->childrenModuleSet->ContainsKey(moduleRequestKey))
        {
            parentRecord->childrenModuleSet->AddNew(moduleRequestKey, this);

            if (this->parentModuleList == nullptr)
            {
                Recycler* recycler = GetScriptContext()->GetRecycler();
                this->parentModuleList = RecyclerNew(recycler, ModuleRecordList, recycler);
            }

            if (!this->parentModuleList->Contains(parentRecord))
            {
                this->parentModuleList->Add(parentRecord);
            }

            if (IsEvaluating())
            {
                parentRecord->IncrementAwaited();
            }
        }
    }

    bool SourceTextModuleRecord::ConfirmChildrenParsed()
    {
        if (!this->WasParsed())
        {
            return false;
        }
        if (confirmedReady || this->ParentsNotified())
        {
            return true;
        }
        bool result = true;
        confirmedReady = true;
        if (childrenModuleSet != nullptr)
        {
            childrenModuleSet->EachValue([&](SourceTextModuleRecord* childModuleRecord) {
                if (childModuleRecord->ParentsNotified())
                {
                    return false;
                }
                else
                {
                    if (childModuleRecord->ConfirmChildrenParsed())
                    {
                        return false;
                    }
                    else
                    {
                        result = false;
                        return true;
                    }
                }
            });
        }
        confirmedReady = false;
        return result;
    }

    void SourceTextModuleRecord::EnsureChildModuleSet(ScriptContext *scriptContext)
    {
        if (nullptr == this->childrenModuleSet)
        {
            ArenaAllocator* allocator = scriptContext->GeneralAllocator();
            this->childrenModuleSet = Anew(allocator, ChildModuleRecordSet, allocator);
        }
    }

    HRESULT SourceTextModuleRecord::ResolveExternalModuleDependencies()
    {
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("ResolveExternalModuleDependencies(%s)\n"), this->GetSpecifierSz());

        ScriptContext* scriptContext = GetScriptContext();

        HRESULT hr = NOERROR;
        if (requestedModuleList != nullptr)
        {
            EnsureChildModuleSet(scriptContext);

            requestedModuleList->MapUntil([&](ModuleRequestEntry requestEntry) {
                IdentPtr specifier = requestEntry.specifier;
                LPCOLESTR moduleName = specifier->Psz();
                // Requests that differ in attributes are distinct requests and
                // resolve to distinct child records.
                LPCOLESTR moduleRequestKey = SourceTextModuleRecord::GetModuleRequestKey(scriptContext->GeneralAllocator(), specifier, requestEntry.attributes);
                ModuleRecordBase* moduleRecordBase = nullptr;
                SourceTextModuleRecord* moduleRecord = nullptr;
                bool itemFound = childrenModuleSet->TryGetValue(moduleRequestKey, &moduleRecord);
                if (!itemFound)
                {
                    Var attributes = BuildModuleAttributesObject(requestEntry.attributes);
                    hr = scriptContext->GetHostScriptContext()->FetchImportedModule(this, moduleName, attributes, &moduleRecordBase);
                    if (hr == E_NOTIMPL)
                    {
                        // The request carries import attributes but the host
                        // registered no attributes-capable callback.
                        JavascriptError *error = scriptContext->GetLibrary()->CreateTypeError();
                        JavascriptError::SetErrorMessageProperties(error, hr, _u("import attributes are not supported by the host"), scriptContext);
                        this->errorObject = error;
                        hr = E_FAIL;
                        return true;
                    }
                    if (FAILED(hr))
                    {
                        return true;
                    }
                    moduleRecord = SourceTextModuleRecord::FromHost(moduleRecordBase);
                    Var errorObject = moduleRecord->GetErrorObject();
                    if (errorObject == nullptr)
                    {
                        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>SetParent in (%s)\n"), moduleRecord->GetSpecifierSz());
                        moduleRecord->SetParent(this, moduleRequestKey);
                    }
                    else
                    {
                        this->errorObject = errorObject;
                        hr = E_FAIL;
                        return true;
                    }
                }
                return false;
            });

            if (FAILED(hr))
            {
                if (this->errorObject == nullptr)
                {
                    JavascriptError *error = scriptContext->GetLibrary()->CreateError();
                    JavascriptError::SetErrorMessageProperties(error, hr, _u("fetch import module failed"), scriptContext);
                    this->errorObject = error;
                }

                OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\tfetch import module failed\n"));
                NotifyParentsAsNeeded();
            }
        }
        return hr;
    }

    void SourceTextModuleRecord::CleanupBeforeExecution()
    {
        // zero out fields is more a defense in depth as those fields are not needed anymore
        Assert(wasParsed);
        Assert(evaluating);
        Assert(wasDeclarationInitialized);
        // Debugger can reparse the source and generate the byte code again. Don't cleanup the
        // helper information for now.
    }

    bool SourceTextModuleRecord::ModuleDeclarationInstantiation()
    {
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("ModuleDeclarationInstantiation(%s)\n"), this->GetSpecifierSz());
        ScriptContext* scriptContext = GetScriptContext();

        if (this->WasDeclarationInitialized() || this->errorObject != nullptr)
        {
            return false;
        }

        try
        {
            AUTO_NESTED_HANDLED_EXCEPTION_TYPE((ExceptionType)(ExceptionType_OutOfMemory|ExceptionType_JavascriptException));
            InitializeLocalExports();

            InitializeLocalImports();

            InitializeIndirectExports();

            SetWasDeclarationInitialized();
            if (childrenModuleSet != nullptr)
            {
                childrenModuleSet->EachValue([=](SourceTextModuleRecord* childModuleRecord)
                {
                    Assert(childModuleRecord->WasParsed());
                    childModuleRecord->ModuleDeclarationInstantiation();
                    // ES2025 16.2.1.5 ModuleDeclarationInstantiation (via
                    // InnerModuleLink): a dependency's failed link is this
                    // module's failed link, and the root's instantiation
                    // must surface it (NotifyHostAboutModuleReady carries
                    // errorObject) before any module-body evaluation.
                    // InitializeLocalImports/InitializeIndirectExports
                    // record a child's unresolvable binding on the child's
                    // own record without throwing, so propagate the first
                    // child error here (first-wins, matching
                    // OnChildModuleReady's error propagation).
                    if (childModuleRecord->errorObject != nullptr && this->errorObject == nullptr)
                    {
                        this->errorObject = childModuleRecord->errorObject;
                    }
                });
            }

            ENTER_SCRIPT_IF(scriptContext, true, false, false, !scriptContext->GetThreadContext()->IsScriptActive(),
            {
                ModuleNamespace::GetModuleNamespace(this);
            });
        }
        catch (const JavascriptException& err)
        {
            this->errorObject = err.GetAndClear()->GetThrownObject(scriptContext);
        }

        if (this->errorObject != nullptr)
        {
            return false;
        }

        return true;
    }

    void SourceTextModuleRecord::GenerateRootFunction()
    {
        // On cyclic dependency, we may end up generating the root function twice
        // so make sure we don't
        if (this->rootFunction != nullptr)
        {
            return;
        }

        ScriptContext* scriptContext = GetScriptContext();
        Js::AutoDynamicCodeReference dynamicFunctionReference(scriptContext);
        CompileScriptException se;

        Assert(this->WasDeclarationInitialized());
        Assert(this == scriptContext->GetLibrary()->GetModuleRecord(this->pSourceInfo->GetSrcInfo()->moduleID));

        this->rootFunction = scriptContext->GenerateRootFunction(parseTree, sourceIndex, this->parser, this->pSourceInfo->GetParseFlags(), &se, Constants::ModuleCode);

        // Parser uses a temporary guest arena to keep regex patterns alive. We need to release this arena only after we have no further use
        // for the regex pattern objects.
        this->ReleaseParserResources();

        if (rootFunction == nullptr)
        {
            const WCHAR * sourceUrl = this->GetSpecifierSz();

            this->errorObject = JavascriptError::CreateFromCompileScriptException(scriptContext, &se, sourceUrl);
            OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("\t>NotifyParentAsNeeded rootFunction == nullptr\n"));
            NotifyParentsAsNeeded();
        }
#ifdef ENABLE_SCRIPT_DEBUGGING
        else
        {
            scriptContext->GetDebugContext()->RegisterFunction(this->rootFunction->GetFunctionBody(), nullptr);
        }
#endif
        if (childrenModuleSet != nullptr)
        {
            childrenModuleSet->EachValue([=](SourceTextModuleRecord* childModuleRecord)
            {
                childModuleRecord->GenerateRootFunction();
            });
        }
    }

    static bool ReportModuleCompletion(SourceTextModuleRecord* module, Var exception)
    {
        bool hasCallback = false;
        ScriptContext* scriptContext = module->GetScriptContext();
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("ReportModuleCompletion %s\n"), module->GetSpecifierSz());
        LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
        {
            hasCallback = scriptContext->GetHostScriptContext()->ReportModuleCompletion(module, exception);
        });
        return hasCallback;
    }

    bool SourceTextModuleRecord::ModuleEvaluationPrepass()
    {
        if (this->errorObject != nullptr)
        {
            // Cleanup in case of error.
            this->ReleaseParserResourcesForHierarchy();

            if (this->promise != nullptr)
            {
                SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(false, this->errorObject, this->scriptContext, this, false);
                return false;
            }
            if (isRootModule && ReportModuleCompletion(this, errorObject))
            {
                return false;
            }
            else
            {
                JavascriptExceptionOperators::Throw(errorObject, this->scriptContext);
            }
        }
        SetEvaluationPrepassed();

#if DBG
        if (childrenModuleSet != nullptr)
        {
            childrenModuleSet->EachValue([=](SourceTextModuleRecord* childModuleRecord)
            {
                AssertMsg(childModuleRecord->WasParsed(), "child module needs to have been parsed");
                AssertMsg(childModuleRecord->WasDeclarationInitialized(), "child module needs to have been initialized.");
            });
        }
#endif

        if (childrenModuleSet != nullptr)
        {
            childrenModuleSet->EachValue([=](SourceTextModuleRecord* childModuleRecord)
            {
                if (!childModuleRecord->WasEvaluationPrepassed())
                {
                    childModuleRecord->ModuleEvaluationPrepass();
                }

                // if child module was evaluated before and threw need to re-throw now
                // if child module has been dynamically imported and has exception need to throw
                if (childModuleRecord->GetErrorObject() != nullptr)
                {
                    this->ReleaseParserResourcesForHierarchy();

                    JavascriptExceptionOperators::Throw(childModuleRecord->GetErrorObject(), this->scriptContext);
                }
            });
        }

        AUTO_NESTED_HANDLED_EXCEPTION_TYPE((ExceptionType)(ExceptionType_OutOfMemory | ExceptionType_JavascriptException));
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            Arguments outArgs(CallInfo(CallFlags_Value, 0), nullptr);
            this->generator = VarTo<JavascriptGenerator>(rootFunction->CallRootFunction(outArgs, scriptContext, true));
        }
        END_SAFE_REENTRANT_CALL

        return true;
    }


    Var SourceTextModuleRecord::ModuleEvaluation()
    {
        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("ModuleEvaluation(%s)\n"), this->GetSpecifierSz());

        if (WasEvaluated() || IsEvaluating() || !scriptContext->GetConfig()->IsES6ModuleEnabled())
        {
            return nullptr;
        }

        try
        {
            if (!WasEvaluationPrepassed())
            {
                if (!ModuleEvaluationPrepass())
                {
                    return scriptContext->GetLibrary()->GetUndefined();
                }
            }

            Assert(this->errorObject == nullptr);
            SetEvaluating(true);
            
            if (requestedModuleList != nullptr)
            {
                requestedModuleList->Reverse();
                requestedModuleList->Map([&](ModuleRequestEntry requestEntry) {
                    SourceTextModuleRecord* childModuleRecord = nullptr;
                    LPCOLESTR moduleRequestKey = SourceTextModuleRecord::GetModuleRequestKey(scriptContext->GeneralAllocator(), requestEntry.specifier, requestEntry.attributes);
                    AssertOrFailFast(childrenModuleSet->TryGetValue(moduleRequestKey, &childModuleRecord));

                    childModuleRecord->ModuleEvaluation();
                    // if child module was evaluated before and threw need to re-throw now
                    // if child module has been dynamically imported and has exception need to throw
                    if (childModuleRecord->GetErrorObject() != nullptr)
                    {
                        this->ReleaseParserResourcesForHierarchy();

                        JavascriptExceptionOperators::Throw(childModuleRecord->GetErrorObject(), this->scriptContext);
                    }
                });
            }
            if (awaitedModules == 0)
            {
                FinishModuleEvaluation(true);
            }
            else
            {
                if (this->parentModuleList != nullptr)
                {
                    parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
                    {
                        parentModule->IncrementAwaited();
                    });
                }
            }
        }
        catch (const Js::JavascriptException &err)
        {
            if (!WasEvaluated())
            {
                SetWasEvaluated();
            }
            SetEvaluating(false);
            JavascriptExceptionObject *exception = err.GetAndClear();
            Var errorObject = exception->GetThrownObject(scriptContext);
            AssertOrFailFastMsg(errorObject != nullptr, "ModuleEvaluation: null error object thrown from root function");
            this->errorObject = errorObject;
            if (this->promise != nullptr)
            {
                ResolveOrRejectDynamicImportPromise(false, errorObject, scriptContext, this, false);
            }
            if (isRootModule && !ReportModuleCompletion(this, errorObject))
            {
                JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
            }
        }

        return scriptContext->GetLibrary()->GetUndefined();
    }

    void SourceTextModuleRecord::FinishModuleEvaluation(bool shouldIncrementAwait)
    {
        if (WasEvaluated())
        {
            return;
        }

        CleanupBeforeExecution();

        JavascriptGenerator* gen = static_cast<JavascriptGenerator*> (generator);

        AUTO_NESTED_HANDLED_EXCEPTION_TYPE((ExceptionType)(ExceptionType_OutOfMemory | ExceptionType_JavascriptException));
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            if (gen->IsAsyncModule())
            {
                JavascriptPromise* prom = JavascriptAsyncFunction::BeginAsyncFunctionExecution(this->generator);
                if (this->parentModuleList != nullptr && shouldIncrementAwait)
                {
                    parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
                    {
                        parentModule->IncrementAwaited();
                    });
                }
                auto* fulfilled = scriptContext->GetLibrary()->CreateAsyncModuleCallbackFunction(EntryAsyncModuleFulfilled, this);
                auto* rejected = scriptContext->GetLibrary()->CreateAsyncModuleCallbackFunction(EntryAsyncModuleRejected, this);
                auto* unused = JavascriptPromise::UnusedPromiseCapability(scriptContext);
                JavascriptPromise::PerformPromiseThen(prom, unused, fulfilled, rejected, scriptContext);
            }
            else
            {
                SetWasEvaluated();
                SetEvaluating(false);
                gen->CallGenerator(scriptContext->GetLibrary()->GetUndefined(), ResumeYieldKind::Normal);
                if (this->parentModuleList != nullptr && !shouldIncrementAwait)
                {
                    parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
                    {
                        if (parentModule->DecrementAwaited())
                        {
                            if(parentModule->IsEvaluating())
                            {
                                parentModule->FinishModuleEvaluation(false);
                            }
                        }
                    });
                }
                if (this->promise != nullptr)
                {
                    ResolveOrRejectDynamicImportPromise(true, GetNamespace(), scriptContext, this, false);
                }
                if (isRootModule)
                {
                    ReportModuleCompletion(this, nullptr);
                }
            }
        }
        END_SAFE_REENTRANT_CALL
    }

    void SourceTextModuleRecord::DecrementAndEvaluateIfNothingAwaited()
    {
        if (DecrementAwaited())
        {
            // A cyclic importer can receive this decrement from a child's
            // async completion before its own evaluation has started (e.g.
            // a dynamically imported module whose load message is still
            // queued while a child it imports in a cycle already finished).
            // FinishModuleEvaluation on a module that never began evaluating
            // would dereference its null generator and tripped asserts on
            // wasParsed/evaluating/declaration-initialized state. Mirrors the
            // IsEvaluating() guard at the other DecrementAwaited call site
            // in FinishModuleEvaluation. The awaited-count bookkeeping stays
            // balanced: the module's own ModuleEvaluation later sees
            // awaitedModules == 0 and evaluates normally.
            if (!IsEvaluating())
            {
                return;
            }
            JavascriptExceptionObject *exception = nullptr;
            try
            {
                FinishModuleEvaluation(false);
            }
            catch (const Js::JavascriptException &err)
            {
                if (!WasEvaluated())
                {
                    SetWasEvaluated();
                }
                SetEvaluating(false);
                exception = err.GetAndClear();
                Var errorObject = exception->GetThrownObject(scriptContext);
                AssertOrFailFastMsg(errorObject != nullptr, "ModuleEvaluation: null error object thrown from root function");

                PropogateRejection(errorObject);
                return;
            }
        }
    }

    Var SourceTextModuleRecord::EntryAsyncModuleFulfilled(
        RecyclableObject* function,
        CallInfo callInfo, ...)
    {
        SourceTextModuleRecord* module = VarTo<AsyncModuleCallbackFunction>(function)->module;
        module->SetEvaluating(false);
        module->SetWasEvaluated();
        if (module->parentModuleList != nullptr)
        {
            module->parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
            {
                parentModule->DecrementAndEvaluateIfNothingAwaited();
            });
        }

        if (module->GetPromise() != nullptr)
        {
            ResolveOrRejectDynamicImportPromise(true, module->GetNamespace(), module->scriptContext, module, false);
        }
        else if (module->GetIsRootModule())
        {
            ReportModuleCompletion(module, nullptr);
        }

        return function->GetLibrary()->GetUndefined();
    }

    Var SourceTextModuleRecord::EntryAsyncModuleRejected(
        RecyclableObject* function,
        CallInfo callInfo, ...)
    {
        SourceTextModuleRecord* module = VarTo<AsyncModuleCallbackFunction>(function)->module;

        PROBE_STACK(module->scriptContext, Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);

        Assert (args.Info.Count > 1);
        module->PropogateRejection(args[1]);

        return function->GetLibrary()->GetUndefined();
    }

    void SourceTextModuleRecord::PropogateRejection(Var reason)
    {
        SetEvaluating(false);
        if (wasEvaluated)
        {
            return;
        }

        SetWasEvaluated();

        this->errorObject = reason;
        if (this->parentModuleList != nullptr)
        {
            parentModuleList->Map([=](uint i, SourceTextModuleRecord* parentModule)
            {
                if (parentModule->GetErrorObject() == nullptr)
                {
                    parentModule->PropogateRejection(reason);
                }
            });
        }

        if (this->promise != nullptr)
        {
            ResolveOrRejectDynamicImportPromise(false, reason, scriptContext, this, false);
        }
        if (isRootModule)
        {
            ReportModuleCompletion(this, errorObject);
        }
    }

    HRESULT SourceTextModuleRecord::OnHostException(void* errorVar)
    {
        if (!VarIs<RecyclableObject>(errorVar))
        {
            return E_INVALIDARG;
        }
        if (WasParsed())
        {
            return E_INVALIDARG;
        }
        this->errorObject = errorVar;

        // a sub module failed to download. we need to notify that the parent that sub module failed and we need to report back.
        return NOERROR;
    }

    SourceTextModuleRecord* SourceTextModuleRecord::GetChildModuleRecord(LPCOLESTR specifier) const
    {
        SourceTextModuleRecord* childModuleRecord = nullptr;
        if (childrenModuleSet == nullptr)
        {
            AssertMsg(false, "We should have some child modulerecords first before trying to get child modulerecord.");
            return nullptr;
        }
        if (!childrenModuleSet->TryGetValue(specifier, &childModuleRecord))
        {
            return nullptr;
        }
        return childModuleRecord;
    }

    SourceTextModuleRecord* SourceTextModuleRecord::GetChildModuleRecord(IdentPtr specifier, ModuleAttributeList* attributes) const
    {
        LPCOLESTR moduleRequestKey = SourceTextModuleRecord::GetModuleRequestKey(scriptContext->GeneralAllocator(), specifier, attributes);
        return GetChildModuleRecord(moduleRequestKey);
    }

    LPCOLESTR SourceTextModuleRecord::GetModuleRequestKey(ArenaAllocator* allocator, IdentPtr specifier, ModuleAttributeList* attributes)
    {
        // Canonical request identity: the specifier followed by the sorted
        // attribute key/value pairs. Components are joined with U+001F, with
        // literal U+001F bytes doubled so the encoding is unambiguous for
        // arbitrary string content. A missing WithClause is equivalent to an
        // empty attribute list, so both produce the bare specifier key.
        // The key is allocated from the given arena; callers pass the general
        // allocator, whose lifetime covers the child-module map that retains
        // insertion keys.
        const char16 separator = 0x1F;
        uint32 totalLength = specifier->Cch();
        for (ModuleAttributeEntry* entry = (attributes != nullptr) ? attributes->head : nullptr;
            entry != nullptr; entry = entry->next)
        {
            // Two components plus two separators per attribute; extra room
            // for doubled separators is added below per component.
            totalLength += 2 + entry->key->Cch() + entry->value->Cch();
        }
        // Account for separators already in `totalLength` above; now add
        // room for doubled separators inside component text.
        auto countSeparators = [](const char16* text, uint32 length) {
            uint32 count = 0;
            for (uint32 i = 0; i < length; i++)
            {
                if (text[i] == separator)
                {
                    count++;
                }
            }
            return count;
        };
        totalLength += countSeparators(specifier->Psz(), specifier->Cch());
        for (ModuleAttributeEntry* entry = (attributes != nullptr) ? attributes->head : nullptr;
            entry != nullptr; entry = entry->next)
        {
            totalLength += countSeparators(entry->key->Psz(), entry->key->Cch());
            totalLength += countSeparators(entry->value->Psz(), entry->value->Cch());
        }

        char16* key = AnewArray(allocator, char16, totalLength + 1);
        char16* cursor = key;
        auto appendComponent = [&](const char16* text, uint32 length) {
            if (cursor != key)
            {
                *cursor++ = separator;
            }
            for (uint32 i = 0; i < length; i++)
            {
                *cursor++ = text[i];
                if (text[i] == separator)
                {
                    *cursor++ = separator;
                }
            }
        };
        appendComponent(specifier->Psz(), specifier->Cch());
        for (ModuleAttributeEntry* entry = (attributes != nullptr) ? attributes->head : nullptr;
            entry != nullptr; entry = entry->next)
        {
            appendComponent(entry->key->Psz(), entry->key->Cch());
            appendComponent(entry->value->Psz(), entry->value->Cch());
        }
        *cursor = 0;
        Assert((uint32)(cursor - key) == totalLength);
        return key;
    }

    Var SourceTextModuleRecord::BuildModuleAttributesObject(ModuleAttributeList* attributes)
    {
        if (attributes == nullptr || attributes->head == nullptr)
        {
            // No attributes: the caller takes the legacy host path (or the
            // new callback with `undefined`, when the host registered it).
            return nullptr;
        }
        ScriptContext* scriptContext = GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        // Ownership record: Recycler-owned plain object created for one
        // synchronous host fetch. It is referenced from this frame's native
        // stack (scanned) until the host returns; the host must JsAddRef any
        // value it retains past return. Properties are plain data properties
        // on a fresh ordinary object, so no user code runs during construction.
        // OOM (JavascriptException) propagates like the other allocations on
        // this path and is converted at the API boundary.
        DynamicObject* attributesObject = library->CreateObject();
        for (ModuleAttributeEntry* entry = attributes->head; entry != nullptr; entry = entry->next)
        {
            PropertyId propertyId = EnsurePropertyIdForIdentifier(entry->key);
            JavascriptString* value = JavascriptString::NewCopySz(entry->value->Psz(), scriptContext);
            JavascriptOperators::SetProperty(attributesObject, attributesObject, propertyId, value, scriptContext, PropertyOperation_None);
        }
        return attributesObject;
    }

    void SourceTextModuleRecord::InitializeLocalImports()
    {
        if (importRecordList != nullptr)
        {
            importRecordList->Map([&](ModuleImportOrExportEntry& importEntry) {
                Js::PropertyId importName = EnsurePropertyIdForIdentifier(importEntry.importName);

                SourceTextModuleRecord* childModule = this->GetChildModuleRecord(importEntry.moduleRequest, importEntry.attributes);
                ModuleNameRecord* importRecord = nullptr;
                // We don't need to initialize anything for * import.
                if (importName != Js::PropertyIds::star_)
                {
                    if (!childModule->ResolveExport(importName, nullptr, &importRecord)
                        || importRecord == nullptr)
                    {
                        JavascriptError* errorObj = scriptContext->GetLibrary()->CreateSyntaxError();
                        JavascriptError::SetErrorMessage(errorObj, JSERR_ModuleResolveImport, importEntry.importName->Psz(), scriptContext);
                        this->errorObject = errorObj;
                        return;
                    }
                }
            });
        }
    }

    // Local exports are stored in the slotarray in the SourceTextModuleRecord.
    void SourceTextModuleRecord::InitializeLocalExports()
    {
        Recycler* recycler = scriptContext->GetRecycler();
        Var undefineValue = scriptContext->GetLibrary()->GetUndefined();
        if (localSlotCount == InvalidSlotCount)
        {
            uint currentSlotCount = 0;
            if (localExportRecordList != nullptr)
            {
                ArenaAllocator* allocator = scriptContext->GeneralAllocator();
                localExportMapByExportName = Anew(allocator, LocalExportMap, allocator);
                localExportMapByLocalName = Anew(allocator, LocalExportMap, allocator);
                localExportIndexList = Anew(allocator, LocalExportIndexList, allocator);
                localExportRecordList->Map([&](ModuleImportOrExportEntry exportEntry)
                {
                    Assert(exportEntry.moduleRequest == nullptr);
                    Assert(exportEntry.importName == nullptr);
                    PropertyId exportNameId = EnsurePropertyIdForIdentifier(exportEntry.exportName);
                    PropertyId localNameId = EnsurePropertyIdForIdentifier(exportEntry.localName);

                    // We could have exports that look local but actually exported from other module
                    // import {foo} from "module1.js"; export {foo};
                    ModuleNameRecord* importRecord = nullptr;
                    if (this->GetImportEntryList() != nullptr
                        && this->ResolveImport(localNameId, &importRecord)
                        && importRecord != nullptr)
                    {
                        return;
                    }

                    // 2G is too big already.
                    if (localExportCount >= INT_MAX)
                    {
                        this->ReleaseParserResourcesForHierarchy();
                        JavascriptError::ThrowRangeError(scriptContext, JSERR_TooManyImportExports);
                    }
                    localExportCount++;
                    uint exportSlot = UINT_MAX;

                    for (uint i = 0; i < (uint)localExportIndexList->Count(); i++)
                    {
                        if (localExportIndexList->Item(i) == localNameId)
                        {
                            exportSlot = i;
                            break;
                        }
                    }

                    if (exportSlot == UINT_MAX)
                    {
                        exportSlot = currentSlotCount;
                        localExportMapByLocalName->Add(localNameId, exportSlot);
                        localExportIndexList->Add(localNameId);
                        Assert(localExportIndexList->Item(currentSlotCount) == localNameId);
                        currentSlotCount++;
                        if (currentSlotCount >= INT_MAX)
                        {
                            this->ReleaseParserResourcesForHierarchy();
                            JavascriptError::ThrowRangeError(scriptContext, JSERR_TooManyImportExports);
                        }
                    }

                    localExportMapByExportName->Add(exportNameId, exportSlot);

                });
            }
            // Namespace object will be added to the end of the array though invisible through namespace object itself.
            localExportSlots = RecyclerNewArray(recycler, Field(Var), currentSlotCount + 1);
            for (uint i = 0; i < currentSlotCount; i++)
            {
                localExportSlots[i] = undefineValue;
            }
            localExportSlots[currentSlotCount] = nullptr;

            localSlotCount = currentSlotCount;

#if ENABLE_NATIVE_CODEGEN
            if (JITManager::GetJITManager()->IsOOPJITEnabled() && JITManager::GetJITManager()->IsConnected())
            {
                PSCRIPTCONTEXT_HANDLE remoteScriptContext = this->scriptContext->GetRemoteScriptAddr(false);
                if (remoteScriptContext)
                {
                    HRESULT hr = JITManager::GetJITManager()->AddModuleRecordInfo(
                        remoteScriptContext,
                        this->GetModuleId(),
                        (intptr_t)this->GetLocalExportSlots());
                    JITManager::HandleServerCallResult(hr, RemoteCallType::StateUpdate);
                }
            }
#endif
        }
    }

    PropertyId SourceTextModuleRecord::EnsurePropertyIdForIdentifier(IdentPtr pid)
    {
        PropertyId propertyId = pid->GetPropertyId();
        if (propertyId == Js::Constants::NoProperty)
        {
            propertyId = scriptContext->GetOrAddPropertyIdTracked(pid->Psz(), pid->Cch());
            pid->SetPropertyId(propertyId);
        }
        return propertyId;
    }

    void SourceTextModuleRecord::InitializeIndirectExports()
    {
        ModuleNameRecord* exportRecord = nullptr;
        if (indirectExportRecordList != nullptr)
        {
            indirectExportRecordList->Map([&](ModuleImportOrExportEntry exportEntry)
            {
                PropertyId propertyId = EnsurePropertyIdForIdentifier(exportEntry.importName);
                SourceTextModuleRecord* childModuleRecord = GetChildModuleRecord(exportEntry.moduleRequest, exportEntry.attributes);
                if (childModuleRecord == nullptr)
                {
                    JavascriptError* errorObj = scriptContext->GetLibrary()->CreateReferenceError();
                    JavascriptError::SetErrorMessage(errorObj, JSERR_CannotResolveModule, exportEntry.moduleRequest->Psz(), scriptContext);
                    this->errorObject = errorObj;
                    return;
                }
                if (propertyId != PropertyIds::star_ &&
                    (!childModuleRecord->ResolveExport(propertyId, nullptr, &exportRecord) ||
                    (exportRecord == nullptr)))
                {
                    JavascriptError* errorObj = scriptContext->GetLibrary()->CreateSyntaxError();
                    JavascriptError::SetErrorMessage(errorObj, JSERR_ModuleResolveExport, exportEntry.exportName->Psz(), scriptContext);
                    this->errorObject = errorObj;
                    return;
                }
            });
        }
    }

    uint SourceTextModuleRecord::GetLocalExportSlotIndexByExportName(PropertyId exportNameId)
    {
        Assert(localSlotCount != 0);
        Assert(localExportSlots != nullptr);
        uint slotIndex = InvalidSlotIndex;
        if (!localExportMapByExportName->TryGetValue(exportNameId, &slotIndex))
        {
            AssertMsg(false, "exportNameId is not in local export list");
            return InvalidSlotIndex;
        }
        else
        {
            return slotIndex;
        }
    }

    uint SourceTextModuleRecord::GetLocalExportSlotIndexByLocalName(PropertyId localNameId)
    {
        Assert(localSlotCount != 0 || localNameId == PropertyIds::star_);
        Assert(localExportSlots != nullptr);
        uint slotIndex = InvalidSlotIndex;
        if (localNameId == PropertyIds::star_)
        {
            return localSlotCount;  // namespace is put on the last slot.
        } else if (!localExportMapByLocalName->TryGetValue(localNameId, &slotIndex))
        {
            AssertMsg(false, "exportNameId is not in local export list");
            return InvalidSlotIndex;
        }
        return slotIndex;
    }

    // static
    Var SourceTextModuleRecord::ResolveOrRejectDynamicImportPromise(bool isResolve, Var value, ScriptContext *scriptContext, SourceTextModuleRecord *moduleRecord, bool useReturn)
    {
        bool isScriptActive = scriptContext->GetThreadContext()->IsScriptActive();
        JavascriptPromise *promise = nullptr;
        if (moduleRecord != nullptr)
        {
            promise = moduleRecord->GetPromise();
        }

        if (promise == nullptr)
        {
            promise = JavascriptPromise::CreateEnginePromise(scriptContext);
        }

        ENTER_SCRIPT_IF(scriptContext, true, false, false, !isScriptActive,
        {
            if (isResolve)
            {
                promise->Resolve(value, scriptContext);
            }
            else
            {
                promise->Reject(value, scriptContext);
            }
        });

        if (moduleRecord != nullptr)
        {
            moduleRecord->SetPromise(nullptr);
            if (!moduleRecord->WasEvaluated())
            {
                moduleRecord->SetWasEvaluated();
            }
            if (useReturn)
            {
                return JavascriptPromise::CreatePassThroughPromise(promise, scriptContext);
            }
        }
        return promise;
    }

    Var SourceTextModuleRecord::GetImportMetaObject()
    {
        if (this->importMetaObject)
        {
            return this->importMetaObject;
        }

        ScriptContext* scriptContext = this->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        this->importMetaObject = library->CreateObject(library->GetNull());

        OUTPUT_TRACE_DEBUGONLY(Js::ModulePhase, _u("InitializeImportMeta %s\n"), this->GetSpecifierSz());
        LEAVE_SCRIPT_IF_ACTIVE(scriptContext,
        {
            scriptContext->GetHostScriptContext()->InitializeImportMeta(this, this->importMetaObject);
        });

        return this->importMetaObject;
    }

    template<>
    bool VarIsImpl<AsyncModuleCallbackFunction>(RecyclableObject* obj)
    {
        return VarIs<JavascriptFunction>(obj) && (
            VirtualTableInfo<AsyncModuleCallbackFunction>::HasVirtualTable(obj) ||
            VirtualTableInfo<CrossSiteObject<AsyncModuleCallbackFunction>>::HasVirtualTable(obj)
        );
    }

}
