# H1 External Runtime Host -- Engine C-ABI & Packaging Readiness

> Audit of ChakraCore's public C ABI and `zig build package` SDK surface for building `H1` (standalone Zig + `libuv` runtime) in a **separate downstream repository**.

## 1. Already Available in Public JsRT & `libChakraNodeApi`

- **Runtime, GC, Memory & Interrupts (`ChakraCommon.h`):**
  - `JsCreateRuntime`, `JsDisposeRuntime`, `JsCreateContext`, `JsSetCurrentContext`
  - `JsGetRuntimeMemoryUsage`, `JsGetRuntimeMemoryLimit`, `JsSetRuntimeMemoryLimit`, `JsSetRuntimeMemoryAllocationCallback`
  - `JsDisableRuntimeExecution` / `JsEnableRuntimeExecution` / `JsIsRuntimeExecutionDisabled`, `JsIdle`, `JsCollectGarbage`
- **Event Loop, Promises & Timers (`ChakraCommon.h`, `ChakraCore.h`):**
  - `JsSetPromiseContinuationCallback` (microtask queue)
  - `JsSetHostPromiseRejectionTracker` (`unhandledRejection` / `rejectionHandled`)
  - `JsSetTimerContinuationCallback` + `JsSetTimerContinuationCancelCallback`
- **ES Modules & ES2025 Import Attributes (`ChakraCore.h`):**
  - `JsInitializeModuleRecord`, `JsParseModuleSource` (UTF-8 / UTF-16), `JsModuleEvaluation`, `JsSetModuleHostInfo` / `JsGetModuleHostInfo`
  - Callbacks: `FetchImportedModuleWithAttributesCallback`, `FetchImportedModuleFromScriptWithAttributesCallback`, `InitializeImportMetaCallback`, `NotifyModuleReadyCallback`, `ReportModuleCompletionCallback`
- **Zero-Copy Buffers, Tracing, `structuredClone` & `worker_threads` (`ChakraCommon.h`, `ChakraCore.h`):**
  - `JsCreateExternalArrayBuffer`, `JsExternalizeArrayBuffer`, `JsGetArrayBufferStorage`, `JsDetachArrayBuffer`
  - `JsCreateTracedExternalObject`, `JsSetRuntimeDomWrapperTracingCallbacks`
  - `JsSharedArrayBufferContentHandle` (`JsCreateSharedArrayBufferWithSharedContent`, `JsGetSharedArrayBufferContent`, `JsReleaseSharedArrayBufferContentHandle`)
  - `JsVarSerializer*` / `JsVarDeserializer*` (with transferable `ArrayBuffer` support)
- **Node-API Addon Hosting (`NodeApiHostApi.h`, `include/nodeapi/`):**
  - `161` `NAPI_VERSION=10` exports in `libChakraNodeApi`
  - `chakra_nodeapi_create_env`, `chakra_nodeapi_set_module_api_version`, `chakra_nodeapi_destroy_env`
  - `chakra_nodeapi_host_services`: `post_task`, `post_work`, `cancel_work`, `ref_loop`, `unref_loop`, `fatal_exception`, `uv_loop`, `host_manages_promise_queue`
- **Inspector / Debugger (`ChakraDebug.h`):**
  - Full `JsDiag*` C API (`JsDiagStartDebugging`, breakpoints, stepping, stack frames, property inspection, frame eval) + `JsTTD*`.

## 2. Engine / Packaging Gaps to Close for a Separate `H1` Repo

1. **Package `NodeApiHostApi.h` in the SDK tarball (`tools/package/build_package.py`, `build.zig`, `linux.zig`, `windows.zig`):**
   - `build_package.py` currently packages `ChakraCore.h`, `ChakraCommon.h`, `ChakraDebug.h`, `ChakraCoreVersion.h`, and `include/nodeapi/*.h`, while `NodeApiHostApi.h` is marked private and omitted from public header installation. An external `H1` repo needs `NodeApiHostApi.h` installed in the SDK prefix/tarball.
2. **Dynamic `uv_*` symbol visibility for `.node` addons:**
   - When `H1` embeds `libuv` and loads prebuilt `.node` addons via `dlopen`, those addons expect `uv_*` symbols at runtime (requiring `H1` to export `uv_*` or link a shared `libuv`).
3. **Optional synthetic/host-evaluated ESM namespace helper (ergonomic enhancement):**
   - `JsSetModuleHostInfo` + `JsParseModuleSource` covers source ESM/JSON/CJS facades cleanly, though a direct synthetic module / `JsGetModuleNamespace` C helper may be worth adding if `node:*` built-in loading becomes boilerplate-heavy.
