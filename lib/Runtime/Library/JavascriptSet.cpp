//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{
JavascriptSet::JavascriptSet(DynamicType* type)
    : DynamicObject(type)
{
}

JavascriptSet* JavascriptSet::New(ScriptContext* scriptContext)
{
    JavascriptSet* set = scriptContext->GetLibrary()->CreateSet();

    return set;
}

JavascriptSet::SetDataList::Iterator JavascriptSet::GetIterator()
{
    return this->list.GetIterator();
}

Var JavascriptSet::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    JavascriptLibrary* library = scriptContext->GetLibrary();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set"));

    Var newTarget = args.GetNewTarget();
    bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);
    CHAKRATEL_LANGSTATS_INC_LANGFEATURECOUNT(ES6, Set, scriptContext);

    JavascriptSet* setObject = nullptr;

    if (callInfo.Flags & CallFlags_New)
    {
        setObject = library->CreateSet();
    }
    else
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set"), _u("Set"));
    }
    Assert(setObject != nullptr);

    Var iterable = (args.Info.Count > 1) ? args[1] : library->GetUndefined();

    // REVIEW: This condition seems impossible?
    if (setObject->kind != SetKind::EmptySet)
    {
        Assert(UNREACHED);
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_ObjectIsAlreadyInitialized, _u("Set"), _u("Set"));
    }

    RecyclableObject* iter = nullptr;
    RecyclableObject* adder = nullptr;

    if (JavascriptConversion::CheckObjectCoercible(iterable, scriptContext))
    {
        iter = JavascriptOperators::GetIterator(iterable, scriptContext);
        Var adderVar = JavascriptOperators::GetPropertyNoCache(setObject, PropertyIds::add, scriptContext);
        if (!JavascriptConversion::IsCallable(adderVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        adder = VarTo<RecyclableObject>(adderVar);
    }

    if (iter != nullptr)
    {
        JavascriptOperators::DoIteratorStepAndValue(iter, scriptContext, [&](Var nextItem) {
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                CALL_FUNCTION(scriptContext->GetThreadContext(), adder, CallInfo(CallFlags_Value, 2), setObject, nextItem);
            }
            END_SAFE_REENTRANT_CALL
        });
    }

    return isCtorSuperCall ?
        JavascriptOperators::OrdinaryCreateFromConstructor(VarTo<RecyclableObject>(newTarget), setObject, nullptr, scriptContext) :
        setObject;
}

Var JavascriptSet::EntryAdd(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.add"), _u("Set"));
    }

    Var value = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();

    if (JavascriptNumber::Is(value) && JavascriptNumber::IsNegZero(JavascriptNumber::GetValue(value)))
    {
        // Normalize -0 to +0
        value = JavascriptNumber::New(0.0, scriptContext);
    }

    set->Add(value);

    return set;
}

Var JavascriptSet::EntryClear(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.clear"), _u("Set"));
    }

    set->Clear();

    return scriptContext->GetLibrary()->GetUndefined();
}

Var JavascriptSet::EntryDelete(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.delete"), _u("Set"));
    }

    Var value = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();

    bool didDelete = set->Delete(value);

    return scriptContext->GetLibrary()->CreateBoolean(didDelete);
}

Var JavascriptSet::EntryForEach(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.forEach"));

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.forEach"), _u("Set"));
    }

    if (args.Info.Count < 2 || !JavascriptConversion::IsCallable(args[1]))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, _u("Set.prototype.forEach"));
    }
    RecyclableObject* callBackFn = VarTo<RecyclableObject>(args[1]);

    Var thisArg = (args.Info.Count > 2) ? args[2] : scriptContext->GetLibrary()->GetUndefined();

    auto iterator = set->GetIterator();

    while (iterator.Next())
    {
        Var value = iterator.Current();

        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            CALL_FUNCTION(scriptContext->GetThreadContext(), callBackFn, CallInfo(CallFlags_Value, 4), thisArg, value, value, args[0]);
        }
        END_SAFE_REENTRANT_CALL
    }

    return scriptContext->GetLibrary()->GetUndefined();
}

