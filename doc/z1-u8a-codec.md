# Z1 Wedge Boundary Contract: `Uint8Array` Base64 / Hex Zig Codec

Status: Wave 1 (`q13-z1-base64`) first production `Z1` Zig 0.17.0 SIMD codec
for ES2026 `Uint8Array.{fromBase64,fromHex}` and
`Uint8Array.prototype.{toBase64,setFromBase64,toHex,setFromHex}`.

This document records the boundary contract required by
[doc/zig-strategy.md](zig-strategy.md) (section 5.3) and
[doc/memory-management.md](memory-management.md) (section 3).

## 1. Files and Build Integration

- C header: [lib/ZigCodec/u8a_codec.h](../lib/ZigCodec/u8a_codec.h)
- Freestanding Zig SIMD codec: [lib/ZigCodec/u8a_codec.zig](../lib/ZigCodec/u8a_codec.zig)
- C++ boundary wrapper: [lib/Runtime/Library/TypedArray.cpp](../lib/Runtime/Library/TypedArray.cpp)
- Build graph integration:
  - macOS (`build.zig`): compiled via `b.addObject` (`target`, `.optimize = .fast`, `.pic = true`, `.link_libc = false`, `.link_libcpp = false`), appended to `static_objs` outside the 540 C/C++ TU count (`WANT_STATIC_MEMBERS = 527`), and wired into `check-selftest`.
  - Linux (`tools/build/linux.zig`): compiled via `b.addObject`, appended to `libChakraCoreStatic.a` and `libChakraCore.so` outside `expectedCompileCount` (`STATIC_ARCHIVE_BASE = 670`), and wired into `check-selftest`.
- Symbol visibility: every exported entry point uses `callconv(.c)` and `@export(..., .{ .name = "zig_u8a_...", .linkage = .strong, .visibility = .hidden })`, keeping all `zig_u8a_*` symbols private to the linked shared library / binary.

## 2. Flat C ABI Surface (`u8a_codec.h`)

All functions are synchronous, stateless, and leaf-only (`link_libc = false`,
`link_libcpp = false`, zero Zig heap allocators, zero `Recycler` pointers,
zero C++ exceptions, zero syscalls/I/O):

- `zig_u8a_encode_hex(src, src_len, dst)`
- `zig_u8a_decode_hex(src, src_len, dst, dst_cap, out_read, out_written)`
- `zig_u8a_encode_base64_len(src_len, omit_padding)`
- `zig_u8a_encode_base64(src, src_len, dst, url_alphabet, omit_padding)`
- `zig_u8a_decode_base64_max_len(src_len)`
- `zig_u8a_decode_base64(src, src_len, dst, dst_cap, url_alphabet, last_chunk_mode, out_read, out_written)`

Nullability & empty-buffer rules:
- `src_len == 0` or `dst_cap == 0` never dereferences `src` or `dst` (e.g., ChakraCore `ArrayBuffer(0)` has `GetBuffer() == nullptr`).
- `dst == NULL` in `zig_u8a_decode_hex` / `zig_u8a_decode_base64` runs validation and byte counting into `*out_written` without writing memory.
- Overflow in `zig_u8a_encode_base64_len` returns `SIZE_MAX`.

## 3. Ownership Record (`doc/memory-management.md` section 3)

