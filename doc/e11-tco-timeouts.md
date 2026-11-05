# E11 — TCO tail + deterministic timeouts (engine-defect lane)

Status: **implemented on mainline `modernize/baseline`** (base `f25e8cb9c`).
Scope: the two C4 populations named by the lane charter — the
`tail-call-optimization` 34 edition-selected failures (manifest §5c) and the
6 deterministic timeouts attributed to the c4 host bucket (4×
`Array/prototype/{splice,unshift}/throws-if-integer-limit-exceeded` + 2×
`Atomics/wait/good-views`).

## 1. Population A — the TCO 34: all feature-missing, zero bounded defects

**Fresh selection** (all 35 corpus files tagged `tail-call-optimization`,
pinned revision `2e0a5676`): **1 passed / 34 failed** — identical to the
manifest. Every failure is `Expected no error, got Error: Out of stack space`.

| Split | Count | Verdict |
|---|---:|---|
| Proper-tail-call semantics tests (deep tail recursion at `$MAX_ITERATIONS = 100000`) | **34** | **Feature-missing — CARD E11-1** (below) |
| Bounded defects (evaluation-order or adjacent shapes) | **0** | none exist in this population |
| Passes today | 1 | `Proxy/revocable/tco-fn-realm.js` — tests only the realm of a revoked-proxy TypeError; no deep recursion, passes by shape |

Evidence the 34 need frame re-use rather than a tweak:

- Each test recurses 100,000 times in tail position through every
  HasCallInTailPosition shape (`return f(n-1)` in blocks/if/switch/try/
  loops/`?:`/`&&`/`||`/`??`/comma/tagged-template/final expression, plus the
  four `eval`-shadowed callee variants).
- ch's measured max tail-recursion depth is **6,901** frames (binary-search
  probe, `-nonative -Test262`), so 100,000 cannot pass by headroom.
