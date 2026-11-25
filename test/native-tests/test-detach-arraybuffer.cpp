//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Focused C++ JsRT regression test for ArrayBuffer detach/release ownership
// and WebAssemblyArrayBuffer rejection in JsDetachArrayBuffer:
// 1. Non-externalized JsCreateArrayBuffer(65536) + JsDetachArrayBuffer (120 iters + GC + dispose),
//    plus TypedArray/DataView view invalidation, 0-byte, resizable, and immutable checks.
// 2. JsCreateExternalArrayBuffer + JsDetachArrayBuffer (120 iters): finalizeCallback stays
//    preserved until GC/dispose and runs exactly once per buffer; 0-byte nullptr external buffer.
// 3. JsCreateArrayBuffer(65536) + JsExternalizeArrayBuffer + JsDetachArrayBuffer + free (120 iters).
// 4. JsDetachArrayBuffer(wasmMemory.buffer) returns JsErrorInvalidArgument and WASM memory
//    remains callable and uncorrupted (including after Memory.prototype.grow).

#include "ChakraCore.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define CHECK_EC(expr) \
    do { \
        JsErrorCode ec__ = (expr); \
        if (ec__ != JsNoError) { \
            fprintf(stderr, "FAIL: %s returned error 0x%x at line %d\n", #expr, (unsigned)ec__, __LINE__); \
            return 1; \
        } \
    } while (0)

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s at line %d\n", (msg), __LINE__); \
            return 1; \
        } \
    } while (0)

#if defined(__clang__) || defined(__GNUC__)
#define DETACH_TEST_NOINLINE __attribute__((noinline))
#else
#define DETACH_TEST_NOINLINE
#endif

static DETACH_TEST_NOINLINE void ScrubStack()
{
    volatile unsigned char buf[4096];
    for (size_t i = 0; i < sizeof(buf); i++)
    {
        buf[i] = 0;
    }
}

static int RunUtf8(const char* code, JsValueRef* outResult = nullptr)
{
    JsValueRef fname = JS_INVALID_REFERENCE;
    JsValueRef src = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateString("test_detach_ab.js", 17, &fname));
    CHECK_EC(JsCreateString(code, strlen(code), &src));
    CHECK_EC(JsRun(src, JS_SOURCE_CONTEXT_NONE, fname, JsParseScriptAttributeNone, outResult));
    return 0;
}

static DETACH_TEST_NOINLINE int DetachOneRegularBuffer(unsigned int byteLength)
{
    JsValueRef ab = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateArrayBuffer(byteLength, &ab));

    BYTE* ptr = nullptr;
    unsigned int len = 0;
    CHECK_EC(JsGetArrayBufferStorage(ab, &ptr, &len));
    if (byteLength > 0)
    {
        CHECK(ptr != nullptr && len == byteLength, "expected non-null backing storage");
        ptr[0] = 0x11;
        ptr[byteLength - 1] = 0x22;
    }
    else
    {
        CHECK(len == 0, "expected 0 length for 0-byte buffer");
    }

    CHECK_EC(JsDetachArrayBuffer(ab));
    CHECK(JsDetachArrayBuffer(ab) == JsErrorInvalidArgument,
        "second JsDetachArrayBuffer must return JsErrorInvalidArgument");

    BYTE* detachedPtr = reinterpret_cast<BYTE*>(0x1);
    unsigned int detachedLen = 999;
    CHECK_EC(JsGetArrayBufferStorage(ab, &detachedPtr, &detachedLen));
    CHECK(detachedPtr == nullptr && detachedLen == 0,
        "detached ArrayBuffer must report NULL storage and 0 length");
    return 0;
}