```text
Resource / creation call / source location:
  1) Input UTF-16 code units: JS::JavascriptString::GetString() in lib/Runtime/Library/TypedArray.cpp
  2) Output encoded UTF-16 string: Js::BufferStringBuilder in lib/Runtime/Library/TypedArray.cpp
  3) Output decoded Uint8Array: JavascriptLibrary::CreateArrayBuffer() + Uint8Array::Create() in TypedArray.cpp
  4) In-place setFromBase64/setFromHex target: caller TypedArrayBase::GetByteBuffer()

Domain and allocator instance (including backing allocator lifetime):
  - JS values (`JavascriptString`, `ArrayBuffer`, `TypedArrayBase`, `DynamicObject`) live in
    ScriptContext / Recycler GC domain.
  - ArrayBuffer storage is owned by ArrayBufferBase (PageAllocator / Heap / Recycler-backed virtual
    or inline storage managed by C++ runtime).
  - Stack scratch (`stackBuf[1024]`) in `DecodeToNewUint8Array` is frame-local and never escapes.
  - Zig `u8a_codec.zig` owns NO heap allocations (`0` bytes allocated inside Zig).

Physical allocation base, extent, alignment, release callback or pool kind:
  - Input string buffer: owned by flattened `JavascriptString*` (`16`-bit aligned `char16` buffer).
  - Output string buffer: `BufferStringBuilder::WritableString` allocated for `charLength + 1` code units.
  - TypedArray backing bytes: `[0 .. ta->GetByteLength()]` starting at `ta->GetByteBuffer()`, within
    ArrayBuffer's physical allocation (`<= MaxArrayBufferLength`).
  - Zig accesses `[*]const u8/u16` and `[*]u8/u16` with unaligned `@ptrCast` (`align(1)`) for SIMD loads/stores.

Logical length / borrowed views (kept separate from the allocation descriptor):
  - Logical byte length is `ta->GetByteLength()` (which reads live `GetArrayBuffer()->GetByteLength()`
    for RAB views). `IsDetachedBuffer()` and `IsOutOfBounds()` are verified in C++ right before borrow.
  - `u8a_codec.zig` receives explicit `(ptr, len)` scalar pairs only; no slices outlive the single call.

Owner; transfer/publication point; remaining aliases:
  - C++ runtime retains exclusive ownership of all JS objects and backing buffers before, during, and
    after each `zig_u8a_*` call.
  - `stackBuf[1024]` is copied into `ab->GetBuffer()` before publishing `Uint8Array::Create(...)`.
  - No raw pointers, `Recycler` pointers, or borrowed slices are stored across calls in Zig.

Borrow invalidation: GC / detach / growth / mutation / reset / close / disposal:
  - Options getters (`alphabet`, `omitPadding`, `lastChunkHandling`) execute BEFORE any raw pointer
    borrow or `IsDetachedBuffer() || IsOutOfBounds()` check.
  - On `setFromBase64`/`setFromHex`: receiver `TypeIds_Uint8Array` and `IsImmutableBuffer` checks
    execute BEFORE reading `string` or `options`.
  - Input `JavascriptString*`, target `TypedArrayBase*`, and newly created `ArrayBuffer*` are held in
    `ENTER_PINNED_SCOPE` across `GetString()`, `CreateArrayBuffer()`, and `BufferStringBuilder`
    allocations so conservative GC retains all backing storage.
  - During `zig_u8a_*` execution, zero JS callbacks or allocations occur, preventing detach/resize/GC
    while raw pointers are borrowed.

Potential user-code or reentrancy points:
  - `GetCodecOptionsObject`, `ReadAlphabetOption`, `ReadOmitPaddingOption` (`ToBoolean`), and
    `ReadLastChunkHandlingOption` invoke ES getters/valueOf on user objects BEFORE borrowing buffers.
  - `zig_u8a_*` itself cannot re-enter C++ or JS.

Thread/context allowed for use, root changes and destruction:
  - Entered strictly on the JS engine's active script thread (`ScriptContext`) inside runtime entry
    points (`EntryFromBase64`, `EntryFromHex`, `EntryToBase64`, `EntrySetFromBase64`, `EntryToHex`,
    `EntrySetFromHex`).
  - Zig leaf routines have no global mutable state or thread-local state and are pure data functions.

Release on success, partial failure, exception, OOM, cancel and shutdown:
  - Success: returns newly created GC-managed `Uint8Array`, `JavascriptString`, or `{ read, written }`
    plain object; stack scratch goes out of scope.
  - `ZIG_U8A_SYNTAX_ERROR`: C++ throws ES `SyntaxError`. For `setFromBase64` / `setFromHex`, valid
    decoded prefix bytes written before the syntax error remain in `ta` per ES2026
    `writes-up-to-error`; for `fromBase64` / `fromHex`, pass-1 / stackBuf fails before `ArrayBuffer`
    allocation.
  - OOM in `CreateArrayBuffer`, `BufferStringBuilder`, or `GetString()`: standard Recycler OOM exception
    path handles cleanup; Zig holds zero resources to leak.

Memory accounting, capacity/retention bound and exhaustion behavior:
  - Small decode (`<= 1024` bytes) uses 1 KiB bounded stack buffer.
  - Large decode (`> 1024` bytes) validates and computes exact byte count in pass 1 (`dst = NULL`),
    checks `<= ArrayBuffer::MaxArrayBufferLength`, and allocates exact `ArrayBuffer` size once.
  - Encode validates `IsValidCharCount(outLen)` before allocating string buffer (`RangeError` on overflow).

Tests / commands / measured results / remaining uncertainty:
  - `mise exec -- zig test lib/ZigCodec/u8a_codec.zig` and `-OReleaseFast`: All 7 tests passed (RFC 4648, base64url, lastChunkHandling modes, whitespace/non-ASCII rejection, maxLength lookahead, SIMD vs scalar equivalence).
  - `mise exec -- zig build check-selftest` and `mise exec -- zig build check-build-tools`: passed (96 Python build-tool tests OK, including updated Linux archive count pins `670/579/633/542`).
  - `mise exec -- zig build -Dprofile=debug-static check-metadata`: PASS (527 archive members in `libChakraCoreStatic.a`).
  - `mise exec -- zig build` (`test` profile) + Test262 6 Uint8Array Base64/Hex directories: 140 passed / 0 failed (140 scenarios, 70 files; 25/25 controls).
  - `test/typedarray/uint8array_base64_hex.js` (9/9 tests covering RFC 4648, 0..512 SIMD boundaries, 64 KiB `Uint8VirtualArray` + GC, `lastChunkHandling`, `writes-up-to-error`, immutable/detach/RAB option getters, cross-realm) + `test/typedarray` regression suite: 62/62 PASS.
  - `mise exec -- zig build -Dsanitize=address` and `-Dsanitize=undefined`: linked cleanly with host clang++ and passed both `test/typedarray/uint8array_base64_hex.js` (9/9) and Test262 (140/140).
  - `nm -gU zig-out/lib/libChakraCore.dylib | grep zig_u8a`: empty (zero leaked `zig_u8a_*` exports).
  - Cross-target compile check `zig build-obj -target x86_64-linux-gnu -OReleaseFast -fPIC lib/ZigCodec/u8a_codec.zig`: exited 0; full Linux ELF link (`tools/build/linux.zig`) verified by static inspection and metadata-pin unit tests (`test_build_tools.py`), awaiting Linux container run for full x86_64-linux-gnu runtime execution.
```

