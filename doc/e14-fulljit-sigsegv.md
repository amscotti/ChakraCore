# E14 — fulljit SIGSEGV family: root cause, fix, verification

Status: **implemented on mainline `modernize/baseline`** (base `d0b2dc019`).
Scope: the e12 card at doc/open-cards.md:110/111 — the deterministic
fulljit SIGSEGV family (115 SIGSEGV + 3 SIGBUS in the e12 `-j10` dynapogo
run; 109 non-WASM incl. es7/matchall, bigint64array, resizable-typedarray,
array_findlast, SpecialSymbolCapture) plus the load-only
blockscope-functionbinding crasher.

## 1. Reproduction and minimization

The e12 standalone repro reproduced at HEAD (rc=139, deterministic). A
fresh lldb backtrace (`lldb -b`, direct `run`; a stop-at-entry launch
`process launch -s true` happens to exit 0 — the initial stop perturbs
the layout — which is one reason the defect is flaky under debuggers):

```
frame #0: Js::RecyclableObject::GetType(this=0x0)          RecyclableObject.h:276
frame #2: Js::JavascriptOperators::GetTypeId(0x0)
frame #4: Js::JavascriptOperators::PrototypeObject(prototypeProperty=0x0, ...)
          JavascriptOperators.cpp:7023
frame #5: Js::JavascriptOperators::GetPrototypeObjectForConstructorCache(...)
          JavascriptOperators.cpp:6999  — GetProperty(prototype) returned TRUE
frame #6: Js::JavascriptOperators::NewScObjectCommon(...)   JavascriptOperators.cpp:7404
frame #7+: JIT frames (no symbols)
EXC_BAD_ACCESS (code=1, address=0x8)  — reading `type` at offset 8 of null
```

The property lookup **finds** `prototype` on the constructor but the slot
value is the null Var `0x0`. Minimal witness (4 lines, deterministic):

```js
class Sub {}                 // or: var Sub = class {}; class Sub extends Object {} ...
var s = new Sub();           // any class; plain `function F(){}` does NOT crash
print("ok");
```

Flags: `-forceNative -off:simpleJit -bgJitDelay:0` (the dynapogo variant
flags; profile-input state irrelevant — no `-dynamicprofileinput` needed).
Every class-creating script crashes; every plain-function script passes.
`-nonative` passes everything.

## 2. Root cause (bisected by reading, then proven at the object level)

Guilty commit: **`9b9ef5d50` — b5 class-tail 2/3 (e6 §2.5+6, constructor
shape reorder)** — exactly the PRIME SUSPECT in the e12 card. No bisect
builds were needed: the shape sites were read directly and the defect is
visible in the one-line diff hunk.

The B5 reorder moved the class constructor's `prototype` from slot 0 to
slot 2 (handler order `length, name, prototype`). The runtime sites
(`OP_NewClassConstructor` slots 0/1, `InitClassHelper` slot 2 via
`SetSlot`) are correct. The JIT site is not: in
`Lowerer::LowerNewClassConstructor` (lib/Backend/Lower.cpp), the
prototype store was changed from aux-slot byte-offset 0 to

```cpp
InsertMove(IR::IndirOpnd::New(opndAuxSlots, 2, TyMachPtr, m_func, true), opndProto, ...);
```

but `IR::IndirOpnd::New(RegOpnd*, int32 offset, ...)` takes a **byte**
offset (every other call site passes `offsetof(...)`-style values; the
indexed form uses a register + scale, e.g. Lower.cpp:5938). Slot 2 lives
at byte offset `2 * sizeof(Var)` = 16. The store therefore:

- wrote the 8-byte proto pointer at aux-array byte 2 — misaligned,
  clobbering the high 6 bytes of slot 0 (`length`) and the low 2 bytes of
  slot 1 (`name`) with pointer bytes: both become garbage Vars;
- left slot 2 (`prototype`) at its recycler-zero initial value **0x0**.

Proven at the object level under lldb: `constructor` is a
`FunctionWithHomeObj<ScriptFunction>` whose 3-slot
`SimpleTypeHandler<3>` (`SharedFunctionWithNonWritablePrototypeLengthAndNameTypeHandler`,
inlineSlotCapacity 0 → slots are aux slots) reads `prototype` from
aux[2] = 0x0 → `PrototypeObject(0x0)` → `GetTypeId(0x0)` → `GetType()`
reads `type` at null+8 = address 0x8 — the exact EXC_BAD_ACCESS address.
Value-level confirmation without a debugger: under fulljit,
`Object.getOwnPropertyNames(Sub)` prints `length,name,prototype` (key
order correct — descriptors, not values) but reading `Sub.length,
Sub.name` SIGSEGVs on the corrupted slots; the interpreter prints
`0 Sub true`.

The bad object provenance (memory-management.md five questions): the
invalid Var is not a dangling pointer or an ownership failure — it is a
never-written, zero-initialized recycler aux slot published through a
type handler whose descriptor for `prototype` points at it. No lifetime,
barrier, or reentrancy change is involved; the JIT store simply targeted
the wrong byte offset in an array the runtime already owns.

