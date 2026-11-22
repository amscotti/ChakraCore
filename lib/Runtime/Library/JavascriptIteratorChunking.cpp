//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/JavascriptIteratorChunking.h"

namespace Js
{
namespace
{
    Var CallMethodNoArgs(ScriptContext* scriptContext, Var funcVar, Var thisVar)
    {
        if (!JavascriptConversion::IsCallable(funcVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedFunction);
        }
        RecyclableObject* callable = VarTo<RecyclableObject>(funcVar);
        Var argsArr[1] = { thisVar };
        CallInfo callInfo(CallFlags_Value, 1);
        return scriptContext->GetThreadContext()->ExecuteImplicitCall(callable, ImplicitCall_Accessor, [&]() -> Var
        {
            return JavascriptFunction::CallFunction<true>(callable, callable->GetEntryPoint(), Arguments(callInfo, argsArr));
        });
    }

    // IteratorClose with an abrupt validation error: consult "return" once
    // and swallow any error from reading or calling "return" so the original
    // TypeError/RangeError propagates.
    void CloseIteratorSwallow(RecyclableObject* iterator, ScriptContext* scriptContext)
    {
        if (iterator == nullptr)
        {
            return;
        }
        try
        {
            Var returnMethod = JavascriptOperators::GetProperty(iterator, PropertyIds::return_, scriptContext);
            if (JavascriptOperators::IsUndefinedOrNull(returnMethod) || !JavascriptConversion::IsCallable(returnMethod))
            {
                return;
            }
            CallMethodNoArgs(scriptContext, returnMethod, iterator);
        }
        catch (const JavascriptException& err)
        {
            err.GetAndClear();
        }
    }

    uint32 ValidateChunkSizeOrClose(RecyclableObject* thisObject, Var sizeVar, ScriptContext* scriptContext, const char16* apiName)
    {
        if (!JavascriptOperators::IsAnyNumberValue(sizeVar))
        {
            CloseIteratorSwallow(thisObject, scriptContext);
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
        }
        double numSize = JavascriptConversion::ToNumber(sizeVar, scriptContext);
        if (!NumberUtilities::IsFinite(numSize) || JavascriptConversion::ToInteger(numSize) != numSize)
        {
            CloseIteratorSwallow(thisObject, scriptContext);
            JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
        }
        if (numSize < 1.0 || numSize > static_cast<double>(UINT32_MAX))
        {
            CloseIteratorSwallow(thisObject, scriptContext);
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
        }
        return static_cast<uint32>(numSize);
    }

    bool ValidateUndersizedOrClose(RecyclableObject* thisObject, Var undersizedVar, ScriptContext* scriptContext, const char16* apiName)
    {
        if (JavascriptOperators::IsUndefinedObject(undersizedVar))
        {
            return false;
        }
        if (VarIs<JavascriptString>(undersizedVar))
        {
            JavascriptString* str = UnsafeVarTo<JavascriptString>(undersizedVar);
            if (str->BufferEquals(_u("only-full"), 9))
            {
                return false;
            }
            if (str->BufferEquals(_u("allow-partial"), 13))
            {
                return true;
            }
        }
        CloseIteratorSwallow(thisObject, scriptContext);
        JavascriptError::ThrowTypeError(scriptContext, JSERR_FunctionArgument_Invalid, apiName);
    }

    Var CreateChunkingHelper(RecyclableObject* function, Arguments& args, bool isWindows)
    {
        ScriptContext* scriptContext = function->GetScriptContext();
        JavascriptLibrary* library = scriptContext->GetLibrary();
        const char16* apiName = isWindows ? _u("Iterator.prototype.windows") : _u("Iterator.prototype.chunks");

        Var thisVar = args.Info.Count > 0 ? args[0] : library->GetUndefined();
        if (!JavascriptOperators::IsObject(thisVar))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        RecyclableObject* thisObject = VarTo<RecyclableObject>(CrossSite::MarshalVar(scriptContext, thisVar));

        Var sizeVar = args.Info.Count > 1 ? args[1] : library->GetUndefined();
        uint32 targetSize = ValidateChunkSizeOrClose(thisObject, sizeVar, scriptContext, apiName);

        bool allowPartial = false;
        if (isWindows)
        {
            Var undersizedVar = args.Info.Count > 2 ? args[2] : library->GetUndefined();
            allowPartial = ValidateUndersizedOrClose(thisObject, undersizedVar, scriptContext, apiName);
        }

        // GetIteratorDirect(O): read "next" once after argument validation.
        Var nextMethod = JavascriptOperators::GetPropertyNoCache(thisObject, PropertyIds::next, scriptContext);
        nextMethod = CrossSite::MarshalVar(scriptContext, nextMethod);

        JavascriptIteratorChunkState* chunkState = RecyclerNew(
            scriptContext->GetRecycler(),
            JavascriptIteratorChunkState,
            targetSize,
            isWindows,
            allowPartial);

        return JavascriptIteratorHelper::CreateChunkHelper(
            scriptContext,
            isWindows ? IteratorHelperKind::Windows : IteratorHelperKind::Chunks,
            thisObject,
            nextMethod,
            chunkState);
    }
} // namespace

    JavascriptIteratorChunkState::JavascriptIteratorChunkState(uint32 targetSize, bool isWindows, bool allowPartial)
        : buffer(nullptr),
          capacity(0),
          head(0),
          count(0),
          targetSize(targetSize),
          isWindows(isWindows),
          allowPartial(allowPartial)
    {
        Assert(targetSize >= 1);
    }

