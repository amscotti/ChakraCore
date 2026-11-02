//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) 2021 ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Language/InterpreterStackFrame.h"

using namespace Js;

namespace
{
    // The retained write-barrier frame is tagged with the low bit (the frame is
    // at least pointer-aligned) so the recycler's conservative heap scan sees
    // an unaligned value and does not mark the frame as live.
    const uintptr_t WriteBarrierFrameTag = 1;

    uintptr_t TagWriteBarrierFrame(InterpreterStackFrame* frame)
    {
        Assert(frame != nullptr);
        Assert((reinterpret_cast<uintptr_t>(frame) & WriteBarrierFrameTag) == 0);
        return reinterpret_cast<uintptr_t>(frame) | WriteBarrierFrameTag;
    }

    InterpreterStackFrame* UntagWriteBarrierFrame(uintptr_t taggedFrame)
    {
        Assert(taggedFrame != 0);
        return reinterpret_cast<InterpreterStackFrame*>(taggedFrame & ~WriteBarrierFrameTag);
    }

    // RAII helper to set the state of the generator to completed if an exception is 
    // thrown or if the save state InterpreterStackFrame is never created implying 
    // the generator is JITed and returned without ever yielding
    struct GeneratorStateHelper
    {
        using GeneratorState = JavascriptGenerator::GeneratorState;

        JavascriptGenerator* generator;
        bool didThrow;

        GeneratorStateHelper(JavascriptGenerator* generator) :
            generator(generator),
            didThrow(true)
        {
            generator->SetState(GeneratorState::Executing);
        }

        ~GeneratorStateHelper()
        {
            generator->SetState(IsDone() ? GeneratorState::Completed : GeneratorState::Suspended);
        }

        bool IsDone()
        {
            // If the generator is jit'd, we set its interpreter frame to nullptr at the
            // end right before the epilogue to signal that the generator has completed
            auto* frame = generator->GetFrame();

            if (didThrow || frame == nullptr)
                return true;

            int nextOffset = frame->GetReader()->GetCurrentOffset();
            int endOffset = frame->GetFunctionBody()->GetByteCode()->GetLength();

            if (nextOffset == endOffset - 1)
                return true;

            return false;
        }
    };
}

JavascriptGenerator::JavascriptGenerator(
    DynamicType* type,
    Arguments& args,
    ScriptFunction* scriptFunction) :
        DynamicObject(type),
        args(args),
        frame(nullptr),
        state(GeneratorState::SuspendedStart),
        scriptFunction(scriptFunction),
        resumeYieldObject(nullptr),
        writeBarrierFrame(0)
#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
        , liveCoroutineFunctionBody(nullptr)
        , liveCoroutineFrameCounted(false)
#endif
{}

JavascriptGenerator* JavascriptGenerator::New(
    Recycler* recycler,
    DynamicType* generatorType,
    Arguments& args,
    ScriptFunction* scriptFunction)
{
    // InterpreterStackFrame takes a pointer to the args, so copy them to the recycler
    // heap and use that buffer for the generator's InterpreterStackFrame
    Field(Var)* argValuesCopy = nullptr;

    if (args.Info.Count > 0)
    {
        argValuesCopy = RecyclerNewArray(recycler, Field(Var), args.Info.Count);
        CopyArray(argValuesCopy, args.Info.Count, args.Values, args.Info.Count);
    }

    Arguments heapArgs(args.Info, unsafe_write_barrier_cast<Var*>(argValuesCopy));

#if GLOBAL_ENABLE_WRITE_BARRIER
    // On Apple ARM64 the reparse guard also needs Finalize to run for a
    // collected suspended generator even if the software write barrier is
    // disabled at runtime. Finalization is what drops the FunctionBody's live
    // frame count in that case. The extra finalizer for non-generator
    // JavascriptGenerator users is a no-op; keeping it unconditional avoids
    // relying on flags that are not final yet at allocation time.
    if (CONFIG_FLAG(ForceSoftwareWriteBarrier) || ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD)
    {
        JavascriptGenerator* obj = RecyclerNewFinalized(
            recycler,
            JavascriptGenerator,
            generatorType,
            heapArgs,
            scriptFunction);

        if (CONFIG_FLAG(ForceSoftwareWriteBarrier) && obj->args.Values != nullptr)
        {
            recycler->RegisterPendingWriteBarrierBlock(
                obj->args.Values,
                obj->args.Info.Count * sizeof(Var));

            recycler->RegisterPendingWriteBarrierBlock(
                &obj->args.Values,
                sizeof(Var*));
        }

        return obj;
    }
#endif

    return RecyclerNew(recycler, JavascriptGenerator, generatorType, heapArgs, scriptFunction);
}

