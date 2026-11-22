//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    // GC-traced state for Iterator.prototype.{chunks,windows}.
    //
    // Ownership & GC lifetime record (see doc/memory-management.md section 3):
    // - Resource / creation: Recycler-allocated JavascriptIteratorChunkState
    //   owning a lazily allocated circular buffer (`Field(Field(Var)*) buffer`)
    //   of `Field(Var)` slots, created in EntryChunks / EntryWindows.
    // - Domain & allocator: Recycler heap (`scriptContext->GetRecycler()`),
    //   traced via `Field(...)` write-barrier pointers from the owning
    //   JavascriptIteratorHelper (`helper->chunkState`).
    // - Physical vs logical: `capacity` tracks the physical `buffer` slot
    //   extent (`0 <= count <= capacity <= targetSize <= UINT32_MAX`); `head`
    //   and `count` track active circular-buffer elements without aliasing any
    //   yielded JS Array (`Materialize` always copies into a fresh Array).
    // - Owner & transfer: exclusively owned by one JavascriptIteratorHelper;
    //   never shared or exposed directly to JavaScript.
    // - Reentrancy & invalidation: user `next()` / `return()` and GC can run
    //   across yields/steps; `JavascriptIteratorHelper::executing` blocks
    //   reentrant `next()`/`return()`, while `Field(...)` tracing keeps active
    //   buffered elements alive across GC.
    // - Release & retention bound: `ConsumeElements()` nulls `Field(Var)` slots
    //   after each full chunk yield; `Clear()` nulls slots and drops `buffer`
    //   on exhaustion, final partial yield, `return()`, or abrupt unwind,
    //   followed by `helper->chunkState = nullptr`. Lazy growth from
    //   `min(targetSize, 4)` bounds empty/short streams even for `2^32 - 1`.
    class JavascriptIteratorChunkState
    {
    public:
        JavascriptIteratorChunkState(uint32 targetSize, bool isWindows, bool allowPartial);

        bool IsWindows() const { return isWindows; }
        bool AllowPartial() const { return allowPartial; }
        uint32 GetTargetSize() const { return targetSize; }
        uint32 GetCount() const { return count; }

        void Push(ScriptContext* scriptContext, Var value);
        void PushSliding(ScriptContext* scriptContext, Var value);
        JavascriptArray* Materialize(ScriptContext* scriptContext);
        void ConsumeElements();
        void Clear();

    private:
        uint32 LogicalToPhysicalIndex(uint32 logicalIndex) const;
        void EnsureCapacityForPush(ScriptContext* scriptContext);

        Field(Field(Var)*) buffer;
        Field(uint32) capacity;
        Field(uint32) head;
        Field(uint32) count;
        Field(uint32) targetSize;
        Field(bool) isWindows;
        Field(bool) allowPartial;
    };
} // namespace Js
