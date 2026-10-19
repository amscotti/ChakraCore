//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{

enum class ResumeYieldKind
{
    Normal = 0,
    Throw = 1,
    Return = 2
};

class JavascriptGenerator : public DynamicObject
{
public:
    enum class GeneratorState
    {
        SuspendedStart,
        Suspended,
        Executing,
        Completed
    };

    static uint32 GetFrameOffset()
    {
        return offsetof(JavascriptGenerator, frame);
    }

    static uint32 GetCallInfoOffset()
    {
        return offsetof(JavascriptGenerator, args) + Arguments::GetCallInfoOffset();
    }

    static uint32 GetArgsPtrOffset()
    {
        return offsetof(JavascriptGenerator, args) + Arguments::GetValuesOffset();
    }

    void SetState(GeneratorState state)
    {
        const bool isCompleting = state == GeneratorState::Completed &&
            this->state != GeneratorState::Completed;

        this->state = state;
        if (isCompleting)
        {
            // Completion drops the generator's references to the frame and
            // argument buffer. Remove their pending software write-barrier
            // entries while the pointers still identify the registered blocks.
            UnregisterPendingWriteBarrierBlocks();
        }

        if (state == GeneratorState::Completed)
        {
#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
            // Completion is observed after the JIT epilogue may already have
            // nulled `frame`, so use the retained FunctionBody registration
            // rather than the frame to release the reparse guard.
            ReleaseLiveCoroutineFrame();
#endif
            frame = nullptr;
            args.Values = nullptr;
            scriptFunction = nullptr;
        }
    }

    void ThrowIfExecuting(const char16* apiName);
    Var CallGenerator(Var data, ResumeYieldKind resumeKind);

private:
    Field(InterpreterStackFrame*) frame;
    Field(GeneratorState) state;
    Field(Arguments) args;
    Field(ScriptFunction*) scriptFunction;
    Field(DynamicObject*) resumeYieldObject;

    // Address of the frame registered with the software write barrier. The JIT
    // epilogue nulls `frame` before completion is observed, so this retained
    // address is the only stable way to unregister the pending block.
    // The low bit is tagged so the conservative heap scan does not treat this
    // bookkeeping word as a live GC reference. It is cleared when the block is
    // unregistered and is never used to access the frame.
    uintptr_t writeBarrierFrame;

#if ENABLE_SUSPENDED_COROUTINE_REPARSE_GUARD
    // FunctionBody that owns the bytecode backing the suspended frame. This
    // Field keeps the body alive for as long as the generator can resume into
    // it; the paired count on that body blocks a debugger reparse from freeing
    // the bytecode. Set by SetFrame and released exactly once on completion or
    // on finalization of a collected suspended generator.
    Field(FunctionBody*) liveCoroutineFunctionBody;
    bool liveCoroutineFrameCounted;

    void ReleaseLiveCoroutineFrame();
#endif

    void SetResumeYieldProperties(Var value, ResumeYieldKind kind);
    void UnregisterPendingWriteBarrierBlocks();

protected:
    DEFINE_VTABLE_CTOR_MEMBER_INIT(JavascriptGenerator, DynamicObject, args);
    DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(JavascriptGenerator);

    JavascriptGenerator(DynamicType* type, Arguments& args, ScriptFunction* scriptFunction);

public:

    static JavascriptGenerator* New(
        Recycler* recycler,
        DynamicType* generatorType,
        Arguments& args,
        ScriptFunction* scriptFunction);

    bool IsSuspendedStart() const { return this->state == GeneratorState::SuspendedStart; }
    bool IsExecuting() const { return this->state == GeneratorState::Executing; }
    bool IsSuspended() const { return this->state == GeneratorState::Suspended; }
    bool IsCompleted() const { return this->state == GeneratorState::Completed; }

    bool IsAsyncModule() const;

    void SetSuspendedStart()
    {
        Assert(
            this->state == GeneratorState::SuspendedStart || 
            this->state == GeneratorState::Suspended);
        this->state = GeneratorState::SuspendedStart;
    }

    void SetCompleted()
    {
        Assert(this->state != GeneratorState::Executing);
        this->SetState(GeneratorState::Completed);
    }

    InterpreterStackFrame* GetFrame() const { return frame; }
    const Arguments& GetArguments() const { return args; }

    void SetScriptFunction(ScriptFunction* sf) { this->scriptFunction = sf; }
    void SetFrame(InterpreterStackFrame* frame, size_t bytes);
    void SetFrameSlots(uint slotCount, Field(Var)* frameSlotArray);

#if GLOBAL_ENABLE_WRITE_BARRIER
    virtual void Finalize(bool isShutdown) override;
#endif

    class EntryInfo
    {
    public:
        static FunctionInfo Next;
        static FunctionInfo Return;
        static FunctionInfo Throw;
    };

    static Var EntryNext(RecyclableObject* function, CallInfo callInfo, ...);
    static Var EntryReturn(RecyclableObject* function, CallInfo callInfo, ...);
    static Var EntryThrow(RecyclableObject* function, CallInfo callInfo, ...);

#if ENABLE_TTD
    static JavascriptGenerator* New(
        Recycler* recycler,
        DynamicType* generatorType,
        Arguments &args,
        Js::JavascriptGenerator::GeneratorState generatorState);

    virtual void MarkVisitKindSpecificPtrs(TTD::SnapshotExtractor* extractor) override;
    virtual TTD::NSSnapObjects::SnapObjectType GetSnapTag_TTD() const override;
    virtual void ExtractSnapObjectDataInto(TTD::NSSnapObjects::SnapObject* objData, TTD::SlabAllocator& alloc) override;
    // virtual void ProcessCorePaths() override;
#endif

#ifdef ENABLE_DEBUG_CONFIG_OPTIONS
public:
    struct BailInSymbol {
        uint32 id;
        Var value;
        static uint32 GetBailInSymbolIdOffset() { return offsetof(BailInSymbol, id); }
        static uint32 GetBailInSymbolValueOffset() { return offsetof(BailInSymbol, value); }
    };

    Field(BailInSymbol*) bailInSymbolsTraceArray = nullptr;
    Field(int) bailInSymbolsTraceArrayCount = 0;

    static uint32 GetBailInSymbolsTraceArrayOffset()
    {
        return offsetof(JavascriptGenerator, bailInSymbolsTraceArray);
    }

    static uint32 GetBailInSymbolsTraceArrayCountOffset()
    {
        return offsetof(JavascriptGenerator, bailInSymbolsTraceArrayCount);
    }

    static void OutputBailInTrace(JavascriptGenerator* generator);
#endif

};

template<>
bool VarIsImpl<JavascriptGenerator>(RecyclableObject* obj);

}