template<>
bool Js::VarIsImpl<JavascriptGenerator>(RecyclableObject* obj)
{
    auto typeId = JavascriptOperators::GetTypeId(obj);
    return typeId == TypeIds_Generator || typeId == TypeIds_AsyncGenerator;
}

void JavascriptGenerator::SetFrame(InterpreterStackFrame* frame, size_t bytes)
{
    Assert(this->frame == nullptr);
    this->frame = frame;
#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
    // The frame's ByteCodeReader and operand metadata point into this body.
    // Count it exactly once for the generator's lifetime; completion or the
    // generator's finalizer releases the count even after the JIT epilogue has
    // nulled `frame`.
    Assert(!this->liveCoroutineFrameCounted);
    Assert(this->liveCoroutineFunctionBody == nullptr);
    FunctionBody* functionBody = frame->GetFunctionBody();
    Assert(functionBody != nullptr);
    // Synchronous and async generators keep a resumable InterpreterStackFrame
    // whose reader points into this body. Plain async functions use the same
    // JavascriptGenerator object: their frames are counted too, but a debugger
    // reparse treats them differently - the body is not pinned; the reparse
    // gets a fresh FunctionBody and this body stays whole for the suspended
    // frames (FunctionBody::NewDebugReparseBodyForSuspendedAsync) - so the
    // debugger's locals/stepping baseline for plain async functions is
    // preserved. See doc/dbg-async-attach.md.
    if (functionBody->IsCoroutine())
    {
        functionBody->AddLiveCoroutineFrame();
        this->liveCoroutineFunctionBody = functionBody;
        this->liveCoroutineFrameCounted = true;
    }
#endif
#if GLOBAL_ENABLE_WRITE_BARRIER
    if (CONFIG_FLAG(ForceSoftwareWriteBarrier))
    {
        Assert(this->writeBarrierFrame == 0);
        GetScriptContext()->GetRecycler()->RegisterPendingWriteBarrierBlock(frame, bytes);
        this->writeBarrierFrame = TagWriteBarrierFrame(frame);
    }
#endif
}

void JavascriptGenerator::SetFrameSlots(Js::RegSlot slotCount, Field(Var)* frameSlotArray)
{
    AssertMsg(this->frame->GetFunctionBody()->GetLocalsCount() == slotCount, 
        "Unexpected mismatch in frame slot count for generated.");

    for (Js::RegSlot i = 0; i < slotCount; i++)
        GetFrame()->m_localSlots[i] = frameSlotArray[i];
}