Why the whole family: any JIT-compiled `class` creation (the opcode is
inlined by `LowerNewClassConstructor` whenever the enclosing function is
fulljit-compiled) yields a constructor whose `prototype` reads null; the
first subsequent `new C()` walks
`NewScObjectCommon → GetPrototypeObjectForConstructorCache → GetTypeId(0x0)`
and dies. matchall.js & co. merely create `class ... extends RegExp` (or
any class) and `new` it. `-nonative`/interpreted runs use
`InitClassHelper`'s correct `SetSlot(2)` and pass. The load-only
blockscope-functionbinding crasher (open-cards.md:111) is the same
defect reached only under parallel load's JIT tiering.

## 3. Fix

lib/Backend/Lower.cpp `Lowerer::LowerNewClassConstructor` — scale the
slot index like the other constant aux-slot stores
(`index * sizeof(Js::Var)`, cf. Lower.cpp:6270/7494/7508):

```cpp
InsertMove(IR::IndirOpnd::New(opndAuxSlots, 2 * sizeof(Js::Var), TyMachPtr, m_func, true), opndProto, ...);
```

One line + comment; no runtime, bytecode, or shape change. The
`ScrFunc_OP_NewClassConstructor` helper call and the proto-side aux[0]
store were already correct.

Ownership record: no new allocation, root, barrier, transfer point, or
thread crossing; the store's target array and its recycler ownership are
unchanged, only the byte displacement is corrected.

## 4. Why the gates missed it (and the smoke that closes the gap)

- **No-JIT gates (2051/2103)**: interpreter/simple-jit paths never lower
  `NewClassConstructor`; `InitClassHelper`'s `SetSlot(2)` was always
  correct. Semantics-only coverage cannot see lowering bugs.