- **Oracle agreement:** Node v26.5.0 (V8) throws
  `RangeError: Maximum call stack size exceeded` on the canonical shape —
  V8 also does not implement proper tail calls. The prior lanes'
  "8 TCO stack-overflow" attributions (S1/B3) and any
  `f,ReferenceError`-vs-`g,ReferenceError` shapes are **not** in this
  population: those are the already-carded Annex B.3.9 sloppy
  call-target evaluation-order residual (CARD R2, e7 receipt §"Repair-iteration
  cards"); this corpus's annexB tree contains no tail-call-tagged test.

Proper tail calls (ES2015 27.2.4 `PrepareForTailCall` / static semantics
`HasCallInTailPosition`) were never implemented in ChakraCore — an
interpreter-loop re-entrancy + JIT-frame-teardown project (C3-class), the
same product decision V8 made. **Not a bounded lane.**

## 2. Population B — the 6 deterministic timeouts

### B1. Array integer-limit (4 timeouts + 2 same-family fast fails)

The three corpus files set `arrayLike.length = 2**53-1` (and saturating
spellings) and call the generic path. Spec (ES2025, read262):
`push` step 5 (`len + argCount > 2^53-1`), `splice` step 8
(`len + insertCount - actualDeleteCount > 2^53-1`), `unshift` step 4.a
(`len + argCount > 2^53-1`) each require a **TypeError before any element
is moved or stored**. Node v26.5.0 throws `TypeError` in all three.

| Test | Scenarios | ch (before) | Node | Root cause |
|---|---:|---|---|---|
| `splice/throws-if-integer-limit-exceeded` | 2 | **timeout** (SIGKILL) | TypeError | `EntrySplice` dispatched `ObjectSpliceHelper<uint64>` with `length = 2^53-1`, which walks `len` indices — O(2^53) |
| `unshift/throws-if-integer-limit-exceeded` | 2 | **timeout** (SIGKILL) | TypeError | `UnshiftObjectHelper` shifted `[end, len)` — O(2^53) |
| `push/throws-if-integer-limit-exceeded` | 2 | fast fail (no throw) | TypeError | `EntryPushNonJavascriptArray` stored at `2^53-1` and wrote `length = 2^53` — no limit check |

**Fix** (one comparison per site, `lib/Runtime/Library/JavascriptArray.cpp`):

1. `EntrySplice` — after `start`/`deleteLen` coercion and `insertLen`
   capture, before the array fast path and `ObjectSpliceHelper` dispatch:
   `(length - deleteLen) + insertLen > FiftyThirdPowerOfTwoMinusOne` →
   `ThrowTypeError(JSERR_IllegalArraySizeAndLength)` (the existing 5111 code
   already used by `concat`'s and `toSpliced`'s identical checks). `deleteLen`
   is already clamped to `len`, so no underflow; real-array lengths (uint32)
   cannot reach the limit.
2. `UnshiftObjectHelper` — inside `unshiftElements > 0`, immediately after
   `OP_GetLength`, before the shift walks:
   `length > BigIndex(FiftyThirdPowerOfTwoMinusOne - unshiftElements)` →
   same throw. (`len` is ToLength-saturated ≤ 2^53-1; no underflow.)
3. `EntryPushNonJavascriptArray` — after `ToLength`, before the first
   `SetItem`: `(len) + (argCount - 1) > FiftyThirdPowerOfTwoMinusOne` →
   same throw.

No allocation/ownership/GC surface (per doc/memory-management.md §3): the
checks are pure uint64/BigIndex comparisons on locals already read; the
throw path is the standard recycler-owned error. No retained pointers.

**Node-parity probes** (all identical ch ↔ Node, incl. observable order):
`length` getter → `start` coercion → `deleteCount` coercion → throw;
`len = 2^53-2` + 1 push succeeds and caps `length` at 2^53-1; `len = 2^53-3`
+ 3 push throws, + 2 succeeds; argCount-0 `push`/`unshift` never throw;
small generic/real-array splice/unshift/push behavior unchanged.

### B2. Atomics/wait/good-views (2): reclassified host→disabled-flag, no engine change

Diagnosis chain (all reproduced):

1. Flag-off default: the test's `SharedArrayBuffer` use lives **inside the
   `$262.agent.start(...)` source** (`bin/ch/WScriptJsrt.cpp`
   `'crossthread'` path). The agent thread's `JsRun` throws
   `ReferenceError: SharedArrayBuffer is not defined` and the error is
   **deliberately not surfaced** (the spawn-wait comment: "JsRun errors are
   not surfaced here"); the thread parks in its broadcast/shutdown wait —
   it never reports.
2. The main thread then calls `$262.agent.getReport()`, which the pinned
   corpus's `harness/atomicsHelper.js` (lines 29–36) **paves over with a
   blocking poll** (`while ((r = getReport()) == null) sleep(1)`) — an
   upstream-by-design contract that assumes agents always eventually
   report. Over a silently-dead agent it never returns → the 10 s harness
   timeout → SIGKILL.
3. **The waitAsync analogs fail fast and cleanly**
   (`Atomics/waitAsync/good-views.js` ± bigint: `ReferenceError: 'Atomics'
   is not defined` on the main thread) — the expected flag-off shape. Only
   the agent-side-SAB shape hangs.
4. **Flag-on ceiling: `-ESSharedArrayBuffer` → 2/2 PASS.**

Verdict: these 2 are disabled-flag failures (the S6 product decision, same
owner as the other 744 Atomics scenarios) that *manifest* as timeouts
through the corpus helper's blocking getReport. An engine "fix" would have
to make a dead agent's `GetReport` return a synthetic value (breaking the
null-until-report contract every other agent test relies on) or surface
agent-thread errors onto stderr (which eshost's error parser treats as host
errors, risking the 44 flag-on agent scenarios). **Reclassify, don't fix**:
host bucket 426 → 421 (−4 Array fixed, −2 moved), disabled-flag bucket
1,734 → 1,736.

## 3. Before / after (this HEAD, default flags, pinned corpus)

| Selection | Before | After | Δ |
|---|---|---|---|
| TCO 35-file selection | 1/34 | 1/34 | 0 (card E11-1) |
| `{splice,unshift,push}/throws-if-integer-limit-exceeded` (6 scenarios) | 0/6 (4 timeout + 2 fast-fail) | **6/6** | +6 |
| Focused dirs `Array/prototype/{splice,unshift,push}/**` (254 scenarios) | 240/14 | **248/6** | **+8 fail→pass, 0 pass→fail** (includes `push/S15.4.4.7_A2_T2.js` ×2) |
| `Atomics/wait/good-views` | 0/2 (timeout) | 0/2 (timeout) | 0 (reclassified; flag-on 2/2) |

Gates: `zig build check` **rc=0** (regression **2050/0** pinned
2049→2050, bytecode up-to-date + pure-verify rc 0, native PASS, smoke
PASS, metadata PASS); `-Dprofile=debug-static check-regression` **rc=0,
2102/0** (2101→2102); `-Dprofile=jit check` **rc=0** (shared file
recompiled clean). Focused rlexe (`--variants=disable_jit`):
`test/Array` + `test/es5` **176/0**, `test/es7` **107/0** (new entry),
S6 files (`s6-growable-nonshared`, `s6-waitasync-pause`,
`s6-bigint-atomics`, `s6-broadcast-type`, `s6-waitasync-cross-agent`)
green. `git diff --check` clean.

## 4. Residual cards

| Card | Scenarios | Content |
|---|---:|---|
| **E11-1 proper tail calls** | 34 (+1 shape-pass) | ES2015 27.2.4 `PrepareForTailCall`/`HasCallInTailPosition` never implemented (interpreter re-entrancy + JIT frame teardown; C3-class). Same product posture as V8/Node (oracle also stack-overflows). Feature-decision owner; not bounded-fixable by stack headroom (depth 6,901 ≪ 100,000). |
| **E11-2 unshift fast-path bypasses prototype accessors** | 6 (3 files × 2: `set-length-array-is-frozen`, `set-length-array-length-is-non-writable`, `read-only-property`) | Pre-existing (fails identically before this lane). `array.unshift(1)` on `[]` with an `Array.prototype[0]` setter: spec/Node invoke the setter, suppress the own store, then throw on the sabotaged `length` Set; ch's `useNoSideEffectUnshift` fast path stores directly (`own0 true`, setter never called) because `HasAnyES5ArrayInPrototypeChain(arr, /*force*/true)` only detects ES5Arrays in the chain, not indexed accessors on plain-object prototypes. Ordinary stores, `push` and `splice` all consult the setter correctly — the asymmetry is contained in `UnshiftHelper`/`DirectSetItemAt`. Fix = extend the unshift guard (or route the growing item store through proto-respecting SetItem); belongs with the next array-[[Set]] slice. |
| **E11-3 (reclassification)** | 2 | §2.B2 above: `Atomics/wait/good-views` moves host→disabled-flag (S6 owner); mechanism recorded so the next full manifest refresh books it correctly. No engine change. |

**Expected C4 delta** (re-measurement lane): edition-selected failures
4,080 → **4,072** (−8 = the 6 limit scenarios + `push/S15.4.4.7_A2_T2` ×2,
which expects the same throw for `length: Infinity`; the file is untagged,
hence edition-selected per the policy rule — verified from the before-run
attrs). 0 regressions; host bucket 426 → 421 (−4 fixed, −2 Atomics
reclassified per §2.B2), disabled-flag bucket 1,734 → 1,736.

## 5. Commands

```
# triage / before-after (both populations, default flags)
tools/test262/run.py --binary $PWD/zig-out/bin/ch --checkout $PWD/out/test262 \
  --output out/test262-e11-tco-before <35 tail-call-optimization files>   # 1/34
#   ... -after                                                            # 1/34
#   ... out/test262-e11-arr{,-after}      <3 limit files>                 # 0/6 -> 6/6
#   ... out/test262-e11-array-focus{,-after} '…/splice/**' '…/unshift/**' '…/push/**'
#                                                                          # 240/14 -> 248/6
#   ... --host-flags '… -ESSharedArrayBuffer' out/test262-e11-atomics-flag-on good-views  # 2/2
# gates + focused
test/runtests.py --test --variants=disable_jit --binary …/ch test/Array test/es5   # 176/0
test/runtests.py --test --variants=disable_jit --binary …/ch test/es7              # 107/0
zig build check;  zig build -Dprofile=debug-static check-regression;  zig build -Dprofile=jit check
```