void JavascriptGenerator::UnregisterPendingWriteBarrierBlocks()
{
#if GLOBAL_ENABLE_WRITE_BARRIER
    if (!CONFIG_FLAG(ForceSoftwareWriteBarrier))
    {
        return;
    }

    auto* recycler = GetScriptContext()->GetRecycler();

    // SetState(Completed) clears these pointers, so each registration is
    // removed exactly once: here while the generator is still suspended, or
    // from the completion transition before the pointers are cleared.
    //
    // The JIT epilogue nulls `frame` before completion is observed, so prefer
    // the address retained by SetFrame. Clearing it before the unregister
    // keeps the cleanup exactly once; the `frame` fallback covers a frame
    // registered without a retained address.
    if (this->writeBarrierFrame != 0)
    {
        InterpreterStackFrame* registeredFrame = UntagWriteBarrierFrame(this->writeBarrierFrame);
        this->writeBarrierFrame = 0;
        recycler->UnRegisterPendingWriteBarrierBlock(registeredFrame);
    }
    else if (this->frame)
    {
        recycler->UnRegisterPendingWriteBarrierBlock(this->frame);
    }

    if (this->args.Values)
    {
        recycler->UnRegisterPendingWriteBarrierBlock(this->args.Values);
        recycler->UnRegisterPendingWriteBarrierBlock(&this->args.Values);
    }
#endif
}

#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
void JavascriptGenerator::ReleaseLiveCoroutineFrame()
{
    if (this->liveCoroutineFrameCounted)
    {
        Assert(this->liveCoroutineFunctionBody != nullptr);
        this->liveCoroutineFunctionBody->RemoveLiveCoroutineFrame();
        this->liveCoroutineFunctionBody = nullptr;
        this->liveCoroutineFrameCounted = false;
    }
}
#endif

#if GLOBAL_ENABLE_WRITE_BARRIER
void JavascriptGenerator::Finalize(bool isShutdown)
{
    if (!isShutdown)
    {
#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
        // A collected suspended generator will never resume, so it no longer
        // needs the FunctionBody's bytecode pinned. Shutdown skips this: the
        // context and its bodies are going away together, and ordering between
        // the generator and FunctionBody finalizers is not guaranteed.
        ReleaseLiveCoroutineFrame();
#endif
        UnregisterPendingWriteBarrierBlocks();
    }
}
#endif

void JavascriptGenerator::ThrowIfExecuting(const char16* apiName)
{
    if (this->IsExecuting())
    {
        JavascriptError::ThrowTypeError(
            GetScriptContext(),
            JSERR_GeneratorAlreadyExecuting,
            apiName);
    }
}

Var JavascriptGenerator::CallGenerator(Var data, ResumeYieldKind resumeKind)
{
    Assert(!IsExecuting() && !IsCompleted());

    ScriptContext* scriptContext = this->GetScriptContext();
    JavascriptLibrary* library = scriptContext->GetLibrary();
    Var result = nullptr;

    if (this->frame)
    {
        // if the function already has a state it may be going to resume in the jit
        // if so copy any innerScopes into registers jit can access
        // Read inner-scope layout from the frame's body: after a debug reparse
        // the script function's proxy may point at a fresh body while a
        // suspended frame still executes the original one (the coroutine
        // inner-scope numbering is stable across the reparse, but the frame's
        // body is the authority for the frame's layout).
        FunctionBody* frameBody = this->frame->GetFunctionBody();
        uint32 innerScopeCount = frameBody->GetInnerScopeCount();
        for (uint32 i = 0; i < innerScopeCount; ++i)
        {
            Js::RegSlot reg = frameBody->GetFirstInnerScopeRegister() + i;
            this->frame->SetNonVarReg(reg, this->frame->InnerScopeFromIndex(i));
        }
    }

    SetResumeYieldProperties(data, resumeKind);

    {
        Var thunkArgs[] = {this, this->resumeYieldObject};
        Arguments arguments(_countof(thunkArgs), thunkArgs);
        GeneratorStateHelper helper(this);

        try
        {
            BEGIN_SAFE_REENTRANT_CALL(scriptContext->GetThreadContext())
            {
                result = JavascriptFunction::CallFunction<1>(
                    this->scriptFunction,
                    this->scriptFunction->GetEntryPoint(),
                    arguments);
            }
            END_SAFE_REENTRANT_CALL
            helper.didThrow = false;
        }
        catch (const JavascriptException& err)
        {
            JavascriptExceptionOperators::DoThrowCheckClone(err.GetAndClear(), scriptContext);
        }
    }

    // Clear the value property of the resume yield object so that we don't
    // extend the lifetime of the value
    SetResumeYieldProperties(library->GetUndefined(), ResumeYieldKind::Normal);

    if (IsCompleted())
        return library->CreateIteratorResultObject(result, library->GetTrue());

    return result;
}