- **`check_jit` (the era's 646-step jit gate)**: its physical smoke
  exercises a hot call, rest args and `apply` — **no class creation and
  no `new`**. The e6 lane's manual JIT spot-probes asserted the
  constructor own-key **order** (`length,name,prototype`) — descriptor
  names, not slot values — so the corrupted `length`/`name` and nulled
  `prototype` were invisible to them.
- **Gap closure**: `tools/build/check_jit.py`'s smoke script now creates
  a class, reads `P.length, P.name, P.prototype === P.prototype`,
  constructs `new P()` and a derived anonymous class, and pins
  `getOwnPropertyNames` — all under both `-mic:1 -bgjit-` and
  `-forceNative -off:simpleJit -bgJitDelay:0`. Negative control
  (executed twice this lane): with the fix stashed and the jit profile
  rebuilt, the smoke fails exactly as designed —
  `check_jit: FAIL: -forceNative -off:simpleJit -bgJitDelay:0 exited
  -11`, the crash landing at `jit-class-vals` (reading `P.length`, the
  corrupted slot) while the `-mic` leg still passes — and after
  restoring the fix and rebuilding it is green again. (The second,
  accidental control: a full `-j10` dynapogo suite that overlapped a
  broken-binary window reproduced the e12 before-shape, 115×`-11` +
  6×`-10`, against 0 signals on the fixed binary — §5.)

## 5. Verification

| Check | Before (e12 / HEAD d0b2dc019) | After |
|---|---|---|
| Standalone repro matchall.js, dynapogo flags | rc=139 deterministic | **20/20 clean** (summary `pass`) |
| matchall.js default tier (no forceNative) | crash family context | 20/20 clean |
| Minimal witnesses (empty class + one `new`; named/derived/`extends RegExp` classes with warmup loops; plain `function F(){}` control) | every class shape rc=139, plain fn rc=0 | all rc=0; plain fn unchanged |
| bigint64array / resizable-typedarray / array_findlast / SpecialSymbolCapture dynapogo flags | SIGSEGV | all rc=0, `pass` |
| Full dynapogo suite `-j4` (`runtests.py --test --variants dynapogo`) | 1907/398 at `-j10` (e12): 115×SIGSEGV + 3×SIGBUS | **2019/286, 0×`-11`, 0×`-10`** — failures confined to the tagged asm.js/WASM dirs (AsmJs 92, WasmSpec 123, wasm 54, AsmJSFloat 14, WasmSpec.MultiValue 2, ASMJSParser 1) |
| Full dynapogo suite `-j10` (load-only crasher leg) | es6/blockscope-functionbinding.js SIGSEGV at `-j10` only | **2019/286, 0×`-11`, 0×`-10`**; es6 223/0, es7 108/0 |
| interpreted variant (`--variants interpreted`) | — | 2441/401 with 3 signal exits — the pre-existing `Bugs/bug_OS21193960.js` + `Array/Array_TypeConfusion_bugs.js` pair (both variants), unrelated by construction: the fixed store lives in JIT-generated native code and never executes without native tiers |
| no-JIT gates `zig build check` / debug-static `check-regression` | 2051 / 2103 | **2051 / 2103** (`out/e14-gates.log`, rc=0 chain) |
| `zig build -Dprofile=jit check` | PASS (646-step gate, pre-gap) | **PASS** incl. the extended check_jit smoke + bytecode verify (`out/e14-gates.log`) |
| Negative control (fix stashed, jit profile rebuilt) | — | `check_jit: FAIL: -forceNative -off:simpleJit -bgJitDelay:0 exited -11` — crash at `jit-class-vals` (reading the corrupted length slot) while the `-mic` leg passes; restored + rebuilt → PASS |

Both recorded after-suites report identical totals at `-j4` and `-j10`.
Versus the e12-era receipt base (2006/286 at `-j4` on its older base),
passed grew 2006→2019 because the e5–e13 lanes added focused rlexe
entries; the invariant that matters — failed = the same 286 tagged-only
population, zero signal exits — is restored exactly (286 tagged + 118
signal exits ≈ e12's 398 before-failures).

**Verification-hygiene incident (for log archaeology):** two sessions
ran this lane's verification concurrently. One full `-j10` "after" run
overlapped a broken-binary negative-control window and reproduced the
before-shape (115×`-11` + 6×`-10`); another was doubly interleaved
into one log with unattributable totals. Both logs were discarded. The
recorded `-j10` after-run executed against an immutable private copy of
the fixed binaries (ch + libChakraCore.dylib; dylib md5
`f8fe4f76715281910af6d572e21cef29`, identical to the installed
`out/jit/bin/libChakraCore.dylib`), and the `-j4` after-run's whole
window (18:37–18:58) precedes the first broken-binary install
(19:03:29) — both logs are fixed-binary evidence. The interpreted
after-log (19:01) is likewise inside the fixed window.

## 6. Logs

- `out/e12-dynapogo-j10-before.log` — baseline: 1907/398, 115×SIGSEGV +
  3×SIGBUS.
- `out/e14-dynapogo-j4-after.log` — full dynapogo `-j4`, fixed binary:
  2019/286, zero signal exits.
- `out/e14-dynapogo-j10-after.log` — full dynapogo `-j10` against the
  immutable fixed-binary copy: 2019/286, zero signal exits.
- `out/e14-interpreted-after.log` — interpreted variant, fixed binary:
  2441/401, 3 pre-existing signal exits (bug_OS21193960,
  Array_TypeConfusion_bugs ×2).
- `out/e14-gates.log` — test-profile `zig build check` (2051/0),
  debug-static `check-regression` (2103/0), `-Dprofile=jit check`
  (check_jit PASS with the extended smoke, bytecode headers unchanged).

## 7. Residuals

- The e12 card's 3 SIGBUS and 6 WASM-tagged signal exits belong to the
  same 118-crash population; both recorded after-suite runs show zero
  signal exits of either kind, so they were all the same defect under
  different memory layouts (a corrupted `length`/`name` Var read can
  fault at addresses other than 0x8 depending on the pointer bytes).
- dynapogo remains load-sensitive outside this defect: the discarded
  contended runs showed a handful of extra non-signal failures whose
  names are all inside the recorded runs' passing sets. Not a defect of
  this fix; worth remembering when reading ad-hoc `-j10` runs on a
  busy machine.
- The interpreted variant's `bug_OS21193960` /
  `Array_TypeConfusion_bugs` signal exits are pre-existing (present
  with and without the fix) and stay on the books as separate items.
- No other unscaled constant-slot `IndirOpnd` store exists in the
  lowerer (audited: only LowerNewClassConstructor's two sites; slot 0's
  byte offset 0 is correct by identity).
- Linux JIT gates can now treat dynapogo as authoritative for this
  family; the standing Mac-only evidence cards (open-cards.md:103/104)
  still require Linux runs.

### Review outcome (dual ACCEPT, folds landed)

Both reviewers ACCEPTed with zero `[bug]`. The general review verified
the offset semantics and ran an independent 65-site audit (no other
unscaled stores; the concat-idiom identified as the unit-confusion
source); the smoke was proven capable of catching B5 at landing. The
adversarial review ran a 348-site parser audit (3 hits, all benign),
set-delta forensics proving the blast radius EXACTLY the non-WASM
signal-exit set with zero baseline contamination, runtime ground-truth
verification of every B5 shape site (slot-index APIs interpreter-side,
byte-offset JIT-side, now consistent), A3 marshalling arg-order
re-derivation, an independent dynapogo slice of the affected dirs
(617/0), and live check_jit verification under both tiers.

**Folds:** the stale j10 figures (1994/311) were corrected to the
authoritative preserved 2019/286 by the second implementer session
(`69281b6e4`) — both preserved j10 runs are set-identical to j4 (zero
signals); the adversarial R1 is thereby discharged. R2 noted: the
interpreted before-control log (broken-binary, 20 crashes) is
unpreserved — the 20→0 delta rests on the receipt plus the 3 residual
crashers verified identical; accepted. Nits folded: "length alone
SIGSEGVs" phrasing corrected (length reads as a huge TaggedInt; the
crash comes from the name read and `new`); `out/b3-base-bin` is a
pre-BUG (Nov 3) reference, not pre-fix. The masked matchall #15 flake
is proven independent (zero path intersection; 40/40 clean) and stays
carded for the regex lanes.