static int TestNonExternalizedDetach()
{
    JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef ctx = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
    CHECK_EC(JsCreateContext(rt, &ctx));
    CHECK_EC(JsSetCurrentContext(ctx));

    // Verify TypedArray and DataView parent invalidation on JsDetachArrayBuffer.
    {
        JsValueRef ab = JS_INVALID_REFERENCE;
        JsValueRef ta = JS_INVALID_REFERENCE;
        JsValueRef dv = JS_INVALID_REFERENCE;
        CHECK_EC(JsCreateArrayBuffer(64, &ab));
        CHECK_EC(JsCreateTypedArray(JsArrayTypeUint32, ab, 4, 4, &ta));
        CHECK_EC(JsCreateDataView(ab, 0, 16, &dv));

        BYTE* abPtr = nullptr;
        unsigned int abLen = 0;
        CHECK_EC(JsGetArrayBufferStorage(ab, &abPtr, &abLen));
        CHECK(abPtr != nullptr && abLen == 64, "expected 64-byte buffer");
        abPtr[4] = 0x44;
        abPtr[5] = 0x33;
        abPtr[6] = 0x22;
        abPtr[7] = 0x11;

        JsValueRef global = JS_INVALID_REFERENCE;
        JsPropertyIdRef propAb = JS_INVALID_REFERENCE;
        JsPropertyIdRef propTa = JS_INVALID_REFERENCE;
        JsPropertyIdRef propDv = JS_INVALID_REFERENCE;
        CHECK_EC(JsGetGlobalObject(&global));
        CHECK_EC(JsCreatePropertyId("__ab", 4, &propAb));
        CHECK_EC(JsCreatePropertyId("__ta", 4, &propTa));
        CHECK_EC(JsCreatePropertyId("__dv", 4, &propDv));
        CHECK_EC(JsSetProperty(global, propAb, ab, true));
        CHECK_EC(JsSetProperty(global, propTa, ta, true));
        CHECK_EC(JsSetProperty(global, propDv, dv, true));

        CHECK(RunUtf8(
            "if (__ta[0] !== 0x11223344 || __dv.getUint32(4, true) !== 0x11223344) {"
            "  throw new Error('pre-detach view read failed');"
            "}") == 0, "pre-detach view script failed");

        CHECK_EC(JsDetachArrayBuffer(ab));

        BYTE* taPtr = nullptr;
        unsigned int taLen = 999;
        CHECK_EC(JsGetTypedArrayStorage(ta, &taPtr, &taLen, nullptr, nullptr));
        CHECK(taLen == 0 && taPtr != abPtr + 4, "detached TypedArray must clear buffer pointer and length");

        CHECK(RunUtf8(
            "if (!__ab.detached || __ab.byteLength !== 0) throw new Error('ab not detached');"
            "if (__ta.length !== 0 || __ta[0] !== undefined) throw new Error('ta not detached');"
            "var dvThrew = false;"
            "try { __dv.getInt32(0); } catch (e) { dvThrew = e instanceof TypeError; }"
            "if (!dvThrew) throw new Error('DataView.getInt32 must throw TypeError after detach');"
            "var dvLenThrew = false;"
            "try { void __dv.byteLength; } catch (e) { dvLenThrew = e instanceof TypeError; }"
            "if (!dvLenThrew) throw new Error('DataView.byteLength must throw TypeError after detach');"
            "__ab = null; __ta = null; __dv = null;") == 0, "post-detach view script failed");
    }

    CHECK(DetachOneRegularBuffer(0) == 0, "0-byte detach failed");

    // 120 iterations + GC: exposes any leaked RefCountedBuffer or backing buffer.
    for (int i = 0; i < 120; i++)
    {
        CHECK(DetachOneRegularBuffer(65536) == 0, "regular 64K detach failed");
        if ((i % 30) == 29)
        {
            ScrubStack();
            CHECK_EC(JsCollectGarbage(rt));
        }
    }

    // Resizable & immutable ArrayBuffer handling (when supported by the runtime).
    for (int i = 0; i < 20; i++)
    {
        JsValueRef rab = JS_INVALID_REFERENCE;
        CHECK(RunUtf8(
            "(() => {"
            "  try { return new ArrayBuffer(128, { maxByteLength: 65536 }); } catch (e) { return null; }"
            "})()", &rab) == 0, "resizable ArrayBuffer eval failed");
        JsValueType vt = JsUndefined;
        CHECK_EC(JsGetValueType(rab, &vt));
        if (vt == JsArrayBuffer)
        {
            CHECK_EC(JsDetachArrayBuffer(rab));
            CHECK(JsDetachArrayBuffer(rab) == JsErrorInvalidArgument,
                "second detach on resizable ArrayBuffer must fail");
        }
    }

    {
        JsValueRef imm = JS_INVALID_REFERENCE;
        CHECK(RunUtf8(
            "(() => {"
            "  var b = new ArrayBuffer(16);"
            "  return typeof b.transferToImmutable === 'function' ? b.transferToImmutable() : null;"
            "})()", &imm) == 0, "immutable ArrayBuffer eval failed");
        JsValueType vt = JsUndefined;
        CHECK_EC(JsGetValueType(imm, &vt));
        if (vt == JsArrayBuffer)
        {
            CHECK(JsDetachArrayBuffer(imm) == JsErrorInvalidArgument,
                "JsDetachArrayBuffer must reject immutable ArrayBuffer");
        }
    }

    ScrubStack();
    CHECK_EC(JsCollectGarbage(rt));
    CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
    CHECK_EC(JsDisposeRuntime(rt));
    return 0;
}