    uint32 JavascriptIteratorChunkState::LogicalToPhysicalIndex(uint32 logicalIndex) const
    {
        Assert(this->capacity > 0);
        Assert(this->head < this->capacity);
        Assert(logicalIndex < this->capacity);
        uint32 tailSpace = this->capacity - this->head;
        return logicalIndex < tailSpace ? (this->head + logicalIndex) : (logicalIndex - tailSpace);
    }

    void JavascriptIteratorChunkState::EnsureCapacityForPush(ScriptContext* scriptContext)
    {
        Assert(this->count < this->targetSize);
        if (this->count < this->capacity)
        {
            return;
        }
        uint32 newCap = 0;
        if (this->capacity == 0)
        {
            newCap = this->targetSize < 4u ? this->targetSize : 4u;
        }
        else
        {
            newCap = (this->capacity <= this->targetSize / 2u) ? (this->capacity * 2u) : this->targetSize;
        }
        Assert(newCap > this->capacity && newCap <= this->targetSize);
        Field(Var)* newBuffer = RecyclerNewArrayZ(scriptContext->GetRecycler(), Field(Var), newCap);
        for (uint32 i = 0; i < this->count; ++i)
        {
            newBuffer[i] = this->buffer[LogicalToPhysicalIndex(i)];
        }
        for (uint32 i = 0; i < this->count; ++i)
        {
            this->buffer[LogicalToPhysicalIndex(i)] = nullptr;
        }
        this->buffer = newBuffer;
        this->capacity = newCap;
        this->head = 0;
    }

    void JavascriptIteratorChunkState::Push(ScriptContext* scriptContext, Var value)
    {
        EnsureCapacityForPush(scriptContext);
        Var marshaled = CrossSite::MarshalVar(scriptContext, value);
        uint32 tail = LogicalToPhysicalIndex(this->count);
        this->buffer[tail] = marshaled;
        this->count++;
    }

    void JavascriptIteratorChunkState::PushSliding(ScriptContext* scriptContext, Var value)
    {
        Assert(this->isWindows);
        Assert(this->count == this->targetSize);
        Assert(this->capacity == this->targetSize);
        Var marshaled = CrossSite::MarshalVar(scriptContext, value);
        this->buffer[this->head] = marshaled;
        this->head = (this->capacity - 1u == this->head) ? 0 : (this->head + 1u);
    }

    JavascriptArray* JavascriptIteratorChunkState::Materialize(ScriptContext* scriptContext)
    {
        JavascriptArray* arr = scriptContext->GetLibrary()->CreateArray(this->count);
        for (uint32 i = 0; i < this->count; ++i)
        {
            Var elem = this->buffer[LogicalToPhysicalIndex(i)];
            Assert(elem != nullptr);
            arr->SetItem(i, CrossSite::MarshalVar(scriptContext, elem), PropertyOperation_None);
        }
        return arr;
    }

    void JavascriptIteratorChunkState::ConsumeElements()
    {
        if (this->buffer != nullptr)
        {
            for (uint32 i = 0; i < this->count; ++i)
            {
                this->buffer[LogicalToPhysicalIndex(i)] = nullptr;
            }
        }
        this->head = 0;
        this->count = 0;
    }

    void JavascriptIteratorChunkState::Clear()
    {
        ConsumeElements();
        this->buffer = nullptr;
        this->capacity = 0;
    }

    Var JavascriptIterator::EntryChunks(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateChunkingHelper(function, args, false);
    }

    Var JavascriptIterator::EntryWindows(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        Assert(!(callInfo.Flags & CallFlags_New));
        return CreateChunkingHelper(function, args, true);
    }

    bool JavascriptIteratorHelper::AdvanceChunking(ScriptContext* scriptContext, Var* outValue)
    {
        JavascriptIteratorChunkState* state = this->chunkState;
        if (state == nullptr || this->IsIteratorDone())
        {
            if (state != nullptr)
            {
                state->Clear();
                this->chunkState = nullptr;
            }
            return false;
        }

        for (;;)
        {
            Var value = nullptr;
            if (!StepUnderlying(scriptContext, &value))
            {
                bool yieldPartial = state->IsWindows()
                    ? (state->AllowPartial() && state->GetCount() > 0 && state->GetCount() < state->GetTargetSize())
                    : (state->GetCount() > 0);
                if (yieldPartial)
                {
                    *outValue = state->Materialize(scriptContext);
                    state->Clear();
                    this->chunkState = nullptr;
                    return true;
                }
                state->Clear();
                this->chunkState = nullptr;
                return false;
            }

            if (state->IsWindows() && state->GetCount() == state->GetTargetSize())
            {
                state->PushSliding(scriptContext, value);
            }
            else
            {
                state->Push(scriptContext, value);
            }

            if (state->GetCount() == state->GetTargetSize())
            {
                *outValue = state->Materialize(scriptContext);
                if (!state->IsWindows())
                {
                    state->ConsumeElements();
                }
                return true;
            }
        }
    }

    void JavascriptIteratorHelper::CloseChunkingNormal(ScriptContext* scriptContext)
    {
        (void)scriptContext;
        if (this->chunkState != nullptr)
        {
            this->chunkState->Clear();
            this->chunkState = nullptr;
        }
    }

    void JavascriptIteratorHelper::CloseChunkingSwallow(ScriptContext* scriptContext)
    {
        (void)scriptContext;
        if (this->chunkState != nullptr)
        {
            this->chunkState->Clear();
            this->chunkState = nullptr;
        }
    }
} // namespace Js
