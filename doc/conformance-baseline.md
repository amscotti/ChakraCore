# Conformance diagnostic baseline

Recorded on 2026-10-10 local time (report timestamp 2026-10-11 00:00:19 UTC),
before engine feature changes. This records the declared modern Mac pilot and a
pinned upstream corpus; it is **not** a full ES2025 conformance claim.

## Configuration

| Input | Value |
|---|---|
| Engine source | `97e68c917`, ChakraCore 1.13.0 dev, no engine changes |
| Compiler | Zig 0.17.0 / Clang 22.1.8 |
| Configuration | ARM64 macOS, shared, no-JIT, RelWithDebInfo, system ICU 78 |
| Declared deployment minimum | `aarch64-macos.26.0` |
| Actual SDK / ICU versions | macOS SDK 26.5 / ICU 78.3 |
| Executable stack | 8 MiB; callback retained on `ch` |
| Host flags | `-nonative -Test262 -CollectGarbage` |
| Corpus | `tc39/test262`, `2e0a56762801e275a9fdf96dc49d90ba0cddcf63` (2026-10-08) |
| Runner | test262-harness 10.0.0 / locked eshost dependencies; Node v22.22.3 |
| Dependency-lock SHA256 | `f55201031ef666e2575706cfceaa90e48511e3ea2083ef5ec03c823e72dd7b39` |
| `ch` SHA256 | `f2817d95a838ea53065b5a04840813d9ca7e30894048193ae27ea20671a0e29e` |
| Engine dylib SHA256 | `37d68f1e88c212677ef9cd54020edd3ba1a0f0560e1d26b130ec016b3c1b17eb` |

Eight workers, 10000 ms timeout per scenario. Selected patterns:

```text
test/language/**/*.js
test/built-ins/**/*.js
test/annexB/**/*.js
```

The same pilot independently passed **1959/0** legacy regressions, embedded
bytecode verification and `test-shared-basic`. Those are behavior-parity checks;
they do not establish edition conformance.

## Diagnostic results

| Measure | Count |
|---|---:|
| Control scenarios | 10, all expected outcomes matched |
| Executed upstream scenarios | 93,141 |
| Unique executed source files | 48,598 |
| Passed | 61,565 |
| Failed | 31,576 |
| Reported skipped scenarios | 902 |

Skips: 70 optional HTMLDDA-host scenarios, 4 `CanBlockIsFalse` scenarios for this
blocking host, and 828 duplicate strict module scenarios from the upstream stream.
The original module source runs once. Unsupported language features are not
filtered out.

Full scenario results and raw outputs are in ignored
`out/test262-zig/results.json`; configuration, skips and per-feature counts are in
`out/test262-zig/summary.json`. Reproduce through
[tools/test262](../tools/test262/README.md), preserving the revision and flags.

## What the data changes in the plan

The baseline confirms inherited edition debt as well as modern feature gaps.
Some illustrative feature-tag counts:

| Tag | Passed | Failed |
|---|---:|---:|
| `Object.fromEntries` | 50 | 0 |
| `Object.hasOwn` | 124 | 0 |
| `error-cause` | 10 | 0 |
| `regexp-lookbehind` | 0 | 38 |
| `regexp-named-groups` | 118 | 82 |
| `regexp-unicode-property-escapes` | 302 | 1,060 |
| `for-in-order` | 8 | 10 |
| `Array.prototype.flat` | 28 | 2 |
| `Array.prototype.flatMap` | 39 | 2 |
| `BigInt` (default off) | 122 | 2,809 |
| `Temporal` (post-ES2025 scope) | 0 | 9,226 |

Feature counts overlap and include negative parser tests. A feature can have
passing negative cases even when its valid syntax/builtin is missing; these
numbers are not implementation-presence claims. Runtime feature probes separately
reject named groups, lookbehind, property escapes, `d`/`v` and import attributes.
The `for-in` formalization should be triaged, not assumed conformant. Existing
builtins can also have semantic failures despite passing their legacy suites.

## Limits before an edition gate

- The current corpus includes newer proposals and draft semantic changes; select
  and validate an ES2025 policy before turning its percentage into an edition score.
- The upstream validator checks negative error **type**, not every parse /
  resolution / runtime phase distinction. That must be closed for a strict exit gate.
- Host applicability and host-adapter behavior are recorded separately. The
  controls exercise assertions, async completion, modules, realms, detachment,
  expected errors, wrong errors and nonzero empty-output exits; they do not prove
  every host mechanism is complete.
- A new runtime/builtin implementation starts with focused test262 triage and a
  comparable baseline; keep the legacy behavior-parity gate green alongside it.

## C1 addendum (2026-10-13)

The edition policy, edition-selected baseline and root-cause triage for this
recorded data are in [c1-triage.md](c1-triage.md), computed by
`tools/test262/analyze.py` against `tools/test262/edition-policy.json`
without re-running the corpus: **60,896 passed / 19,648 failed across
80,544 edition-selected scenarios**, versus the broad 61,565 / 31,576
above. The committed analysis is `tools/test262/c1-baseline-analysis.json`.
Negative-phase qualification there is **CONDITIONAL** (controls now detect
the demonstrated wrong-phase masquerades; explicit host phase reporting is
still required before certifying a full result), so the limits above stand
for any edition *claim*.
