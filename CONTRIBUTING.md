# Contributing Code

ChakraCore accepts bug fix pull requests. For a bug fix PR to be accepted, it must first have a tracking issue that has been marked approved. Your PR should link to the bug you are fixing. If you've submitted a PR for a bug, please post a comment in the bug to avoid duplication of effort.

ChakraCore also accepts new feature pull requests. For a feature-level PR to be accepted, it first needs to have design discussion. Design discussion can take one of two forms a) a feature request in the issue tracker that has been marked as approved or b) the PR must be accompanied by a full design spec and this spec is later approved in the open design discussion. Features are evaluated against their complexity, impact on other features, roadmap alignment, and maintainability.

These two blogs posts on contributing code to open source projects are a good reference: [Open Source Contribution Etiquette](http://tirania.org/blog/archive/2010/Dec-31.html) by Miguel de Icaza and [Don't "Push" Your Pull Requests](https://www.igvita.com/2011/12/19/dont-push-your-pull-requests/) by Ilya Grigorik.

## Security

If you believe you have found a security issue in ChakraCore 1.11, please share it with Microsoft privately following the guidance at the Microsoft [Security TechCenter](https://technet.microsoft.com/en-us/security/ff852094). Reporting it via this channel helps minimize risk to projects built with ChakraCore.

If you find a security issue in the Master branch of ChakraCore but not in 1.11 please join our discord server and private message one of the Core team members.

## Legal

You will need to complete a Contribution Agreement before your pull request can be accepted. This agreement testifies that you are granting us permission to use the source code you are submitting, and that this work is being submitted under appropriate license that we can use it.

You can read the agreement here: [Contribution Agreement](ContributionAgreement.md)

## Housekeeping

Your pull request should:

* Include a description of what your change intends to do
* Be a child commit of a reasonably recent commit in the master branch
* Pass all unit tests
* Have a clear commit message
* Include adequate tests
  * At least one test should fail in the absence of your non-test code changes. If your PR does not match this criteria, please specify why
  * Tests should include reasonable permutations of the target fix/change
  * Include baseline changes with your change

Submissions that have met these requirements will be will reviewed by a core contributor. Submissions must meet functional and performance expectations, including meeting requirements in scenarios for which the team doesn’t yet have open source tests. This means you may be asked to fix and resubmit your pull request against a new open test case if it fails one of these tests.

ChakraCore is an organically grown codebase. The consistency of style reflects this. For the most part, the team follows these [coding conventions](https://github.com/chakra-core/ChakraCore/wiki/Coding-Convention). Contributors should also follow them when making submissions. Otherwise, follow the general coding conventions adhered to in the code surrounding your changes. Pull requests that reformat the code will not be accepted.

## Running the tests

The unit tests can be run offline with following these steps:

### a) Windows

Active Windows x64 builds use the Zig `0.17.0` front end from an **x64 Native Tools Command Prompt for VS 2022** with `vcpkg` `icu:x64-windows` (see [README.md](README.md) and [doc/windows-handoff.md](doc/windows-handoff.md)):

* `zig build`
* `zig build check-smoke check-metadata verify-bytecode check-jit check-selftest`

Legacy `Build\Chakra.Core.sln` and `test\runtests.cmd` remain in the tree solely as historical references and do not build `lib/ZigCodec` or `libChakraNodeApi`.

### b) Linux or macOS

Build and test ChakraCore with the Zig `0.17.0` front end pinned in [mise.toml](mise.toml) (see [README.md](README.md) for all profile quickstarts):

* `mise exec -- zig build` (default `test` profile -> `zig-out/bin/ch`)
* `mise exec -- zig build check` (regression suite, bytecode verification, native embedding tests, smoke checks, metadata, and selftests)
* `mise exec -- zig build -Dprofile=debug-static check` (static Debug archive + native suites)
* `mise exec -- zig build -Dprofile=jit -Dwasm=true check-wasm check-wasm-jit check-fulljit` (JIT + P1 WASM checks)

Alternatively, you can run `test/runtests.py` directly against a Zig-built binary, e.g. `python3 test/runtests.py -t --binary=zig-out/bin/ch` (or `out/debug-static/bin/ch` with `-d`). Read `test/runtests.py` for subset/tag switches.
Legacy `build.sh` and `cmake` (`make check` / `ninja check`) are unmaintained historical references and no longer link `lib/ZigCodec` or build `libChakraNodeApi`.

## Issue Labels

 - [`help wanted`](https://github.com/chakra-core/ChakraCore/labels/help%20wanted): these issues are specifically well suited for outside contributors.
 - [`good first issue`](https://github.com/chakra-core/ChakraCore/labels/good%20first%20issue): these issues are small and appropriate for people who wish to familiarize themselves with GitHub pull requests and/or ChakraCore's contributor guidelines, build process, and running tests.  We're here to help you get started in open source.

You are welcome to work on issues that are not tagged with these labels. However, issues without these labels may be fairly complex, therefore please discuss with a core team member via comments on the issue before attempting to solve it.

For all issues you choose to work on please communicate on the issue that you are claiming it to avoid duplicated work.

To learn more about our GitHub labels see the [Label Glossary](https://github.com/Microsoft/ChakraCore/wiki/Label-Glossary) on our wiki page.
