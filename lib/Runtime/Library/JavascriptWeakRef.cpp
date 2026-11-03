//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
    Var JavascriptWeakRef::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("WeakRef"));
        }

        // A normal `new WeakRef` does not carry CallFlags_NewTarget; its
        // effective NewTarget is the active constructor. Reflect.construct and
        // super calls supply the explicit NewTarget argument.
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("WeakRef"));
        }

        Var targetVar = (args.Info.Count > 1) ? args[1] : library->GetUndefined();
        RecyclableObject* target = nullptr;

        if (VarIs<JavascriptSymbol>(targetVar))
        {
            // ES2023 CanBeHeldWeakly: an unregistered Symbol is eligible; a
            // Symbol.for registration is not.
            JavascriptSymbol* symbol = VarTo<JavascriptSymbol>(targetVar);
            if (symbol->IsRegistered())
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("WeakRef"));
            }
            target = symbol;
        }
        else if (JavascriptOperators::IsObject(targetVar) &&
                 JavascriptOperators::GetTypeId(targetVar) != TypeIds_HostDispatch)
        {
            target = VarTo<RecyclableObject>(targetVar);
        }
        else
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("WeakRef"));
        }

        // KeepDuringJob must precede the reentrant NewTarget.prototype lookup:
        // a getter/Proxy trap may run an exhaustive GC before the WeakRef is
        // returned. The target remains strongly reachable until OnScriptEnd.
        scriptContext->KeepDuringJob(target);

        JavascriptWeakRef* weakRef = library->CreateWeakRef(target);
        if (weakRef == nullptr)
        {
            // The JS-visible host-dispatch/external surface has no recycler
            // allocation for the weak-reference map. Reject it rather than
            // retaining the target strongly or publishing a half-created
            // object.
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedObject, _u("WeakRef"));
        }

        // GetPrototypeFromConstructor, including the realm fallback when
        // NewTarget.prototype is not an object. Using the NewTarget's library
        // is required by proto-from-ctor-realm.
        DynamicObject* intrinsicProto = library->GetWeakRefPrototype();
        RecyclableObject* newTargetObject = VarTo<RecyclableObject>(newTarget);
        Var proto = JavascriptOperators::GetPropertyNoCache(newTargetObject, PropertyIds::prototype, scriptContext);

        if (JavascriptOperators::IsObject(proto))
        {
            if (VarTo<RecyclableObject>(proto) != intrinsicProto)
            {
                JavascriptObject::ChangePrototype(weakRef, VarTo<RecyclableObject>(proto), true, scriptContext);
            }
        }
        else
        {
            // Realm fallback via the shared spec chain (proxy/bound targets included).
            ScriptContext* newTargetContext = JavascriptOperators::GetFunctionRealm(newTargetObject, scriptContext);
            if (newTargetContext != nullptr && newTargetContext != scriptContext)
            {
                DynamicObject* realmProto = newTargetContext->GetLibrary()->GetWeakRefPrototype();
                if (realmProto != intrinsicProto)
                {
                    JavascriptObject::ChangePrototype(weakRef, realmProto, true, scriptContext);
                }
            }
        }

        return weakRef;
    }

    Var JavascriptWeakRef::EntryDeref(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (!VarIs<JavascriptWeakRef>(args[0]))
        {
            JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("WeakRef.prototype.deref"), _u("WeakRef"));
        }

        return VarTo<JavascriptWeakRef>(args[0])->Deref(scriptContext);
    }

    Var JavascriptWeakRef::Deref(ScriptContext* requestContext) const
    {
        RecyclableObject* target = targetWeakRef != nullptr ? targetWeakRef->Get() : nullptr;
        if (target == nullptr)
        {
            return requestContext->GetLibrary()->GetUndefined();
        }

        // ES WeakRefDeref: AddToKeptObjects(target) before returning it.
        requestContext->KeepDuringJob(target);
        return CrossSite::MarshalVar(requestContext, target);
    }

    BOOL JavascriptWeakRef::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
    {
        stringBuilder->AppendCppLiteral(_u("WeakRef"));
        return TRUE;
    }
}
