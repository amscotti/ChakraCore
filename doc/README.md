# ChakraCore Documentation

In-repo documentation for the ChakraCore modernization effort, pinned to HEAD
`97e68c917` (see [current-state.md](current-state.md) §7 for activity context).

| Document | Contents |
|---|---|
| [current-state.md](current-state.md) | What the project is today: inventory, architecture/build/test summaries, platforms, activity, constraints, gaps |
| [architecture.md](architecture.md) | Engine architecture: component map, execution pipeline, JIT tiers, JsRT API, threading, GC, Intl/WASM, with a Mermaid diagram |
| [build.md](build.md) | Build process: entry points, prerequisites, Unix + Windows builds, CMake structure, ICU matrix, codegen, outputs, testing, CI, staleness |
| [modernization-plan.md](modernization-plan.md) | Modernization plan: Zig 0.17 build adoption (Phase 1), incremental Zig code (Phase 2), feature gaps vs V8 / QuickJS-ng / JavaScriptCore (Phase 3) |

Conventions:

- Every factual claim cites an in-repo `path:line` verified at the pinned HEAD.
- `build.md` commands are source-inspection verified, not run-verified.
- `[NEEDS VERIFICATION on Zig 0.17]` marks Zig-0.17 API/behavior claims researched
  against a 0.16 toolchain plus release notes — re-check after upgrading.
- `[UNVERIFIED EXTERNAL]` marks competitor-ecosystem claims without a primary
  source — keep generic until sourced.

How these were produced: parallel research agents mapped the architecture, build,
current state, and feature gaps from repo sources; independent skeptic agents
reviewed each draft against file bodies; the maintainer then resolved review
conflicts (notably `flat`/`flatMap` presence and commit-date counting) by opening
the files, and a final review pass re-checked the saved documents.