Var JavascriptSet::EntryHas(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.has"), _u("Set"));
    }

    Var value = (args.Info.Count > 1) ? args[1] : scriptContext->GetLibrary()->GetUndefined();

    bool hasValue = set->Has(value);

    return scriptContext->GetLibrary()->CreateBoolean(hasValue);
}

Var JavascriptSet::EntrySizeGetter(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.size"), _u("Set"));
    }

    int size = set->Size();

    return JavascriptNumber::ToVar(size, scriptContext);
}

Var JavascriptSet::EntryEntries(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.entries"), _u("Set"));
    }

    return scriptContext->GetLibrary()->CreateSetIterator(set, JavascriptSetIteratorKind::KeyAndValue);
}

Var JavascriptSet::EntryValues(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();

    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(args[0]);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, _u("Set.prototype.values"), _u("Set"));
    }

    return scriptContext->GetLibrary()->CreateSetIterator(set, JavascriptSetIteratorKind::Value);
}

Var JavascriptSet::EntryGetterSymbolSpecies(RecyclableObject* function, CallInfo callInfo, ...)
{
    ARGUMENTS(args, callInfo);

    Assert(args.Info.Count > 0);

    return args[0];
}

// ---- Set.prototype set methods (rest of S10) ----
// union, intersection, difference, symmetricDifference, isSubsetOf,
// isSupersetOf, and isDisjointFrom. They read the receiver's internal set
// data only; they never call add, has, size, or keys on the receiver, and
// result sets are plain new Sets with no constructor or species lookup.
// The other argument is consumed through a Set-like record
// (sec-getsetrecord); its has and keys ARE user-visible and run in the
// recorded order.

// A Set Record (sec-setrecords): stack-allocated, every member is a
// recycler pointer the GC traces through the caller's frames. [[Size]] is
// integer-truncated, non-negative, and may be +Infinity.
struct SetLikeRecord
{
    RecyclableObject* setLike;
    double size;
    RecyclableObject* has;
    RecyclableObject* keys;
};

// GetIteratorFromMethod state for the record's [[Keys]]: the iterator
// object and the "next" method read once when the iterator was created.
struct SetLikeKeysIterator
{
    RecyclableObject* iterator;
    RecyclableObject* next;
};

// Get(obj, id) where the property may be a user getter.
static Var SetMethodGetProperty(RecyclableObject* object, PropertyId propertyId, ScriptContext* scriptContext)
{
    JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
    Var value = nullptr;
    JS_REENTRANT(jsReentLock, value = JavascriptOperators::GetPropertyNoCache(object, propertyId, scriptContext));
    return value;
}

// Call record.[[Has]] with record.[[SetObject]] as the receiver and
// ToBoolean the result. The receiver brand check mirrors the other entries.
static JavascriptSet* SetMethodCheckReceiver(Var thisArg, const char16* methodName, ScriptContext* scriptContext)
{
    JavascriptSet* set = JavascriptOperators::TryFromVar<JavascriptSet>(thisArg);
    if (set == nullptr)
    {
        JavascriptError::ThrowTypeErrorVar(scriptContext, JSERR_NeedObjectOfType, methodName, _u("Set"));
    }
    return set;
}

// sec-getsetrecord. The observable order is: the Object check, Get "size",
// ToNumber, the NaN TypeError, the integer truncation, the negative-size
// RangeError, Get "has" with its callable check, then Get "keys" with its
// callable check.
static SetLikeRecord GetSetLikeRecord(Var other, const char16* methodName, ScriptContext* scriptContext)
{
    if (!JavascriptOperators::IsObject(other))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
    }

    SetLikeRecord record;
    record.setLike = UnsafeVarTo<RecyclableObject>(other);
    record.size = 0;
    record.has = nullptr;
    record.keys = nullptr;

    Var rawSize = SetMethodGetProperty(record.setLike, PropertyIds::size, scriptContext);

    double numberSize = 0;
    {
        JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
        JS_REENTRANT(jsReentLock, numberSize = JavascriptConversion::ToNumber(rawSize, scriptContext));
    }
    if (JavascriptNumber::IsNan(numberSize))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, methodName);
    }
    // ToIntegerOrInfinity of the already-converted number. No user code runs here.
    double intSize = JavascriptConversion::ToInteger(numberSize);
    if (intSize < 0)
    {
        JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, methodName);
    }
    record.size = intSize;

    Var has = SetMethodGetProperty(record.setLike, PropertyIds::has, scriptContext);
    if (!JavascriptConversion::IsCallable(has))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, methodName);
    }
    record.has = VarTo<RecyclableObject>(has);

    Var keys = SetMethodGetProperty(record.setLike, PropertyIds::keys, scriptContext);
    if (!JavascriptConversion::IsCallable(keys))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_NeedFunction, methodName);
    }
    record.keys = VarTo<RecyclableObject>(keys);

    return record;
}