void JavascriptGenerator::SetResumeYieldProperties(Var value, ResumeYieldKind kind)
{
    auto* library = GetScriptContext()->GetLibrary();
    DynamicType* type = library->GetResumeYieldObjectType();

    if (!this->resumeYieldObject)
        this->resumeYieldObject = DynamicObject::New(GetScriptContext()->GetRecycler(), type);
    else
        AssertOrFailFast(this->resumeYieldObject->GetDynamicType() == type);

    Var kindVar = TaggedInt::ToVarUnchecked((int)kind);
    this->resumeYieldObject->SetSlot(SetSlotArguments(Js::PropertyIds::value, 0, value));
    this->resumeYieldObject->SetSlot(SetSlotArguments(Js::PropertyIds::kind, 1, kindVar));
}

Var JavascriptGenerator::EntryNext(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
    ARGUMENTS(args, callInfo);

    auto* scriptContext = function->GetScriptContext();
    auto* library = scriptContext->GetLibrary();

    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Generator.prototype.next"));

    if (!VarIs<DynamicObject>(args[0]) || JavascriptOperators::GetTypeId(args[0]) != TypeIds_Generator)
    {
        JavascriptError::ThrowTypeErrorVar(
            scriptContext,
            JSERR_NeedObjectOfType,
            _u("Generator.prototype.next"),
            _u("Generator"));
    }

    Var undefinedVar = library->GetUndefined();
    Var input = args.Info.Count > 1 ? args[1] : undefinedVar;

    auto* generator = UnsafeVarTo<JavascriptGenerator>(args[0]);

    if (generator->IsCompleted())
        return library->CreateIteratorResultObject(undefinedVar, library->GetTrue());

    generator->ThrowIfExecuting(_u("Generator.prototype.next"));
    return generator->CallGenerator(input, ResumeYieldKind::Normal);
}

Var JavascriptGenerator::EntryReturn(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
    ARGUMENTS(args, callInfo);

    auto* scriptContext = function->GetScriptContext();
    auto* library = scriptContext->GetLibrary();

    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Generator.prototype.return"));

    if (!VarIs<DynamicObject>(args[0]) || JavascriptOperators::GetTypeId(args[0]) != TypeIds_Generator)
    {
        JavascriptError::ThrowTypeErrorVar(
            scriptContext,
            JSERR_NeedObjectOfType,
            _u("Generator.prototype.return"),
            _u("Generator"));
    }

    Var input = args.Info.Count > 1 ? args[1] : library->GetUndefined();

    auto* generator = UnsafeVarTo<JavascriptGenerator>(args[0]);

    if (generator->IsSuspendedStart())
        generator->SetCompleted();

    if (generator->IsCompleted())
        return library->CreateIteratorResultObject(input, library->GetTrue());

    generator->ThrowIfExecuting(_u("Generator.prototype.return"));
    return generator->CallGenerator(input, ResumeYieldKind::Return);
}

Var JavascriptGenerator::EntryThrow(RecyclableObject* function, CallInfo callInfo, ...)
{
    PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
    ARGUMENTS(args, callInfo);

    auto* scriptContext = function->GetScriptContext();
    auto* library = scriptContext->GetLibrary();

    AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Generator.prototype.throw"));

    if (!VarIs<DynamicObject>(args[0]) || JavascriptOperators::GetTypeId(args[0]) != TypeIds_Generator)
    {
        JavascriptError::ThrowTypeErrorVar(
            scriptContext,
            JSERR_NeedObjectOfType,
            _u("Generator.prototype.throw"),
            _u("Generator"));
    }

    Var input = args.Info.Count > 1 ? args[1] : library->GetUndefined();

    auto* generator = UnsafeVarTo<JavascriptGenerator>(args[0]);

    if (generator->IsSuspendedStart())
        generator->SetCompleted();

    if (generator->IsCompleted())
        JavascriptExceptionOperators::OP_Throw(input, scriptContext);

    generator->ThrowIfExecuting(_u("Generator.prototype.throw"));
    return generator->CallGenerator(input, ResumeYieldKind::Throw);
}

