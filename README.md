# ChakraCore

[![Discord Chat](https://img.shields.io/discord/695166668967510077?label=Discord&logo=Discord)](https://discord.gg/dgRawPdNuC)
[![Licensed under the MIT License](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/Microsoft/ChakraCore/blob/master/LICENSE.txt)
[![PR's Welcome](https://img.shields.io/badge/PRs%20-welcome-brightgreen.svg)](#contribute)

ChakraCore is a JavaScript engine with a C API you can use to add support for JavaScript to any C or C compatible project. It can be compiled for x64 processors on Linux macOS and Windows. And x86 and ARM for Windows only. It is a future goal to support x86 and ARM processors on Linux and ARM on macOS. 

## Future of ChakraCore

As you may have heard Microsoft Edge no longer uses Chakra. Microsoft will continue to provide security updates for ChakraCore 1.11 until 9th March 2021 but do not intend to support it after that.

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

> **This branch (`modernize/baseline`)** builds on macOS ARM64 with the Zig
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
> mise exec -- zig build check -Dprofile=release-shared
> mise exec -- zig build check -Dprofile=jit   # ARM64 macOS JIT smoke + bytecode gates
> ```
>
> The legacy CMake/MSBuild instructions below remain the reference for the
> upstream `master` build and for platforms the Zig build does not cover yet
> (Linux behavior is gated on the B4 milestone; Windows is B6).

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

ChakraCore on this branch can host Node-API addons through a separate
shared library, `libChakraNodeApi`, implemented on ChakraCore's JsRT C
API. The provider **implements the Node-API v8 engine surface
(`js_native_api.h`) plus the plan §4 Table B functions under the §3.2 host
contract**. This does not provide a Node.js runtime: there is no
`require`/CommonJS resolution, `process`, Node built-ins, timers, a
`node` executable, or npm, and the provider must never be described as
Node runtime compatibility. Addons requesting `NAPI_VERSION` >
8 are rejected at load; v9/v10 symbols are absent.

Known non-conformances (see `doc/node-api-receipt.md` for the
probe-backed record):

- No JS `Buffer` global. Provider Buffers are `Uint8Array` views
  marked by a provider-private symbol (`napi_is_buffer` checks the
  marker); `typeof Buffer === 'undefined'`.
- `BigInt64`/`BigUint64` typed arrays return `napi_generic_failure`.
- Async work / thread-safe functions / `napi_get_uv_event_loop`
  return `napi_generic_failure` unless the host injects the §3.2
  scheduling/uv services (the tree's `ch-nodeapi` test host injects
  none, so they fail there by design).
- `napi_async_init`/`napi_async_destroy` bookkeeping only:
  async_hooks propagation is a documented no-op.

Build and gate the provider on macOS arm64 (Zig 0.17.0 via mise,
always through the gate lock `/tmp/opencode/cc-gate-lock.sh`):

```sh
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build libChakraNodeApi
# installs lib/libChakraNodeApi.dylib + include/nodeapi/ (test profile)
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build check-nodeapi
# verifies exactly 145 napi_* + 5 host = 150 exports (fails on missing/extra)
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build check-nodeapi -Dprofile=jit
```

The exports gate (`check-nodeapi`) is a separate step on purpose
(plan PR-10): it runs for the shared profiles (`test`, `jit`) and is
not part of `zig build check`, so engine regression pins in
`tools/build/gate_counts.json` are untouched. A from-scratch consumer
(`zig cc -DNAPI_VERSION=8 -I<installed>/include/nodeapi`, linking the
built provider, run through `tools/nodeapi/ch-nodeapi` — no Node
executable) is the packaging proof; macOS arm64 probe tallies,
the consumer proof log, and the honest Linux x64 status (not run,
provider not wired in `tools/build/linux.zig`) live in
`doc/node-api-receipt.md`.

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