// Call record.[[Has]] with record.[[SetObject]] as the receiver.
static bool SetMethodCallHas(const SetLikeRecord& record, Var value, ScriptContext* scriptContext)
{
    JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
    Var result = nullptr;
    // The C++->JS call must be armed for JIT-compiled callers too (the
    // interpreter's OP_Call region does not extend into a builtin re-entry).
    JS_REENTRANT(jsReentLock,
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            result = CALL_FUNCTION(scriptContext->GetThreadContext(), record.has, CallInfo(CallFlags_Value, 2), record.setLike, value);
        }
        END_SAFE_REENTRANT_CALL);
    return JavascriptConversion::ToBool(result, scriptContext);
}

// GetIteratorFromMethod over record.[[Keys]]: call keys, require an Object
// result, then read "next" once and require it callable.
static SetLikeKeysIterator GetSetLikeKeysIterator(const SetLikeRecord& record, ScriptContext* scriptContext)
{
    JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
    Var iteratorVar = nullptr;
    // Arm the keys() call itself; the "next" Get below routes through
    // ExecuteImplicitCall, which arms itself.
    JS_REENTRANT(jsReentLock,
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            iteratorVar = CALL_FUNCTION(scriptContext->GetThreadContext(), record.keys, CallInfo(CallFlags_Value, 1), record.setLike);
        }
        END_SAFE_REENTRANT_CALL);
    if (!JavascriptOperators::IsObject(iteratorVar))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
    }

    SetLikeKeysIterator keysIterator;
    keysIterator.iterator = VarTo<RecyclableObject>(iteratorVar);
    JS_REENTRANT(jsReentLock, keysIterator.next = JavascriptOperators::CacheIteratorNext(keysIterator.iterator, scriptContext));
    return keysIterator;
}

// IteratorStepValue: call next, require an Object result, Get "done", and
// only then Get "value". False means the iterator is exhausted.
static bool SetLikeKeysStep(const SetLikeKeysIterator& keysIterator, Var* valueOut, ScriptContext* scriptContext)
{
    JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
    bool hasValue = false;
    // IteratorStepAndValue's user transitions (next, done, value) each arm
    // themselves through ExecuteImplicitCall today; this wrapper arm is
    // defense-in-depth so a future direct-call fast path inside the iterator
    // helpers (see the CONSIDER there) stays armed for Set-method callers.
    JS_REENTRANT(jsReentLock,
        BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
        {
            hasValue = JavascriptOperators::IteratorStepAndValue(keysIterator.iterator, scriptContext, keysIterator.next, valueOut);
        }
        END_SAFE_REENTRANT_CALL);
    return hasValue;
}

// IteratorClose (sec-iteratorclose) reached on a normal completion. The
// shared JavascriptOperators::IteratorClose swallows throws because its
// callers arrive there on an abrupt completion; here the "return" Get, the
// Call, and the object check on the result are all observable and must
// propagate per spec.
static void CloseKeysIteratorNormalCompletion(const SetLikeKeysIterator& keysIterator, ScriptContext* scriptContext)
{
    Var returnMethod = SetMethodGetProperty(keysIterator.iterator, PropertyIds::return_, scriptContext);
    if (JavascriptOperators::IsUndefinedOrNull(returnMethod))
    {
        return;
    }
    if (!JavascriptConversion::IsCallable(returnMethod))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction, _u("return"));
    }

    RecyclableObject* callable = VarTo<RecyclableObject>(returnMethod);
    Var result = nullptr;
    {
        JS_REENTRANCY_LOCK(jsReentLock, scriptContext->GetThreadContext());
        JS_REENTRANT(jsReentLock,
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                result = CALL_FUNCTION(scriptContext->GetThreadContext(), callable, CallInfo(CallFlags_Value, 1), keysIterator.iterator);
            }
            END_SAFE_REENTRANT_CALL);
    }
    if (!JavascriptOperators::IsObject(result))
    {
        JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
    }
}