struct ExtBufferState
{
    uint32_t magic;
    int finalizeCount;
    void* bytes;
};

static void CHAKRA_CALLBACK OnExternalBufferFinalize(void* data)
{
    ExtBufferState* st = static_cast<ExtBufferState*>(data);
    if (st == nullptr || st->magic != 0xC0DEC0DEu)
    {
        abort();
    }
    st->finalizeCount++;
    if (st->bytes != nullptr)
    {
        free(st->bytes);
        st->bytes = nullptr;
    }
}

static DETACH_TEST_NOINLINE int CreateAndDetachExternalOne(ExtBufferState* st, unsigned int byteLength)
{
    if (byteLength > 0)
    {
        st->bytes = malloc(byteLength);
        CHECK(st->bytes != nullptr, "malloc failed");
        memset(st->bytes, 0x3C, byteLength);
    }
    JsValueRef ab = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateExternalArrayBuffer(st->bytes, byteLength, OnExternalBufferFinalize, st, &ab));
    CHECK_EC(JsDetachArrayBuffer(ab));

    // JsrtExternalArrayBuffer retains (finalizeCallback, callbackState) until GC Finalize.
    CHECK(st->finalizeCount == 0, "JsDetachArrayBuffer must not invoke JsrtExternalArrayBuffer finalizer prematurely");
    CHECK(JsDetachArrayBuffer(ab) == JsErrorInvalidArgument, "second detach on external AB must fail");

    BYTE* detachedPtr = reinterpret_cast<BYTE*>(0x1);
    unsigned int detachedLen = 999;
    CHECK_EC(JsGetArrayBufferStorage(ab, &detachedPtr, &detachedLen));
    CHECK(detachedPtr == nullptr && detachedLen == 0, "detached external AB must report NULL/0");
    return 0;
}

static int TestExternalArrayBufferDetach()
{
    const int kCount = 120;
    std::vector<ExtBufferState> states(kCount);
    for (int i = 0; i < kCount; i++)
    {
        states[i].magic = 0xC0DEC0DEu;
        states[i].finalizeCount = 0;
        states[i].bytes = nullptr;
    }
    ExtBufferState zeroState = { 0xC0DEC0DEu, 0, nullptr };

    JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef ctx = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
    CHECK_EC(JsCreateContext(rt, &ctx));
    CHECK_EC(JsSetCurrentContext(ctx));

    CHECK(CreateAndDetachExternalOne(&zeroState, 0) == 0, "0-byte external AB detach failed");

    for (int i = 0; i < kCount; i++)
    {
        CHECK(CreateAndDetachExternalOne(&states[i], 65536) == 0, "external AB detach failed");
        if ((i % 30) == 29)
        {
            ScrubStack();
            CHECK_EC(JsCollectGarbage(rt));
        }
    }

    ScrubStack();
    CHECK_EC(JsCollectGarbage(rt));
    CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
    CHECK_EC(JsDisposeRuntime(rt));

    CHECK(zeroState.finalizeCount == 1, "0-byte detached external AB must finalize exactly once");
    for (int i = 0; i < kCount; i++)
    {
        CHECK(states[i].finalizeCount == 1 && states[i].bytes == nullptr,
            "detached external AB must invoke finalizeCallback exactly once");
    }
    return 0;
}

