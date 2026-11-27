//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

using namespace Js;

FunctionInfo JavascriptAsyncGeneratorFunction::functionInfo(
    FORCE_NO_WRITE_BARRIER_TAG(
        JavascriptAsyncGeneratorFunction::EntryAsyncGeneratorFunctionImplementation),
    (FunctionInfo::Attributes)(FunctionInfo::DoNotProfile | FunctionInfo::ErrorOnNew));

JavascriptAsyncGeneratorFunction::JavascriptAsyncGeneratorFunction(
    DynamicType* type,
    GeneratorVirtualScriptFunction* scriptFunction) :
        JavascriptGeneratorFunction(type, &functionInfo, scriptFunction)
{
    DebugOnly(VerifyEntryPoint());
}

JavascriptAsyncGeneratorFunction* JavascriptAsyncGeneratorFunction::New(
    ScriptContext* scriptContext,
    GeneratorVirtualScriptFunction* scriptFunction)
{
    return scriptContext->GetLibrary()->CreateAsyncGeneratorFunction(
        functionInfo.GetOriginalEntryPoint(),
        scriptFunction);
}

template<>
bool Js::VarIsImpl<JavascriptAsyncGeneratorFunction>(RecyclableObject* obj)
{
    return VarIs<JavascriptFunction>(obj) && (
        VirtualTableInfo<JavascriptAsyncGeneratorFunction>::HasVirtualTable(obj) ||
        VirtualTableInfo<CrossSiteObject<JavascriptAsyncGeneratorFunction>>::HasVirtualTable(obj)
    );
}

Var JavascriptAsyncGeneratorFunction::EntryAsyncGeneratorFunctionImplementation(
    RecyclableObject* function,
    CallInfo callInfo, ...)
{
    auto* scriptContext = function->GetScriptContext();
    PROBE_STACK(scriptContext, Js::Constants::MinStackDefault);
    ARGUMENTS(args, callInfo);

    Assert(!(callInfo.Flags & CallFlags_New));

    auto* asyncGeneratorFn = VarTo<JavascriptAsyncGeneratorFunction>(function);
    auto* library = scriptContext->GetLibrary();
    auto* scriptFn = asyncGeneratorFn->GetGeneratorVirtualScriptFunction();
    const bool hasComplexInit = scriptFn->GetFunctionInfo()->GetGeneratorWithComplexParams();
    DynamicObject* defaultProto = library->GetAsyncGeneratorPrototype();
    DynamicObject* protoObject = defaultProto;

    if (!hasComplexInit)
    {
        Var prototype = JavascriptOperators::GetPropertyNoCache(function, Js::PropertyIds::prototype, scriptContext);

        // fall back to the original prototype if we have an invalid prototype object
        if (VarIs<DynamicObject>(prototype))
        {
            protoObject = UnsafeVarTo<DynamicObject>(prototype);
        }
    }

    auto* generator = library->CreateAsyncGenerator(args, scriptFn, protoObject);

    // Run the generator to execute until the beginning of the body
    if (hasComplexInit)
    {
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            generator->CallGenerator(library->GetUndefined(), ResumeYieldKind::Normal);
        }
        END_SAFE_REENTRANT_CALL

        // Parameter initializers run before reading `function.prototype`.
        Var prototype = JavascriptOperators::GetPropertyNoCache(function, Js::PropertyIds::prototype, scriptContext);
        if (VarIs<DynamicObject>(prototype))
        {
            DynamicObject* postProto = UnsafeVarTo<DynamicObject>(prototype);
            if (postProto != defaultProto)
            {
                JavascriptObject::ChangePrototype(generator, postProto, /*shouldThrow=*/false, scriptContext);
            }
        }
    }

    generator->SetSuspendedStart();

    return generator;
}