// CanonicalizeKeyedCollectionKey for Sets: -0 becomes +0. Values taken from
// the other argument's iterator pass through here before any Has, Add, or
// Delete on a result set. Values already stored in a Set are canonical.
static Var CanonicalizeSetMethodKey(Var value, ScriptContext* scriptContext)
{
    if (JavascriptNumber::Is(value) && JavascriptNumber::IsNegZero(JavascriptNumber::GetValue(value)))
    {
        return JavascriptNumber::New(0.0, scriptContext);
    }
    return value;
}

// A fresh plain Set holding the receiver's current elements, in order. This
// is the "copy of set.[[SetData]]" the spec builds after GetSetRecord.
static JavascriptSet* CopySetData(JavascriptSet* set, ScriptContext* scriptContext)
{
    JavascriptSet* result = scriptContext->GetLibrary()->CreateSet();
    auto iter = set->GetIterator();
    while (iter.Next())
    {
        result->Add(iter.Current());
    }
    return result;
}

// Set.prototype.union. The keys iterator is created before the receiver's
// data is copied, and each iterated value is appended to the copy when it is
// not already there. other.has is never consulted.
Var JavascriptSet::EntryUnion(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.union"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.union"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.union"), scriptContext);

    SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);

    JavascriptSet* result = CopySetData(set, scriptContext);
    Var value = nullptr;
    while (SetLikeKeysStep(keysIterator, &value, scriptContext))
    {
        value = CanonicalizeSetMethodKey(value, scriptContext);
        if (!result->Has(value))
        {
            result->Add(value);
        }
    }
    return result;
}

// Set.prototype.intersection. When the receiver is not larger than the
// record, the receiver's live data is walked and each entry is passed to
// other.has; entries deleted by has are skipped, re-added and newly added
// entries are visited, and the result dedupes. Otherwise other's keys are
// iterated and each value kept when the live receiver contains it.
Var JavascriptSet::EntryIntersection(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.intersection"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.intersection"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.intersection"), scriptContext);

    JavascriptSet* result = scriptContext->GetLibrary()->CreateSet();
    if (static_cast<double>(set->Size()) <= record.size)
    {
        auto iter = set->GetIterator();
        while (iter.Next())
        {
            Var entry = iter.Current();
            if (SetMethodCallHas(record, entry, scriptContext) && !result->Has(entry))
            {
                result->Add(entry);
            }
        }
    }
    else
    {
        SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);
        Var value = nullptr;
        while (SetLikeKeysStep(keysIterator, &value, scriptContext))
        {
            value = CanonicalizeSetMethodKey(value, scriptContext);
            if (set->Has(value) && !result->Has(value))
            {
                result->Add(value);
            }
        }
    }
    return result;
}

// Set.prototype.difference. The copy is taken before any branch; when the
// receiver is not larger than the record, the copy itself is walked with
// other.has and matching entries are deleted from the copy (a fixed-extent
// walk: entries has() adds to the receiver are not visited). Otherwise
// other's keys are iterated and each value deleted from the copy when the
// copy still contains it.
Var JavascriptSet::EntryDifference(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.difference"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.difference"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.difference"), scriptContext);

    JavascriptSet* result = CopySetData(set, scriptContext);
    if (static_cast<double>(set->Size()) <= record.size)
    {
        auto iter = result->GetIterator();
        while (iter.Next())
        {
            Var entry = iter.Current();
            if (SetMethodCallHas(record, entry, scriptContext))
            {
                result->Delete(entry);
            }
        }
    }
    else
    {
        SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);
        Var value = nullptr;
        while (SetLikeKeysStep(keysIterator, &value, scriptContext))
        {
            value = CanonicalizeSetMethodKey(value, scriptContext);
            // Keys-walk: membership is checked against the private copy only —
            // no other.has call and no live-receiver consult (unlike
            // intersection's keys-walk, which checks the live receiver).
            // Both asymmetries are per the ES2025 algorithm: record keys are
            // simply removed from the copy when it still contains them.
            if (result->Has(value))
            {
                result->Delete(value);
            }
        }
    }
    return result;
}