static DETACH_TEST_NOINLINE int ExternalizeAndDetachOne()
{
    JsValueRef ab = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateArrayBuffer(65536, &ab));

    BYTE* ptr = nullptr;
    unsigned int len = 0;
    CHECK_EC(JsGetArrayBufferStorage(ab, &ptr, &len));
    CHECK(ptr != nullptr && len == 65536, "expected 65536-byte storage");
    ptr[0] = 0x5A;
    ptr[65535] = 0xA5;

    ArrayBufferFreeFn freeFn = nullptr;
    CHECK_EC(JsGetArrayBufferFreeFunction(ab, &freeFn));
    CHECK(freeFn != nullptr, "expected non-null ArrayBufferFreeFn");

    CHECK_EC(JsExternalizeArrayBuffer(ab));
    CHECK_EC(JsDetachArrayBuffer(ab));
    CHECK(JsDetachArrayBuffer(ab) == JsErrorInvalidArgument, "second detach on externalized AB must fail");

    BYTE* detachedPtr = reinterpret_cast<BYTE*>(0x1);
    unsigned int detachedLen = 999;
    CHECK_EC(JsGetArrayBufferStorage(ab, &detachedPtr, &detachedLen));
    CHECK(detachedPtr == nullptr && detachedLen == 0, "detached externalized AB must report NULL/0");

    // Host retains ownership of externalized bytes across DetachAndRelease().
    CHECK(ptr[0] == 0x5A && ptr[65535] == 0xA5, "externalized host bytes must remain intact until host frees");
    freeFn(ptr);
    return 0;
}

static int TestExternalizedArrayBufferDetach()
{
    JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef ctx = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
    CHECK_EC(JsCreateContext(rt, &ctx));
    CHECK_EC(JsSetCurrentContext(ctx));

    for (int i = 0; i < 120; i++)
    {
        CHECK(ExternalizeAndDetachOne() == 0, "externalized AB detach failed");
        if ((i % 30) == 29)
        {
            ScrubStack();
            CHECK_EC(JsCollectGarbage(rt));
        }
    }

    ScrubStack();
    CHECK_EC(JsCollectGarbage(rt));
    CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
    CHECK_EC(JsDisposeRuntime(rt));
    return 0;
}

