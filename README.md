# ChakraCore

[![Discord Chat](https://img.shields.io/discord/695166668967510077?label=Discord&logo=Discord)](https://discord.gg/dgRawPdNuC)
[![Licensed under the MIT License](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/Microsoft/ChakraCore/blob/master/LICENSE.txt)
[![PR's Welcome](https://img.shields.io/badge/PRs%20-welcome-brightgreen.svg)](#contribute)

ChakraCore is a compact, embeddable JavaScript and WebAssembly engine with a flat C API (`JsRT`) and a separate Node-API v10 addon provider (`libChakraNodeApi` on `modernize/phase4`; v8 on `modernize/baseline`). On `modernize/baseline`, it builds hermetically with Zig `0.17.0` on **macOS ARM64** and **Linux x64** (`~5.3 MB` release package, `98.89%` certified ES2025 edition conformance on macOS ARM64, 3-tier JIT + concurrent GC, WASM multi-value + SIMD JIT).

## Project Direction: The Embeddable JIT Sweet Spot

This fork targets the middle ground between interpreter-only embeds and heavyweight browser engines: sub-minute Zig builds, compact binaries, flat C ABIs (`JsRT` + `libChakraNodeApi`) with zero `v8.h` shims, multi-tier x64/ARM64 JIT + WASM SIMD, and optional Zig leaf/host integrations outside the C++ engine core. See [doc/vision-and-roadmap.md](doc/vision-and-roadmap.md) for the Phase-4 roadmap (`Q1.1` Node-API v9/v10 + multi-addon support landed on `modernize/phase4`; remaining tracks cover ES2025/ES2026 quick wins, `Z1` Base64/Hex Zig C-ABI wedge, modern `Intl`, and a standalone Zig + `libuv` runtime host) and [doc/modernization-plan.md](doc/modernization-plan.md) for verified gates.

## Upstream History

As you may have heard Microsoft Edge no longer uses Chakra. Microsoft stopped actively maintaining upstream ChakraCore 1.11 after March 2021.

ChakraCore is planned to continue as a community project targeted primarily at embedded use cases. We hope to produce future releases with new features and enhancements to support such use cases. We also would like to invite any interested parties to be involved in this project. For further details please see the following draft planning documents:
[Overall plan](https://github.com/chakra-core/org/blob/master/ChakraCore%20Future%20Plan.md)
[Version 1.12 plan](https://github.com/chakra-core/org/blob/master/Release%201.12%20plan.md)

Also see discussion in issue [#6384](https://github.com/microsoft/ChakraCore/issues/6384)

If you'd like to contact the community team please either open an issue or join the Discord chat linked above.

## Security

If you believe you have found a security issue in ChakraCore 1.11, please share it with Microsoft privately following the guidance at the Microsoft [Security TechCenter](https://technet.microsoft.com/en-us/security/ff852094). Reporting it via this channel helps minimize risk to projects built with ChakraCore.

If you find a security issue in the Master branch of Chakracore but not in 1.11 please join our Discord server and private message one of the Core team members.

## Documentation

* [ChakraCore Architecture](https://github.com/chakra-core/ChakraCore/wiki/Architecture-Overview)
* [Quickstart Embedding ChakraCore](https://github.com/chakra-core/ChakraCore/wiki/Embedding-ChakraCore)
* [API Reference](https://github.com/chakra-core/ChakraCore/wiki/JavaScript-Runtime-%28JSRT%29-Reference)
* [Contribution guidelines](CONTRIBUTING.md)
* [Blogs, talks and other resources](https://github.com/chakra-core/ChakraCore/wiki/Resources)
* [Modernization documentation and verified baselines](doc/README.md)

## Building ChakraCore

> **This branch (`modernize/phase4`, branched from `modernize/baseline` `35f53eae0` with `Q1.1` landed)** builds on macOS ARM64 with the Zig
> 0.17.0 front end pinned in `mise.toml` (shared/no-JIT profiles plus the
> ARM64 macOS `jit` profile merged 2026-10-31; see
> [doc/modernization-plan.md](doc/modernization-plan.md) for status,
> [doc/arm64-macos-jit.md](doc/arm64-macos-jit.md) for the JIT record and
> coverage limits, and [doc/README.md](doc/README.md) for the doc index):
>
> ```sh
> mise exec -- zig build                 # default (test) profile
> mise exec -- zig build check           # regression + bytecode + native + smoke + metadata gates
> mise exec -- zig build check -Dprofile=debug-static
> mise exec -- zig build -Dprofile=release-shared check-package
> mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-fulljit
> mise exec -- zig build -Dsanitize=address check-sanitize
> mise exec -- zig build -Dsanitize=undefined check-sanitize
> ```
>
> Linux x64 builds via `tools/build/linux.zig` ([doc/linux-handoff.md](doc/linux-handoff.md) §0);
> legacy CMake/MSBuild instructions below remain the reference for Windows (B6)
> and upstream comparisons.

You can build ChakraCore on Windows 7 SP1 or above, and Windows Server 2008 R2 or above, with either Visual Studio 2015 or 2017 with C++ support installed.  Once you have Visual Studio installed:

* Clone ChakraCore through ```git clone https://github.com/Microsoft/ChakraCore.git```
* Open `Build\Chakra.Core.sln` in Visual Studio
* Build Solution

On macOS you can build ChakraCore with the xcode command line tools and `cmake`.
On Linux you can build ChakraCore with `cmake` and `ninja`.

More details in [Building ChakraCore](https://github.com/chakra-core/ChakraCore/wiki/Building-ChakraCore).

Alternatively, see [Getting ChakraCore binaries](https://github.com/Microsoft/ChakraCore/wiki/Getting-ChakraCore-binaries) for pre-built ChakraCore binaries.

## Using ChakraCore

Once built, you have a few options for how you can use ChakraCore:

* The most basic is to test the engine is running correctly with the application *ch.exe* (ch on linux or macOS).  This app is a lightweight host of ChakraCore that you can use to run small applications.  After building, you can find this binary in:
  * Windows: `Build\VcBuild\bin\${platform}_${configuration}` (e.g. `Build\VcBuild\bin\x64_debug`)
  * macOS/Linux: `buildFolder/config/ch` (e.g. `out/Release/ch`)
* You can [embed ChakraCore](https://github.com/chakra-core/ChakraCore/wiki/Embedding-ChakraCore) in your applications - see [documentation](https://github.com/chakra-core/ChakraCore/wiki/Embedding-ChakraCore) and [samples](https://aka.ms/chakracoresamples).

_A note about using ChakraCore_: ChakraCore is a JavaScript engine, it does not include the external APIs that are provided by a Web Browser or Node.js.  For example, DOM APIs like ```document.write()``` are additional APIs that are not provided by ChakraCore, when embedding ChakraCore in an application you will need to implement your own input and output APIs. For debugging, in `ch` you can use ```print()``` to put text to the terminal.

Alternatively, if you are using the [vcpkg](https://github.com/Microsoft/vcpkg/) dependency manager you can download and install ChakraCore with CMake integration in a single command:
* vcpkg install chakracore

## Node-API provider (`libChakraNodeApi`)

ChakraCore on this branch can host multiple Node-API addons in one
process through a separate shared library, `libChakraNodeApi`,
implemented on ChakraCore's JsRT C API. The provider **implements the
Node-API v10 engine surface (`js_native_api.h`, 123 symbols) plus the
plan §4 Table B functions (`node_api.h`, 32 symbols) under the §3.2 host
contract**. This does not provide a Node.js runtime: there is no
`require`/CommonJS resolution, `process`, Node built-ins, timers, a
`node` executable, or npm, and the provider must never be described as
Node runtime compatibility. Addons requesting `NAPI_VERSION` >
10 (including `NAPI_VERSION_EXPERIMENTAL`) are rejected at load.
(`Q1.1` additive `NAPI_VERSION=9/10` exports and multi-addon registry
support landed on `modernize/phase4`; a separate Zig + `libuv` host
runtime [`H1`] is documented in
[doc/vision-and-roadmap.md](doc/vision-and-roadmap.md).)

Known non-conformances (see [doc/node-api-receipt.md](doc/node-api-receipt.md) for the
probe-backed record):

- No JS `Buffer` global. Provider Buffers are `Uint8Array` views
  marked by a per-context provider-private symbol (shared across
  coexisting `napi_env` instances on the same `(runtime, context)` so
  cross-addon `napi_is_buffer` checks pass); `typeof Buffer ===
  'undefined'`.
- `Float16Array` (JS-created) returns `napi_generic_failure` in
  `napi_get_typedarray_info` because `NAPI_VERSION=10` defines no float16
  typed-array enum value. (`BigInt64Array` / `BigUint64Array` are wired
  directly through Phase-3 JsRT.)
- External Latin-1 / UTF-16 string creation
  (`node_api_create_external_string_{latin1,utf16}`) always copies into
  ChakraCore's Recycler string heap (`*copied = true`) and invokes the
  finalizer synchronously.
- Async work / thread-safe functions / `napi_get_uv_event_loop`
  return `napi_generic_failure` unless the host injects the §3.2
  scheduling/uv services (the tree's `tools/nodeapi/ch-nodeapi` test host
  injects `post_task`/`post_work`/`cancel_work`/`ref_loop`/`unref_loop`,
  with `uv_loop = NULL` so `napi_get_uv_event_loop` still fails cleanly).
- `napi_async_init`/`napi_async_destroy` bookkeeping only:
  async_hooks propagation is a documented no-op.

Build and gate the provider on macOS arm64 (Zig 0.17.0 via mise):

```sh
mise exec -- zig build libChakraNodeApi
# installs lib/libChakraNodeApi.dylib + include/nodeapi/ (test profile)
mise exec -- zig build check-nodeapi
# verifies exactly 155 napi_* + 6 host = 161 exports (fails on missing/extra)
mise exec -- zig build check-nodeapi -Dprofile=jit
```

The exports gate (`check-nodeapi`) is a separate step on purpose
(plan PR-10): it runs for the shared profiles (`test`, `jit`, and
`release-shared`) and is not part of `zig build check`, so engine regression
pins in `tools/build/gate_counts.json` are untouched. A from-scratch consumer
(`zig cc -DNAPI_VERSION=10 -I<installed>/include/nodeapi`, linking the
built provider, run through `tools/nodeapi/ch-nodeapi` — no Node
executable) is the packaging proof; macOS arm64 probe tallies (19 probes +
`tools/nodeapi/run_probe.sh`, stress, and conformance matrices), the consumer
proof log, and the Linux x64 status (wired in `tools/build/linux.zig` and
`.github/workflows/zig-linux.yml`, awaiting native Linux execution) live in
[doc/node-api-receipt.md](doc/node-api-receipt.md).

## Contribute

Contributions to ChakraCore are welcome.  Here is how you can contribute to ChakraCore:

* [Submit bugs](https://github.com/chakra-core/ChakraCore/issues) and help us verify fixes.
* [Submit pull requests](https://github.com/chakra-core/ChakraCore/pulls) for bug fixes and features and discuss existing proposals

Please refer to [Contribution Guidelines](CONTRIBUTING.md) for more details.

## License

Code licensed under the [MIT License](https://github.com/chakra-core/ChakraCore/blob/master/LICENSE.txt).

## Contact Us

If you have questions about ChakraCore, or you would like to reach out to us about an issue you're having or for development advice as you work on a ChakraCore issue, you can reach us as follows:

* Open an [issue](https://github.com/chakra-core/ChakraCore/issues/new) and prefix the issue title with [Question]. See [Question](https://github.com/chakra-core/ChakraCore/issues?q=label%3AQuestion) tag for already-opened questions.
* Discuss ChakraCore with the team and the community via the Discord link above