// Set.prototype.symmetricDifference. The keys iterator is created before the
// receiver's data is copied. Each iterated value is removed from the copy
// when the live receiver still contains it and the copy does too; it is
// appended when the live receiver does not contain it and neither does the
// copy.
Var JavascriptSet::EntrySymmetricDifference(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.symmetricDifference"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.symmetricDifference"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.symmetricDifference"), scriptContext);

    SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);

    JavascriptSet* result = CopySetData(set, scriptContext);
    Var value = nullptr;
    while (SetLikeKeysStep(keysIterator, &value, scriptContext))
    {
        value = CanonicalizeSetMethodKey(value, scriptContext);
        bool alreadyInResult = result->Has(value);
        if (set->Has(value))
        {
            if (alreadyInResult)
            {
                result->Delete(value);
            }
        }
        else if (!alreadyInResult)
        {
            result->Add(value);
        }
    }
    return result;
}

// Set.prototype.isSubsetOf. A receiver larger than the record is an
// immediate false. Otherwise the receiver's live data is walked and each
// entry is passed to other.has; the first miss is an early false. The
// record's keys are never read.
Var JavascriptSet::EntryIsSubsetOf(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.isSubsetOf"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.isSubsetOf"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.isSubsetOf"), scriptContext);

    if (static_cast<double>(set->Size()) > record.size)
    {
        return scriptContext->GetLibrary()->CreateBoolean(false);
    }

    auto iter = set->GetIterator();
    while (iter.Next())
    {
        if (!SetMethodCallHas(record, iter.Current(), scriptContext))
        {
            return scriptContext->GetLibrary()->CreateBoolean(false);
        }
    }
    return scriptContext->GetLibrary()->CreateBoolean(true);
}

// Set.prototype.isSupersetOf. A receiver smaller than the record is an
// immediate false. Otherwise other's keys are iterated and each value is
// checked against the live receiver's data; the first miss closes the
// iterator and returns false. other.has is never consulted.
Var JavascriptSet::EntryIsSupersetOf(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.isSupersetOf"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.isSupersetOf"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.isSupersetOf"), scriptContext);

    if (static_cast<double>(set->Size()) < record.size)
    {
        return scriptContext->GetLibrary()->CreateBoolean(false);
    }

    SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);
    Var value = nullptr;
    while (SetLikeKeysStep(keysIterator, &value, scriptContext))
    {
        if (!set->Has(value))
        {
            CloseKeysIteratorNormalCompletion(keysIterator, scriptContext);
            return scriptContext->GetLibrary()->CreateBoolean(false);
        }
    }
    return scriptContext->GetLibrary()->CreateBoolean(true);
}

// Set.prototype.isDisjointFrom. When the receiver is not larger than the
// record, the receiver's live data is walked and each entry is passed to
// other.has; the first hit is an early false. Otherwise other's keys are
// iterated and each value is checked against the live receiver's data; the
// first hit closes the iterator and returns false.
Var JavascriptSet::EntryIsDisjointFrom(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

    ARGUMENTS(args, callInfo);
    ScriptContext* scriptContext = function->GetScriptContext();
    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Set.prototype.isDisjointFrom"));

    Assert(!(callInfo.Flags & CallFlags_New));

    JavascriptSet* set = SetMethodCheckReceiver(args[0], _u("Set.prototype.isDisjointFrom"), scriptContext);
    Var other = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
    SetLikeRecord record = GetSetLikeRecord(other, _u("Set.prototype.isDisjointFrom"), scriptContext);

    if (static_cast<double>(set->Size()) <= record.size)
    {
        auto iter = set->GetIterator();
        while (iter.Next())
        {
            if (SetMethodCallHas(record, iter.Current(), scriptContext))
            {
                return scriptContext->GetLibrary()->CreateBoolean(false);
            }
        }
        return scriptContext->GetLibrary()->CreateBoolean(true);
    }

    SetLikeKeysIterator keysIterator = GetSetLikeKeysIterator(record, scriptContext);
    Var value = nullptr;
    while (SetLikeKeysStep(keysIterator, &value, scriptContext))
    {
        if (set->Has(value))
        {
            CloseKeysIteratorNormalCompletion(keysIterator, scriptContext);
            return scriptContext->GetLibrary()->CreateBoolean(false);
        }
    }
    return scriptContext->GetLibrary()->CreateBoolean(true);
}


