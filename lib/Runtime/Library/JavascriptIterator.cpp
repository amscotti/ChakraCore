//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
namespace
{
    // An Iterator Record { [[Iterator]], [[NextMethod]], [[Done]] }.
    struct IterRecord
    {
        RecyclableObject* iterator = nullptr;
        Var nextMethod = nullptr;
        bool done = false;
    };

    Var CallVar(ScriptContext* scriptContext, Var funcVar, Var thisVar, Var arg1, Var arg2, Var arg3, uint argCount)
    {
        if (!JavascriptConversion::IsCallable(funcVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        RecyclableObject* callable = VarTo<RecyclableObject>(funcVar);
        Var argsArr[4] = { thisVar, arg1, arg2, arg3 };
        CallInfo callInfo(CallFlags_Value, static_cast<uint>(1 + argCount));
        return scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [&]() -> Var
        {
            return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callInfo, argsArr));
        });
    }

    Var CallVar(ScriptContext* scriptContext, Var funcVar, Var thisVar)
    {
        return CallVar(scriptContext, funcVar, thisVar, nullptr, nullptr, nullptr, 0);
    }

    // IteratorClose with a throw completion: consult "return" exactly
    // once if the record is not already done, and swallow every error
    // raised while closing (the caller rethrows the original error).
    void CloseRecordSwallow(IterRecord& record, ScriptContext* scriptContext)
    {
        if (record.done || record.iterator == nullptr)
        {
            return;
        }
        record.done = true;
        try
        {
            Var returnMethod = JavascriptOperators::GetProperty(record.iterator, PropertyIds::return_, scriptContext);
            if (JavascriptOperators::IsUndefinedOrNull(returnMethod) || !JavascriptConversion::IsCallable(returnMethod))
            {
                return;
            }
            CallVar(scriptContext, returnMethod, record.iterator);
        }
        catch (const JavascriptException& err)
        {
            err.GetAndClear();
        }
    }

    // IteratorClose with a normal (or return) completion: errors while
    // closing propagate, as does a non-object result from "return".
    void CloseRecordNormal(IterRecord& record, ScriptContext* scriptContext)
    {
        if (record.done || record.iterator == nullptr)
        {
            return;
        }
        record.done = true;
        Var returnMethod = JavascriptOperators::GetProperty(record.iterator, PropertyIds::return_, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
        {
            return;
        }
        if (!JavascriptConversion::IsCallable(returnMethod))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        Var result = CallVar(scriptContext, returnMethod, record.iterator);
        if (!JavascriptOperators::IsObject(result))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
    }

    // GetIteratorDirect: cache the "next" method exactly once. A
    // non-callable next is only rejected when it is actually called.
    void GetIteratorDirect(IterRecord& record, RecyclableObject* iterator, ScriptContext* scriptContext)
    {
        record.iterator = iterator;
        record.done = false;
        record.nextMethod = JavascriptOperators::GetPropertyNoCache(iterator, PropertyIds::next, scriptContext);
    }

    // GetIteratorFlattenable(obj, stringHandling). Primitive strings are
    // only iterated when rejectStrings is false (Iterator.from); every
    // other primitive is rejected. Iterables go through @@iterator; a
    // null/undefined @@iterator falls back to treating obj as an iterator.
    void GetIteratorFlattenable(IterRecord& record, Var obj, bool rejectStrings, ScriptContext* scriptContext)
    {
        if (!JavascriptOperators::IsObject(obj))
        {
            if (rejectStrings || JavascriptOperators::GetTypeId(obj) != TypeIds_String)
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            // Get @@iterator with the primitive string as receiver so a
            // getter on String.prototype observes a string `this`.
            RecyclableObject* propertyObject = JavascriptOperators::ToObject(obj, scriptContext);
            Var method = JavascriptOperators::GetProperty(obj, propertyObject, PropertyIds::_symbolIterator, scriptContext);
            if (JavascriptOperators::IsUndefinedOrNull(method) || !JavascriptConversion::IsCallable(method))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
            }
            Var iterator = CallVar(scriptContext, method, obj);
            if (!JavascriptOperators::IsObject(iterator))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            GetIteratorDirect(record, VarTo<RecyclableObject>(iterator), scriptContext);
            return;
        }

        RecyclableObject* objRecord = VarTo<RecyclableObject>(obj);
        Var method = JavascriptOperators::GetPropertyNoCache(objRecord, PropertyIds::_symbolIterator, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(method))
        {
            GetIteratorDirect(record, objRecord, scriptContext);
            return;
        }
        if (!JavascriptConversion::IsCallable(method))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        Var iterator = CallVar(scriptContext, method, objRecord);
        if (!JavascriptOperators::IsObject(iterator))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        GetIteratorDirect(record, VarTo<RecyclableObject>(iterator), scriptContext);
    }

    // IteratorStepValue: true with *outValue, or false when done. Every
    // abrupt outcome marks the record done before propagating, so the
    // caller closes other records but never this one again.
    bool StepValue(IterRecord& record, ScriptContext* scriptContext, Var* outValue)
    {
        Var resultVar = nullptr;
        bool done = false;
        try
        {
            resultVar = CallVar(scriptContext, record.nextMethod, record.iterator);
            if (!JavascriptOperators::IsObject(resultVar))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            RecyclableObject* result = VarTo<RecyclableObject>(resultVar);
            done = JavascriptOperators::IteratorComplete(result, scriptContext);
            if (!done)
            {
                *outValue = JavascriptOperators::IteratorValue(result, scriptContext);
            }
        }
        catch (const JavascriptException& err)
        {
            record.done = true;
            JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
        }
        if (done)
        {
            record.done = true;
            return false;
        }
        return true;
    }

    RecyclableObject* ValidateIteratorThis(Arguments& args, ScriptContext* scriptContext)
    {
        if (!JavascriptOperators::IsObject(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        return VarTo<RecyclableObject>(args[0]);
    }

    Var ArgOrUndefined(Arguments& args, uint index, ScriptContext* scriptContext)
    {
        return args.Info.Count > index ? args[index] : scriptContext->GetLibrary()->GetUndefined();
    }

    // Callable validation happens before GetIteratorDirect reads "next";
    // on failure the placeholder record closes the underlying iterator.
    void ValidateCallableOrClose(RecyclableObject* thisObject, Var callback, ScriptContext* scriptContext)
    {
        if (JavascriptConversion::IsCallable(callback))
        {
            return;
        }
        IterRecord placeholder;
        placeholder.iterator = thisObject;
        CloseRecordSwallow(placeholder, scriptContext);
        JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
    }

    // take/drop limit validation, in spec order, closing on failure.
    double ValidateLimitOrClose(RecyclableObject* thisObject, Var limit, ScriptContext* scriptContext)
    {
        IterRecord placeholder;
        placeholder.iterator = thisObject;
        double numLimit = 0;
        try
        {
            numLimit = JavascriptConversion::ToNumber(limit, scriptContext);
        }
        catch (const JavascriptException& err)
        {
            JavascriptExceptionObject* exception = err.GetAndClear();
            CloseRecordSwallow(placeholder, scriptContext);
            JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
        }
        const double maxSafeInteger = 9007199254740991.0; // 2^53 - 1
        if (JavascriptNumber::IsNan(numLimit) || (NumberUtilities::IsFinite(numLimit) && numLimit > maxSafeInteger))
        {
            CloseRecordSwallow(placeholder, scriptContext);
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid);
        }
        double integerLimit = JavascriptConversion::ToInteger(numLimit);
        if (integerLimit < 0)
        {
            CloseRecordSwallow(placeholder, scriptContext);
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid);
        }
        return integerLimit;
    }
} // namespace

    Var JavascriptIterator::EntrySymbolIterator(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);

        Assert(!(callInfo.Flags & CallFlags_New));

        // Simply return 'this'
        return args[0];
    }

    Var JavascriptIterator::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Var newTarget = args.GetNewTarget();
        JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);

        // The Iterator constructor is abstract: calling it, or
        // constructing it with itself as NewTarget, throws a TypeError.
        // Subclasses (NewTarget different from the active function) work.
        if (!args.IsNewCall() || newTarget == nullptr || JavascriptOperators::IsUndefinedObject(newTarget) || newTarget == function)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Iterator"));
        }

        RecyclableObject* newTargetObject = VarTo<RecyclableObject>(newTarget);
        // GetPrototypeFromConstructor: NewTarget.prototype if it is an
        // object, else %Iterator.prototype% from NewTarget's realm.
        Var proto = JavascriptOperators::GetPropertyNoCache(newTargetObject, PropertyIds::prototype, scriptContext);
        RecyclableObject* result = library->CreateObject(library->GetIteratorPrototype());
        if (JavascriptOperators::IsObject(proto))
        {
            if (VarTo<RecyclableObject>(proto) != library->GetIteratorPrototype())
            {
                JavascriptObject::ChangePrototype(result, VarTo<RecyclableObject>(proto), true, scriptContext);
            }
        }
        else
        {
            ScriptContext* newTargetContext = newTargetObject->GetScriptContext();
            if (newTargetContext != nullptr && newTargetContext != scriptContext)
            {
                DynamicObject* realmProto = newTargetContext->GetLibrary()->GetIteratorPrototype();
                if (realmProto != library->GetIteratorPrototype())
                {
                    JavascriptObject::ChangePrototype(result, realmProto, true, scriptContext);
                }
            }
        }
        return result;
    }

    Var JavascriptIterator::EntryFrom(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();

        Assert(!(callInfo.Flags & CallFlags_New));

        Var obj = ArgOrUndefined(args, 1, scriptContext);
        IterRecord record;
        GetIteratorFlattenable(record, obj, false, scriptContext);

        // An iterator already inheriting from %Iterator% is used
        // directly; anything else is wrapped so the helpers apply.
        JavascriptFunction* iteratorConstructor = library->GetIteratorConstructor();
        if (iteratorConstructor != nullptr && iteratorConstructor->HasInstance(record.iterator, scriptContext, nullptr))
        {
            return record.iterator;
        }
        return library->CreateWrapForValidIterator(record.iterator, record.nextMethod);
    }

    namespace
    {
        Var SetterThatIgnoresPrototypeProperties(Var thisValue, DynamicObject* home, PropertyId propertyId, Var value, ScriptContext* scriptContext)
        {
            if (!JavascriptOperators::IsObject(thisValue))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            RecyclableObject* thisObject = VarTo<RecyclableObject>(thisValue);
            if (thisObject == home)
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            PropertyDescriptor descriptor;
            BOOL hasOwn = JavascriptOperators::GetOwnPropertyDescriptor(thisObject, propertyId, scriptContext, &descriptor);
            if (!hasOwn)
            {
                PropertyDescriptor newDescriptor;
                newDescriptor.SetValue(value);
                newDescriptor.SetWritable(true);
                newDescriptor.SetEnumerable(true);
                newDescriptor.SetConfigurable(true);
                JavascriptOperators::DefineOwnPropertyDescriptor(thisObject, propertyId, newDescriptor, true, scriptContext);
            }
            else
            {
                if (!JavascriptOperators::SetProperty(thisObject, thisObject, propertyId, value, scriptContext, PropertyOperation_ThrowIfNotExtensible))
                {
                    JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
                }
            }
            return scriptContext->GetLibrary()->GetUndefined();
        }
    } // namespace

    Var JavascriptIterator::EntryGetterConstructor(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return function->GetScriptContext()->GetLibrary()->GetIteratorConstructor();
    }

    Var JavascriptIterator::EntrySetterConstructor(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        Var value = ArgOrUndefined(args, 1, scriptContext);
        return SetterThatIgnoresPrototypeProperties(args[0], scriptContext->GetLibrary()->GetIteratorPrototype(), PropertyIds::constructor, value, scriptContext);
    }

    Var JavascriptIterator::EntryGetterToStringTag(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return function->GetScriptContext()->GetLibrary()->CreateStringFromCppLiteral(_u("Iterator"));
    }

    Var JavascriptIterator::EntrySetterToStringTag(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));
        Var value = ArgOrUndefined(args, 1, scriptContext);
        return SetterThatIgnoresPrototypeProperties(args[0], scriptContext->GetLibrary()->GetIteratorPrototype(), PropertyIds::_symbolToStringTag, value, scriptContext);
    }

    namespace
    {
        Var CreateLazyHelper(Arguments& args, ScriptContext* scriptContext, IteratorHelperKind kind, bool takesCallback, bool takesLimit)
        {
            RecyclableObject* thisObject = ValidateIteratorThis(args, scriptContext);
            Var callback = nullptr;
            double remaining = 0;
            if (takesCallback)
            {
                callback = ArgOrUndefined(args, 1, scriptContext);
                ValidateCallableOrClose(thisObject, callback, scriptContext);
            }
            if (takesLimit)
            {
                remaining = ValidateLimitOrClose(thisObject, ArgOrUndefined(args, 1, scriptContext), scriptContext);
            }
            IterRecord record;
            GetIteratorDirect(record, thisObject, scriptContext);
            return scriptContext->GetLibrary()->CreateIteratorHelper(kind, record.iterator, record.nextMethod, callback, remaining);
        }
    } // namespace

    Var JavascriptIterator::EntryMap(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateLazyHelper(args, function->GetScriptContext(), IteratorHelperKind::Map, true, false);
    }

    Var JavascriptIterator::EntryFilter(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateLazyHelper(args, function->GetScriptContext(), IteratorHelperKind::Filter, true, false);
    }

    Var JavascriptIterator::EntryTake(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateLazyHelper(args, function->GetScriptContext(), IteratorHelperKind::Take, false, true);
    }

    Var JavascriptIterator::EntryDrop(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateLazyHelper(args, function->GetScriptContext(), IteratorHelperKind::Drop, false, true);
    }

    Var JavascriptIterator::EntryFlatMap(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateLazyHelper(args, function->GetScriptContext(), IteratorHelperKind::FlatMap, true, false);
    }

    namespace
    {
        void CallCallbackOrClose(IterRecord& record, ScriptContext* scriptContext, Var callback, Var arg1, Var arg2, Var arg3, uint argCount, Var* outResult)
        {
            try
            {
                *outResult = CallVar(scriptContext, callback, scriptContext->GetLibrary()->GetUndefined(), arg1, arg2, arg3, argCount);
            }
            catch (const JavascriptException& err)
            {
                JavascriptExceptionObject* exception = err.GetAndClear();
                CloseRecordSwallow(record, scriptContext);
                JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
            }
        }
    } // namespace

    Var JavascriptIterator::EntryReduce(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        RecyclableObject* thisObject = ValidateIteratorThis(args, scriptContext);
        Var reducer = ArgOrUndefined(args, 1, scriptContext);
        ValidateCallableOrClose(thisObject, reducer, scriptContext);
        IterRecord record;
        GetIteratorDirect(record, thisObject, scriptContext);

        Var accumulator = nullptr;
        double counter = 0;
        if (args.Info.Count > 2)
        {
            accumulator = args[2];
        }
        else
        {
            Var firstValue = nullptr;
            if (!StepValue(record, scriptContext, &firstValue))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }
            accumulator = firstValue;
            counter = 1;
        }

        Var value = nullptr;
        while (StepValue(record, scriptContext, &value))
        {
            Var result = nullptr;
            CallCallbackOrClose(record, scriptContext, reducer, accumulator, value, JavascriptNumber::ToVarWithCheck(counter, scriptContext), 3, &result);
            accumulator = result;
            counter++;
        }
        return accumulator;
    }

    Var JavascriptIterator::EntryToArray(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        RecyclableObject* thisObject = ValidateIteratorThis(args, scriptContext);
        IterRecord record;
        GetIteratorDirect(record, thisObject, scriptContext);

        JavascriptArray* array = scriptContext->GetLibrary()->CreateArray(0);
        uint32 index = 0;
        Var value = nullptr;
        while (StepValue(record, scriptContext, &value))
        {
            array->SetItem(index, value, PropertyOperation_None);
            index++;
        }
        return array;
    }

    Var JavascriptIterator::EntryForEach(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        RecyclableObject* thisObject = ValidateIteratorThis(args, scriptContext);
        Var callback = ArgOrUndefined(args, 1, scriptContext);
        ValidateCallableOrClose(thisObject, callback, scriptContext);
        IterRecord record;
        GetIteratorDirect(record, thisObject, scriptContext);

        double counter = 0;
        Var value = nullptr;
        while (StepValue(record, scriptContext, &value))
        {
            Var result = nullptr;
            CallCallbackOrClose(record, scriptContext, callback, value, JavascriptNumber::ToVarWithCheck(counter, scriptContext), nullptr, 2, &result);
            counter++;
        }
        return scriptContext->GetLibrary()->GetUndefined();
    }

    namespace
    {
        // Shared loop for some/every/find. Modes: 0=some 1=every 2=find.
        Var EntrySomeEveryFind(RecyclableObject* function, Arguments& args, int mode)
        {
            ScriptContext* scriptContext = function->GetScriptContext();
            JavascriptLibrary* library = scriptContext->GetLibrary();
            RecyclableObject* thisObject = ValidateIteratorThis(args, scriptContext);
            Var predicate = ArgOrUndefined(args, 1, scriptContext);
            ValidateCallableOrClose(thisObject, predicate, scriptContext);
            IterRecord record;
            GetIteratorDirect(record, thisObject, scriptContext);

            double counter = 0;
            Var value = nullptr;
            while (StepValue(record, scriptContext, &value))
            {
                Var result = nullptr;
                CallCallbackOrClose(record, scriptContext, predicate, value, JavascriptNumber::ToVarWithCheck(counter, scriptContext), nullptr, 2, &result);
                bool test = JavascriptConversion::ToBool(result, scriptContext);
                if (mode == 0 && test)
                {
                    CloseRecordNormal(record, scriptContext);
                    return library->GetTrue();
                }
                if (mode == 1 && !test)
                {
                    CloseRecordNormal(record, scriptContext);
                    return library->GetFalse();
                }
                if (mode == 2 && test)
                {
                    CloseRecordNormal(record, scriptContext);
                    return value;
                }
                counter++;
            }
            if (mode == 0)
            {
                return library->GetFalse();
            }
            if (mode == 1)
            {
                return library->GetTrue();
            }
            return library->GetUndefined();
        }
    } // namespace

    Var JavascriptIterator::EntrySome(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return EntrySomeEveryFind(function, args, 0);
    }

    Var JavascriptIterator::EntryEvery(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return EntrySomeEveryFind(function, args, 1);
    }

    Var JavascriptIterator::EntryFind(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return EntrySomeEveryFind(function, args, 2);
    }

    JavascriptIteratorHelper::JavascriptIteratorHelper(DynamicType* type, IteratorHelperKind kindValue, RecyclableObject* iteratorValue, Var nextMethodValue, Var callbackValue, double remainingValue)
        : DynamicObject(type),
          iterator(iteratorValue),
          nextMethod(nextMethodValue),
          callback(callbackValue),
          innerIterator(nullptr),
          innerNextMethod(nullptr),
          counter(0),
          remaining(remainingValue),
          kind(kindValue),
          iteratorDone(false),
          innerDone(false),
          hasInner(false),
          completed(false),
          executing(false),
          started(false)
    {
        Assert(type->GetTypeId() == TypeIds_IteratorHelper);
    }

    template <> bool VarIsImpl<JavascriptIteratorHelper>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_IteratorHelper;
    }

    bool JavascriptIteratorHelper::StepUnderlying(ScriptContext* scriptContext, Var* outValue)
    {
        IterRecord record;
        record.iterator = this->iterator;
        record.nextMethod = this->nextMethod;
        record.done = this->iteratorDone;
        bool hasValue = false;
        try
        {
            hasValue = StepValue(record, scriptContext, outValue);
        }
        catch (...)
        {
            this->iteratorDone = record.done;
            throw;
        }
        this->iteratorDone = record.done;
        return hasValue;
    }

    bool JavascriptIteratorHelper::StepInner(ScriptContext* scriptContext, Var* outValue)
    {
        IterRecord record;
        record.iterator = this->innerIterator;
        record.nextMethod = this->innerNextMethod;
        record.done = this->innerDone;
        bool hasValue = false;
        try
        {
            hasValue = StepValue(record, scriptContext, outValue);
        }
        catch (...)
        {
            this->innerDone = record.done;
            throw;
        }
        this->innerDone = record.done;
        return hasValue;
    }

    void JavascriptIteratorHelper::CloseUnderlyingNormal(ScriptContext* scriptContext)
    {
        IterRecord record;
        record.iterator = this->iterator;
        record.done = this->iteratorDone;
        try
        {
            CloseRecordNormal(record, scriptContext);
        }
        catch (...)
        {
            this->iteratorDone = record.done;
            throw;
        }
        this->iteratorDone = record.done;
    }

    void JavascriptIteratorHelper::CloseInnerNormal(ScriptContext* scriptContext)
    {
        if (!this->hasInner)
        {
            return;
        }
        IterRecord record;
        record.iterator = this->innerIterator;
        record.done = this->innerDone;
        try
        {
            CloseRecordNormal(record, scriptContext);
        }
        catch (...)
        {
            this->innerDone = record.done;
            throw;
        }
        this->innerDone = record.done;
    }

    void JavascriptIteratorHelper::CloseAllSwallow(ScriptContext* scriptContext)
    {
        if (this->hasInner)
        {
            IterRecord innerRecord;
            innerRecord.iterator = this->innerIterator;
            innerRecord.done = this->innerDone;
            CloseRecordSwallow(innerRecord, scriptContext);
            this->innerDone = innerRecord.done;
        }
        IterRecord record;
        record.iterator = this->iterator;
        record.done = this->iteratorDone;
        CloseRecordSwallow(record, scriptContext);
        this->iteratorDone = record.done;
    }

    bool JavascriptIteratorHelper::Advance(ScriptContext* scriptContext, Var* outValue)
    {
        JavascriptLibrary* library = scriptContext->GetLibrary();
        switch (this->kind)
        {
        case IteratorHelperKind::Map:
        {
            Var value = nullptr;
            if (!StepUnderlying(scriptContext, &value))
            {
                return false;
            }
            double currentCounter = this->counter;
            Var mapped = CallVar(scriptContext, this->callback, library->GetUndefined(), value, JavascriptNumber::ToVarWithCheck(currentCounter, scriptContext), nullptr, 2);
            this->counter = currentCounter + 1;
            *outValue = mapped;
            return true;
        }
        case IteratorHelperKind::Filter:
        {
            for (;;)
            {
                Var value = nullptr;
                if (!StepUnderlying(scriptContext, &value))
                {
                    return false;
                }
                double currentCounter = this->counter;
                Var result = CallVar(scriptContext, this->callback, library->GetUndefined(), value, JavascriptNumber::ToVarWithCheck(currentCounter, scriptContext), nullptr, 2);
                this->counter = currentCounter + 1;
                if (JavascriptConversion::ToBool(result, scriptContext))
                {
                    *outValue = value;
                    return true;
                }
            }
        }
        case IteratorHelperKind::Take:
        {
            if (this->remaining == 0)
            {
                CloseUnderlyingNormal(scriptContext);
                return false;
            }
            if (!JavascriptNumber::IsPosInf(this->remaining))
            {
                this->remaining--;
            }
            Var value = nullptr;
            if (!StepUnderlying(scriptContext, &value))
            {
                return false;
            }
            *outValue = value;
            return true;
        }
        case IteratorHelperKind::Drop:
        {
            while (this->remaining > 0)
            {
                if (!JavascriptNumber::IsPosInf(this->remaining))
                {
                    this->remaining--;
                }
                Var skipped = nullptr;
                if (!StepUnderlying(scriptContext, &skipped))
                {
                    return false;
                }
            }
            Var value = nullptr;
            if (!StepUnderlying(scriptContext, &value))
            {
                return false;
            }
            *outValue = value;
            return true;
        }
        case IteratorHelperKind::FlatMap:
        {
            for (;;)
            {
                if (this->hasInner)
                {
                    Var innerValue = nullptr;
                    if (StepInner(scriptContext, &innerValue))
                    {
                        *outValue = innerValue;
                        return true;
                    }
                    this->hasInner = false;
                    this->innerIterator = nullptr;
                    this->innerNextMethod = nullptr;
                    this->innerDone = false;
                    continue;
                }
                Var value = nullptr;
                if (!StepUnderlying(scriptContext, &value))
                {
                    return false;
                }
                double currentCounter = this->counter;
                Var mapped = CallVar(scriptContext, this->callback, library->GetUndefined(), value, JavascriptNumber::ToVarWithCheck(currentCounter, scriptContext), nullptr, 2);
                this->counter = currentCounter + 1;
                IterRecord innerRecord;
                GetIteratorFlattenable(innerRecord, mapped, true, scriptContext);
                this->innerIterator = innerRecord.iterator;
                this->innerNextMethod = innerRecord.nextMethod;
                this->innerDone = false;
                this->hasInner = true;
            }
        }
        case IteratorHelperKind::Wrap:
        default:
            AssertMsg(false, "Advance must not be called on a WrapForValidIterator object");
            return false;
        }
    }

    namespace
    {
        JavascriptIteratorHelper* ValidateHelperThis(Arguments& args, ScriptContext* scriptContext, bool wantWrap, const char16* apiName)
        {
            if (!JavascriptOperators::IsObject(args[0]) || JavascriptOperators::GetTypeId(args[0]) != TypeIds_IteratorHelper)
            {
                JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, apiName, wantWrap ? _u("WrapForValidIterator") : _u("Iterator Helper"));
            }
            JavascriptIteratorHelper* helper = UnsafeVarTo<JavascriptIteratorHelper>(args[0]);
            if (helper->IsWrap() != wantWrap)
            {
                JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, apiName, wantWrap ? _u("WrapForValidIterator") : _u("Iterator Helper"));
            }
            return helper;
        }
    } // namespace

    Var JavascriptIteratorHelper::EntryNext(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptIteratorHelper* helper = ValidateHelperThis(args, scriptContext, false, _u("Iterator Helper.prototype.next"));
        // Executing is checked before completed: a started close sets
        // executing, so reentry during that close still throws. A
        // suspended-start close sets only completed.
        if (helper->executing)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_GeneratorAlreadyExecuting, _u("Iterator Helper.prototype.next"));
        }
        if (helper->completed)
        {
            return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
        }

        // Leaving suspended-start. Reentry below observes executing.
        helper->started = true;
        helper->executing = true;
        Var value = nullptr;
        bool hasValue = false;
        try
        {
            hasValue = helper->Advance(scriptContext, &value);
        }
        catch (const JavascriptException& err)
        {
            JavascriptExceptionObject* exception = err.GetAndClear();
            helper->completed = true;
            try
            {
                helper->CloseAllSwallow(scriptContext);
            }
            catch (...)
            {
                helper->executing = false;
                throw;
            }
            helper->executing = false;
            JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
        }
        catch (...)
        {
            helper->executing = false;
            throw;
        }
        helper->executing = false;
        if (!hasValue)
        {
            helper->completed = true;
            return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
        }
        return library->CreateIteratorResultObject(value, library->GetFalse());
    }

    Var JavascriptIteratorHelper::EntryReturn(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptIteratorHelper* helper = ValidateHelperThis(args, scriptContext, false, _u("Iterator Helper.prototype.return"));
        // Executing is checked before completed. A started close sets
        // executing, so reentry during that close still throws. A
        // suspended-start close sets only completed, so reentry returns done.
        if (helper->executing)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_GeneratorAlreadyExecuting, _u("Iterator Helper.prototype.return"));
        }
        if (helper->completed)
        {
            return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
        }

        // suspended-start: [[GeneratorState]] becomes completed before
        // IteratorClose, and the generator is never resumed (not executing).
        if (!helper->started)
        {
            helper->completed = true;
            try
            {
                helper->CloseInnerNormal(scriptContext);
                helper->CloseUnderlyingNormal(scriptContext);
            }
            catch (const JavascriptException& err)
            {
                JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
            }
            return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
        }

        helper->executing = true;
        helper->completed = true;
        try
        {
            helper->CloseInnerNormal(scriptContext);
            helper->CloseUnderlyingNormal(scriptContext);
        }
        catch (const JavascriptException& err)
        {
            JavascriptExceptionObject* exception = err.GetAndClear();
            try
            {
                helper->CloseAllSwallow(scriptContext);
            }
            catch (...)
            {
                helper->executing = false;
                throw;
            }
            helper->executing = false;
            JavascriptExceptionOperators::DoThrowCheckClone(exception, scriptContext);
        }
        catch (...)
        {
            helper->executing = false;
            throw;
        }
        helper->executing = false;
        return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
    }

    Var JavascriptIteratorHelper::EntryWrapNext(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        Assert(!(callInfo.Flags & CallFlags_New));

        // Pure forward. Spec step is Return ? Call(nextMethod, iterator):
        // the underlying result is returned unchanged, including a
        // non-object, and "done" is not read.
        JavascriptIteratorHelper* helper = ValidateHelperThis(args, scriptContext, true, _u("WrapForValidIterator.prototype.next"));
        return CallVar(scriptContext, helper->nextMethod, helper->iterator);
    }

    Var JavascriptIteratorHelper::EntryWrapReturn(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        Assert(!(callInfo.Flags & CallFlags_New));

        JavascriptIteratorHelper* helper = ValidateHelperThis(args, scriptContext, true, _u("WrapForValidIterator.prototype.return"));
        helper->iteratorDone = true;
        Var returnMethod = JavascriptOperators::GetProperty(helper->iterator, PropertyIds::return_, scriptContext);
        if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
        {
            return library->CreateIteratorResultObject(library->GetUndefined(), library->GetTrue());
        }
        if (!JavascriptConversion::IsCallable(returnMethod))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        return CallVar(scriptContext, returnMethod, helper->iterator);
    }
} //namespace Js