static int TestWasmMemoryBufferDetach()
{
    JsRuntimeHandle rt = JS_INVALID_RUNTIME_HANDLE;
    JsContextRef ctx = JS_INVALID_REFERENCE;
    CHECK_EC(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt));
    CHECK_EC(JsCreateContext(rt, &ctx));
    CHECK_EC(JsSetCurrentContext(ctx));

    JsValueRef hasWasm = JS_INVALID_REFERENCE;
    CHECK(RunUtf8(
        "typeof WebAssembly === 'object' && typeof WebAssembly.Memory === 'function'",
        &hasWasm) == 0, "wasm probe failed");
    bool wasmEnabled = false;
    CHECK_EC(JsBooleanToBool(hasWasm, &wasmEnabled));

    if (wasmEnabled)
    {
        // Minimal WASM module importing env.mem (1 page) and exporting:
        //   rw(addr, val) -> i32 (i32.store + i32.load)
        //   sz() -> i32 (memory.size)
        CHECK(RunUtf8(
            "var __wasmMem = new WebAssembly.Memory({ initial: 1 });"
            "var __wasmBytes = new Uint8Array(["
            "  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,"
            "  0x01, 0x0b, 0x02, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f,"
            "  0x02, 0x0c, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00, 0x01,"
            "  0x03, 0x03, 0x02, 0x00, 0x01,"
            "  0x07, 0x0b, 0x02, 0x02, 0x72, 0x77, 0x00, 0x00, 0x02, 0x73, 0x7a, 0x00, 0x01,"
            "  0x0a, 0x15, 0x02,"
            "    0x0e, 0x00, 0x20, 0x00, 0x20, 0x01, 0x36, 0x02, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,"
            "    0x04, 0x00, 0x3f, 0x00, 0x0b"
            "]);"
            "var __wasmInst = new WebAssembly.Instance(new WebAssembly.Module(__wasmBytes), { env: { mem: __wasmMem } });"
            "if (__wasmInst.exports.rw(16, 0x12345678) !== 0x12345678 || __wasmInst.exports.sz() !== 1) {"
            "  throw new Error('initial wasm check failed');"
            "}"
            "var __wasmBuf = __wasmMem.buffer;") == 0, "wasm setup failed");

        JsValueRef global = JS_INVALID_REFERENCE;
        JsPropertyIdRef propBuf = JS_INVALID_REFERENCE;
        JsValueRef wasmBuf = JS_INVALID_REFERENCE;
        CHECK_EC(JsGetGlobalObject(&global));
        CHECK_EC(JsCreatePropertyId("__wasmBuf", 9, &propBuf));
        CHECK_EC(JsGetProperty(global, propBuf, &wasmBuf));

        CHECK(JsDetachArrayBuffer(wasmBuf) == JsErrorInvalidArgument,
            "JsDetachArrayBuffer(wasmMemory.buffer) must return JsErrorInvalidArgument");

        CHECK(RunUtf8(
            "if (__wasmMem.buffer.detached !== false || __wasmMem.buffer.byteLength !== 65536) {"
            "  throw new Error('wasmMemory.buffer state corrupted after rejected detach');"
            "}"
            "if (__wasmInst.exports.rw(16, 0x0badcafe) !== 0x0badcafe) {"
            "  throw new Error('wasm export rw corrupted');"
            "}"
            "if (new Int32Array(__wasmMem.buffer)[4] !== 0x0badcafe || __wasmInst.exports.sz() !== 1) {"
            "  throw new Error('wasm memory view mismatch');"
            "}"
            "__wasmMem.grow(1);"
            "var __wasmBuf2 = __wasmMem.buffer;") == 0, "post-reject wasm check failed");

        JsPropertyIdRef propBuf2 = JS_INVALID_REFERENCE;
        JsValueRef wasmBuf2 = JS_INVALID_REFERENCE;
        CHECK_EC(JsCreatePropertyId("__wasmBuf2", 10, &propBuf2));
        CHECK_EC(JsGetProperty(global, propBuf2, &wasmBuf2));

        CHECK(JsDetachArrayBuffer(wasmBuf) == JsErrorInvalidArgument,
            "JsDetachArrayBuffer on grown/old wasm buffer must return JsErrorInvalidArgument");
        CHECK(JsDetachArrayBuffer(wasmBuf2) == JsErrorInvalidArgument,
            "JsDetachArrayBuffer on grown wasmMemory.buffer must return JsErrorInvalidArgument");

        CHECK(RunUtf8(
            "if (__wasmInst.exports.sz() !== 2 || __wasmInst.exports.rw(65540, 42) !== 42) {"
            "  throw new Error('wasm memory corrupted after grow + rejected detach');"
            "}"
            "__wasmBuf = null; __wasmBuf2 = null; __wasmInst = null; __wasmMem = null;") == 0,
            "post-grow wasm check failed");
    }

    ScrubStack();
    CHECK_EC(JsCollectGarbage(rt));
    CHECK_EC(JsSetCurrentContext(JS_INVALID_REFERENCE));
    CHECK_EC(JsDisposeRuntime(rt));
    return 0;
}

int main()
{
    if (TestNonExternalizedDetach() != 0)
    {
        return 1;
    }
    if (TestExternalArrayBufferDetach() != 0)
    {
        return 1;
    }
    if (TestExternalizedArrayBufferDetach() != 0)
    {
        return 1;
    }
    if (TestWasmMemoryBufferDetach() != 0)
    {
        return 1;
    }

    printf("PASS\n");
    return 0;
}