template <typename T>
T*
JavascriptSet::CreateVarSetFromList(uint initialCapacity)
{
    T* varSet = RecyclerNew(this->GetRecycler(), T, this->GetRecycler(), initialCapacity);

    JavascriptSet::SetDataList::Iterator iter = this->list.GetIterator();
    // TODO: we can use a more efficient Iterator, since we know there will be no side effects
    while (iter.Next())
    {
        varSet->Add(iter.Current(), iter.CurrentNode());
    }
    return varSet;
}

void
JavascriptSet::PromoteToSimpleVarSet()
{
    AssertOrFailFast(this->kind == SetKind::IntSet);
    SimpleVarDataSet* newSet = this->CreateVarSetFromList<SimpleVarDataSet>(this->u.intSet->Count() + 1);

    this->kind = SetKind::SimpleVarSet;
    this->u.simpleVarSet = newSet;
}

void
JavascriptSet::PromoteToComplexVarSet()
{
    uint setSize = 0;
    if (this->kind == SetKind::IntSet)
    {
        setSize = this->u.intSet->Count();
    }
    else
    {
        AssertOrFailFast(this->kind == SetKind::SimpleVarSet);
        setSize = this->u.simpleVarSet->Count();
    }
    ComplexVarDataSet* newSet = this->CreateVarSetFromList<ComplexVarDataSet>(setSize + 1);

    this->kind = SetKind::ComplexVarSet;
    this->u.complexVarSet = newSet;
}

void
JavascriptSet::AddToEmptySet(Var value)
{
    Assert(this->kind == SetKind::EmptySet);
    // We cannot store an int with value -1 inside of a bit vector
    Var taggedInt = JavascriptConversion::TryCanonicalizeAsTaggedInt<false /* allowNegOne */, false /* allowLossyConversion */>(value);
    if (taggedInt)
    {
        int32 intVal = TaggedInt::ToInt32(taggedInt);

        BVSparse<Recycler>* newIntSet = RecyclerNew(this->GetRecycler(), BVSparse<Recycler>, this->GetRecycler());
        newIntSet->Set(intVal);

        this->list.Append(taggedInt, this->GetRecycler());

        this->u.intSet = newIntSet;
        this->kind = SetKind::IntSet;
        return;
    }

    Var simpleVar = JavascriptConversion::TryCanonicalizeAsSimpleVar<false /* allowLossyConversion */>(value);
    if (simpleVar)
    {
        SimpleVarDataSet* newSimpleSet = RecyclerNew(this->GetRecycler(), SimpleVarDataSet, this->GetRecycler());
        SetDataNode* node = this->list.Append(simpleVar, this->GetRecycler());

        newSimpleSet->Add(simpleVar, node);

        this->u.simpleVarSet = newSimpleSet;
        this->kind = SetKind::SimpleVarSet;
        return;
    }

    ComplexVarDataSet* newComplexSet = RecyclerNew(this->GetRecycler(), ComplexVarDataSet, this->GetRecycler());
    SetDataNode* node = this->list.Append(value, this->GetRecycler());

    newComplexSet->Add(value, node);

    this->u.complexVarSet = newComplexSet;
    this->kind = SetKind::ComplexVarSet;
}

bool
JavascriptSet::TryAddToIntSet(Var value)
{
    Assert(this->kind == SetKind::IntSet);
    Var taggedInt = JavascriptConversion::TryCanonicalizeAsTaggedInt<false /* allowNegOne */, false /* allowLossyConversion */>(value);
    if (!taggedInt)
    {
        return false;
    }

    int32 intVal = TaggedInt::ToInt32(taggedInt);
    if (!this->u.intSet->TestAndSet(intVal))
    {
        this->list.Append(taggedInt, this->GetRecycler());
    }
    return true;
}