bool JavascriptGenerator::IsAsyncModule() const
{ 
    FunctionProxy* proxy = this->scriptFunction->GetFunctionProxy();
    return proxy->IsModule() && proxy->IsAsync();
}

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
void JavascriptGenerator::OutputBailInTrace(JavascriptGenerator* generator)
{
    char16 debugStringBuffer[MAX_FUNCTION_BODY_DEBUG_STRING_SIZE];
    FunctionBody* fnBody = generator->scriptFunction->GetFunctionBody();

    Output::Print(
        _u("BailIn: function: %s (%s) offset: #%04x\n"),
        fnBody->GetDisplayName(),
        fnBody->GetDebugNumberSet(debugStringBuffer),
        generator->frame->m_reader.GetCurrentOffset());

    if (generator->bailInSymbolsTraceArrayCount == 0)
    {
        Output::Print(
            _u("BailIn: No symbols reloaded\n"),
            fnBody->GetDisplayName(),
            fnBody->GetDebugNumberSet(debugStringBuffer));
    }
    else
    {
        for (int i = 0; i < generator->bailInSymbolsTraceArrayCount; i++)
        {
            const BailInSymbol& symbol = generator->bailInSymbolsTraceArray[i];
            Output::Print(_u("BailIn: Register #%4d, value: 0x%p\n"), symbol.id, symbol.value);
        }
    }
}
#endif

#if ENABLE_TTD

JavascriptGenerator *JavascriptGenerator::New(
    Recycler* recycler,
    DynamicType* generatorType,
    Arguments& args,
    JavascriptGenerator::GeneratorState state)
{
    auto* obj = JavascriptGenerator::New(recycler, generatorType, args, nullptr);
    obj->SetState(state);
    return obj;
}

void JavascriptGenerator::MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor)
{
    if (this->scriptFunction != nullptr)
    {
        extractor->MarkVisitVar(this->scriptFunction);
    }

    // frame is null when generator has been completed
    if (this->frame != nullptr)
    {
        // mark slot variables for traversal
        Js::RegSlot slotCount = this->frame->GetFunctionBody()->GetLocalsCount();
        for (Js::RegSlot i = 0; i < slotCount; i++)
        {
            Js::Var curr = this->frame->m_localSlots[i];
            if (curr != nullptr)
            {
                extractor->MarkVisitVar(curr);
            }
        }
    }

    // args.Values is null when generator has been completed
    if (this->args.Values != nullptr)
    {
        // mark argument variables for traversal
        uint32 argCount = this->args.GetArgCountWithExtraArgs();
        for (uint32 i = 0; i < argCount; i++)
        {
            Js::Var curr = this->args[i];
            if (curr != nullptr)
            {
                extractor->MarkVisitVar(curr);
            }
        }
    }
}

TTD::NSSnapObjects::SnapObjectType JavascriptGenerator::GetSnapTag_TTD() const
{
    return TTD::NSSnapObjects::SnapObjectType::SnapGenerator;
}

