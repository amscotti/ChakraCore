# Swarm review program — final report (A1–A12 + JITREG)

12-area adversarial review with isolated fix lanes, parent validation, and
merge. Branch `modernize/baseline`: gate green **2059/0**, tree clean.

## Coverage

| Area | Verdict | Commit |
|---|---|---|
| A1 parser escapes (cooked-undefined, strict 8/9) | fixed | `627b77325` |
| A2 serialization (homeobject bit + GUID remint) | fixed | `868b514c1` |
| A3, A4, A5 | clean, no findings | — |
| A6 SmInt pair test (x64 mirror) | fixed | `c6798f022` |
| A7 link validation (CheckFFI + SIMD memarg) | fixed | `a4a4a3f48` |
| A8 Node-API lifetimes | fixed | `cad0222d2` |
| A9 Intl BigInt exact formatting | fixed | `e944f1f66` |
| A10 | clean (sibling folded into S6) | `446e51584` |
| A11 RegExp lastIndex clamp + U9 folds | fixed | `ad93b4efb` |
| A12 module TDZ (finished by parent after lane stall) | fixed | `7d85b7ccd` |
| JITREG-MA matchall #15 StFld guard | fixed | `0eb734a72` |
| JITREG-BI BigInt64 store placeholder | fixed | `1004b326d` |
| JITREG-S6 SAB grow view staleness | fixed | `446e51584` |

## Verification totals (observed, merged tip)

- `zig build check`: PASS 2059/0 (pin 2059/2111); bytecode up to date.
- es6 224/0, Basics 48/48, es7 111/0 (interp + dynapogo — PGO trio fully resolved), es6module 32/0, Intl 17/0, UnifiedRegex 23/0.
- test262 `RegExp/**`: 3036/720 with the 4 `ToLength` flips passing, 0 regressions (base-vs-fixed pass-set diff).
- Node-oracle differentials for every behavioral fix, all tiers.

## Deferred / open (with owner)

- A7-3: needs wasm binary — deferred.
- Exec-failure `lastIndex` reset bypasses non-writable (both tiers, default config): open, regex owner (legacy-behavior disposition; Node throws).
- Case-folding regen process + `Changes_When_Casefolded`/full-fold gaps: open, separate lane.
- Intl residuals: `percent` vs `percentSign` naming, es-ES 4-digit grouping vintage, negative-half sig rounding vs double path: kept engine-consistent, documented.
- Pre-existing environment: P1-jit EH stack-exhaustion x4, libtool `-static` flake (retry succeeds), GCStress full matrices unrun, 241G cache pressure.
- WASM ARM64 full-surface plan: separate track, unchanged.

## Process notes

- JIT profile ships engine code in `libChakraCore.dylib`; `ch` timestamp staleness after rebuilds is expected — validate via behavior + dylib mtime, not the launcher.
- Dynapogo-only runs are vacuous for PGO (no profile without an interpreted leg); canonical two-step recipe is the gate.
- One lane stalled with no delivery (A12, 5+ h idle, no lock/build activity); its small scoped diff was taken over, completed (var-init gap + stale test expectation, both oracle-backed), and committed. Takeover rule used: two consecutive idle observations + scoped, understood work.