bool
JavascriptSet::TryAddToSimpleVarSet(Var value)
{
    Assert(this->kind == SetKind::SimpleVarSet);
    Var simpleVar = JavascriptConversion::TryCanonicalizeAsSimpleVar<false /* allowLossyConversion */>(value);
    if (!simpleVar)
    {
        return false;
    }

    if (!this->u.simpleVarSet->ContainsKey(simpleVar))
    {
        SetDataNode* node = this->list.Append(simpleVar, this->GetRecycler());
        this->u.simpleVarSet->Add(simpleVar, node);
    }

    return true;
}

void
JavascriptSet::AddToComplexVarSet(Var value)
{
    Assert(this->kind == SetKind::ComplexVarSet);
    if (!this->u.complexVarSet->ContainsKey(value))
    {
        SetDataNode* node = this->list.Append(value, this->GetRecycler());
        this->u.complexVarSet->Add(value, node);
    }
}

void
JavascriptSet::Add(Var value)
{
    JS_REENTRANCY_LOCK(jsReentLock, this->GetScriptContext()->GetThreadContext());
    switch (this->kind)
    {
    case SetKind::EmptySet:
    {
        this->AddToEmptySet(value);
        return;
    }

    case SetKind::IntSet:
    {
        if (this->TryAddToIntSet(value))
        {
            return;
        }

        Var simpleVar = JavascriptConversion::TryCanonicalizeAsSimpleVar<false /* allowLossyConversion */>(value);
        if (simpleVar)
        {
            this->PromoteToSimpleVarSet();
            this->Add(simpleVar);
            return;
        }

        this->PromoteToComplexVarSet();
        this->Add(value);
        return;
    }

    case SetKind::SimpleVarSet:
        if (this->TryAddToSimpleVarSet(value))
        {
            return;
        }

        this->PromoteToComplexVarSet();
        this->Add(value);
        return;

    case SetKind::ComplexVarSet:
        this->AddToComplexVarSet(value);
        return;

    default:
        Assume(UNREACHED);
    }
}

void JavascriptSet::Clear()
{
    JS_REENTRANCY_LOCK(jsReentLock, this->GetScriptContext()->GetThreadContext());
    this->list.Clear();
    switch (this->kind)
    {
    case SetKind::EmptySet:
        return;
    case SetKind::IntSet:
        this->u.intSet->ClearAll();
        return;
    case SetKind::SimpleVarSet:
        this->u.simpleVarSet->Clear();
        return;
    case SetKind::ComplexVarSet:
        this->u.complexVarSet->Clear();
        return;
    default:
        Assume(UNREACHED);
    }
}

bool
JavascriptSet::IsInIntSet(Var value)
{
    Assert(this->kind == SetKind::IntSet);
    Var taggedInt = JavascriptConversion::TryCanonicalizeAsTaggedInt<false /* allowNegOne */, true /* allowLossyConversion */>(value);
    if (!taggedInt)
    {
        return false;
    }
    int32 intVal = TaggedInt::ToInt32(taggedInt);
    return this->u.intSet->Test(intVal);
}

template <bool isComplex>
bool
JavascriptSet::DeleteFromVarSet(Var value)
{
    Assert(this->kind == (isComplex ? SetKind::ComplexVarSet : SetKind::SimpleVarSet));
    SetDataNode * node = nullptr;
    if (isComplex
        ? !this->u.complexVarSet->TryGetValueAndRemove(value, &node)
        : !this->u.simpleVarSet->TryGetValueAndRemove(value, &node))
    {
        return false;
    }

    this->list.Remove(node);
    return true;
}

bool
JavascriptSet::DeleteFromSimpleVarSet(Var value)
{
    Assert(this->kind == SetKind::SimpleVarSet);
    Var simpleVar = JavascriptConversion::TryCanonicalizeAsSimpleVar<true /* allowLossyConversion */>(value);
    if (!simpleVar)
    {
        return false;
    }

    return this->DeleteFromVarSet<false>(simpleVar);
}