void JavascriptGenerator::ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc)
{
    TTD::NSSnapObjects::SnapGeneratorInfo* gi = alloc.SlabAllocateStruct<TTD::NSSnapObjects::SnapGeneratorInfo>();

    // TODO: BUGBUG - figure out how to determine what the prototype was
    gi->generatorPrototype = 0;
    //if (this->GetPrototype() == this->GetScriptContext()->GetLibrary()->GetNull())
    //{
    //    gi->generatorPrototype = 1;
    //}
    //else if (this->GetType() == this->GetScriptContext()->GetLibrary()->GetGeneratorConstructorPrototypeObjectType())
    //{
    //    // check type here, not prototype, since type is static across generators
    //    gi->generatorPrototype = 2;
    //}
    //else
    //{
    //    //TTDAssert(false, "unexpected prototype found JavascriptGenerator");
    //}

    gi->scriptFunction = TTD_CONVERT_VAR_TO_PTR_ID(this->scriptFunction);
    gi->state = static_cast<uint32>(this->state);


    // grab slot info from InterpreterStackFrame
    gi->frame_slotCount = 0;
    gi->frame_slotArray = nullptr;
    if (this->frame != nullptr)
    {
        gi->frame_slotCount = this->frame->GetFunctionBody()->GetLocalsCount();
        if (gi->frame_slotCount > 0)
        {
            gi->frame_slotArray = alloc.SlabAllocateArray<TTD::TTDVar>(gi->frame_slotCount);
        }
        for (Js::RegSlot i = 0; i < gi->frame_slotCount; i++)
        {
            gi->frame_slotArray[i] = this->frame->m_localSlots[i];
        }
    }

    // grab arguments
    TTD_PTR_ID* depArray = nullptr;
    uint32 depCount = 0;

    if (this->args.Values == nullptr)
    {
        gi->arguments_count = 0;
    }
    else
    {
        gi->arguments_count = this->args.GetArgCountWithExtraArgs();
    }

    gi->arguments_values = nullptr;
    if (gi->arguments_count > 0)
    {
        gi->arguments_values = alloc.SlabAllocateArray<TTD::TTDVar>(gi->arguments_count);
        depArray = alloc.SlabReserveArraySpace<TTD_PTR_ID>(gi->arguments_count);
    }

    for (uint32 i = 0; i < gi->arguments_count; i++)
    {
        gi->arguments_values[i] = this->args[i];
        if (gi->arguments_values[i] != nullptr && TTD::JsSupport::IsVarComplexKind(gi->arguments_values[i]))
        {
            depArray[depCount] = TTD_CONVERT_VAR_TO_PTR_ID(gi->arguments_values[i]);
            depCount++;
        }
    }

    if (depCount > 0)
    {
        alloc.SlabCommitArraySpace<TTD_PTR_ID>(depCount, gi->arguments_count);
    }
    else if (gi->arguments_count > 0)
    {
        alloc.SlabAbortArraySpace<TTD_PTR_ID>(gi->arguments_count);
    }

    if (this->frame != nullptr)
    {
        gi->byteCodeReader_offset = this->frame->GetReader()->GetCurrentOffset();
    }
    else
    {
        gi->byteCodeReader_offset = 0;
    }

    // Copy the CallInfo data into the struct
    gi->arguments_callInfo_count = gi->arguments_count > 0 ? this->args.Info.Count : 0;
    gi->arguments_callInfo_flags = this->args.Info.Flags;

    // TODO:  understand why there's a mis-match between args.Info.Count and GetArgCountWithExtraArgs
    // TTDAssert(this->args.Info.Count == gi->arguments_count, "mismatched count between args.Info and GetArgCountWithExtraArgs");

    if (depCount == 0)
    {
        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapGeneratorInfo*, TTD::NSSnapObjects::SnapObjectType::SnapGenerator>(objData, gi);
    }
    else
    {
        TTDAssert(depArray != nullptr, "depArray should be non-null if depCount is > 0");
        TTD::NSSnapObjects::StdExtractSetKindSpecificInfo<TTD::NSSnapObjects::SnapGeneratorInfo*, TTD::NSSnapObjects::SnapObjectType::SnapGenerator>(objData, gi, alloc, depCount, depArray);
    }

}
#endif
