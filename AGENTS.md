# Coding-assistant guidance

The staged implementation scope and completion gates are in
[doc/modernization-plan.md](doc/modernization-plan.md).

Before changing allocation, ownership, retained pointers, C++/Zig/JsRT boundaries,
callbacks, async jobs, GC tracing/barriers or memory accounting, read
[doc/memory-management.md](doc/memory-management.md). Establish the allocation
domain, owner/transfer point, invalidation events and permitted thread/context
from actual source and callers. Supply focused tests for changed failure,
reentrancy, GC or completion paths and report the coverage actually exercised.

Keep the selected change small. Preserve existing runtime ownership and release
mechanisms until the new slice's contract is proved; use the guide's ownership
record and review receipt rather than inferring safety from a passing broad suite.
Its native Zig example is educational and does not implement the engine's GC.