bool JavascriptSet::Delete(Var value)
{
    JS_REENTRANCY_LOCK(jsReentLock, this->GetScriptContext()->GetThreadContext());
    switch (this->kind)
    {
    case SetKind::EmptySet:
        return false;

    case SetKind::IntSet:
        if (!IsInIntSet(value))
        {
            return false;
        }
        // We don't have the list node pointer readily available, so deletion from int sets would require walking the list
        // Because of this, let's just promote to a var set
        //
        // If this promotion becomes an issue, we can consider options to improve this, e.g. deferring until an iterator is requested
        this->PromoteToSimpleVarSet();
        return this->Delete(value);

    case SetKind::SimpleVarSet:
        return this->DeleteFromSimpleVarSet(value);

    case SetKind::ComplexVarSet:
        return this->DeleteFromVarSet<true /* isComplex */>(value);

    default:
        Assume(UNREACHED);
        return false;
    }
}

bool JavascriptSet::Has(Var value)
{
    JS_REENTRANCY_LOCK(jsReentLock, this->GetScriptContext()->GetThreadContext());
    switch (this->kind)
    {
    case SetKind::EmptySet:
        return false;

    case SetKind::IntSet:
    {
        Var taggedInt = JavascriptConversion::TryCanonicalizeAsTaggedInt<false /* allowNegOne */, true /* allowLossyConversion */ >(value);
        if (!taggedInt)
        {
            return false;
        }
        int32 intVal = TaggedInt::ToInt32(taggedInt);
        return this->u.intSet->Test(intVal);
    }

    case SetKind::SimpleVarSet:
    {
        // First check if the value is in the set
        if (this->u.simpleVarSet->ContainsKey(value))
        {
            return true;
        }
        // If the value isn't in the set, check if the canonical value is
        Var simpleVar = JavascriptConversion::TryCanonicalizeAsSimpleVar<true /* allowLossyConversion */>(value);
        // If the simple value is the same as the original value, we know it isn't in the set
        if (!simpleVar || simpleVar == value)
        {
            return false;
        }

        return this->u.simpleVarSet->ContainsKey(simpleVar);
    }

    case SetKind::ComplexVarSet:
        return this->u.complexVarSet->ContainsKey(value);

    default:
        Assume(UNREACHED);
        return false;
    }
}

int JavascriptSet::Size()
{
    JS_REENTRANCY_LOCK(jsReentLock, this->GetScriptContext()->GetThreadContext());
    switch (this->kind)
    {
    case SetKind::EmptySet:
        return 0;
    case SetKind::IntSet:
        return this->u.intSet->Count();
    case SetKind::SimpleVarSet:
        return this->u.simpleVarSet->Count();
    case SetKind::ComplexVarSet:
        return this->u.complexVarSet->Count();
    default:
        Assume(UNREACHED);
        return 0;
    }
}

BOOL JavascriptSet::GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext)
{
    stringBuilder->AppendCppLiteral(_u("Set"));
    return TRUE;
}

#if ENABLE_TTD
void JavascriptSet::MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor)
{
    auto iterator = this->GetIterator();
    while(iterator.Next())
    {
        extractor->MarkVisitVar(iterator.Current());
    }
}

TTD::NSSnapObjects::SnapObjectType JavascriptSet::GetSnapTag_TTD() const
{
    return TTD::NSSnapObjects::SnapObjectType::SnapSetObject;
}

void JavascriptSet::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
{
    TTD::NSSnapObjects::SnapSetInfo* ssi = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapSetInfo>();
    ssi->SetSize = 0;

    if(this->Size() == 0)
    {
        ssi->SetValueArray = nullptr;
    }
    else
    {
        ssi->SetValueArray = alloc.SlabAllocateArray<TTD::TTDVar>(this->Size());

        auto iter = this->GetIterator();
        while(iter.Next())
        {
            ssi->SetValueArray[ssi->SetSize] = iter.Current();
            ssi->SetSize++;
        }
    }

    TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapSetInfo*, TTD::NSSnapObjects::SnapObjectType::SnapSetObject>(objData, ssi);
}

JavascriptSet* JavascriptSet::CreateForSnapshotRestore(ScriptContext* ctx)
{
    JavascriptSet* res = ctx->GetLibrary()->CreateSet();

    return res;
}
#endif
}