## 4. Failure Semantics (`FromBase64`, `FromHex`)

- **Base64 decode (`zig_u8a_decode_base64`)**:
  - Skips only ASCII HTTP/HTML whitespace `'\t'` (`0x09`), `'\n'` (`0x0A`), `'\f'` (`0x0C`), `'\r'` (`0x0D`), and `' '` (`0x20`). Rejects `'\v'`, `'\u00a0'`, surrogates, and all other non-alphabet code units with `ZIG_U8A_SYNTAX_ERROR`.
  - Enforces exact `lastChunkHandling`: `"loose"`, `"strict"` (padding required + zero extra bits), and `"stop-before-partial"` (stops cleanly at end of last complete 4-char chunk without consuming trailing partial chars, while rejecting invalid characters or incomplete padding before partial detection completes).
  - Applies `dst_cap` lookahead (`maxLength == 0` returns `read = 0, written = 0`; `remaining == 1` or `2` never consumes a 4-char unpadded chunk that would yield 3 bytes).
- **Hex decode (`zig_u8a_decode_hex`)**:
  - Strict `[0-9a-fA-F]` pairs with NO whitespace skipping.
  - Odd `src_len` returns `ZIG_U8A_SYNTAX_ERROR` immediately with `read = 0, written = 0` without writing `dst`.
  - Invalid hex characters in an even-length string write valid preceding byte pairs up to the error pair (`writes-up-to-error`) and return `ZIG_U8A_SYNTAX_ERROR`.
