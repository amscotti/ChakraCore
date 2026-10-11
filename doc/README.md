# ChakraCore Documentation

In-repo documentation for the ChakraCore modernization effort. Engine and legacy
build references are pinned to baseline `97e68c917`; execution results and
modernization decisions are updated through 2026-10-10. See
[current-state.md](current-state.md) §7 for activity context.

| Document | Contents |
|---|---|
| [current-state.md](current-state.md) | What the project is today: inventory, architecture/build/test summaries, platforms, activity, constraints, gaps |
| [architecture.md](architecture.md) | Engine architecture: component map, execution pipeline, JIT tiers, JsRT API, threading, GC, Intl/WASM, with a Mermaid diagram |
| [build.md](build.md) | Build process: entry points, prerequisites, Unix + Windows builds, CMake structure, ICU matrix, codegen, outputs, testing, CI, staleness |
| [modernization-plan.md](modernization-plan.md) | Executable roadmap: next B1 build slice, scoped Mac/Linux rollout, optional Zig integration, compatibility milestones and full ES2025 objective |
| [conformance-baseline.md](conformance-baseline.md) | Pinned upstream diagnostic baseline: configuration, executed/skip counts, feature debt and edition-gate limits |
| [memory-management.md](memory-management.md) | Coding-assistant ownership guide: TigerStyle/Ghostty/Bun lessons applied to allocators, GC/FFI, callbacks, async lifetime and focused tests |

Conventions:

- Engine/build claims cite in-repo `path:line` references at the pinned baseline;
  external toolchain and language-edition claims cite primary sources.
- `build.md` distinguishes source-inspected recipes from run-verified results:
  §13 records the legacy Mac baseline, §14 the Zig pilot and its corrections.
- Zig 0.17.0 APIs were checked against its installed sources and release notes.
  `[NEEDS VERIFICATION on Zig 0.17]` identifies a specific remaining experiment,
  not an unchecked API spelling. ZLS and incremental-link research do not gate
  Phase 1.
- The roadmap's milestone gates define the active implementation scope. Detailed
  inventories describe reference behavior and future work, not a requirement to
  ship every legacy option or ecosystem feature in the first migration.
- `[UNVERIFIED EXTERNAL]` marks competitor-ecosystem claims without a primary
  source — keep generic until sourced.

How these were produced: parallel research agents mapped the architecture, build,
current state, and feature gaps from repo sources; independent skeptic agents
reviewed each draft against file bodies; the maintainer then resolved review
conflicts (notably `flat`/`flatMap` presence and commit-date counting) by opening
the files, and a final review pass re-checked the saved documents.

The subsequent execution review corrected the JsRT runtime/TLS ownership model,
inherited ES2018 RegExp gaps, callback-retention API, executable stack setting,
and deployment-target assumptions. Modern platform floors are intentional policy
choices ([modernization-plan.md](modernization-plan.md) §2.1), rather than inherited
legacy requirements or the build host's defaults. The durable pilot and pinned
conformance tooling live in [tools/zig-pilot](../tools/zig-pilot/README.md) and
[tools/test262](../tools/test262/README.md).
