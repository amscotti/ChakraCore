# E14 — fulljit SIGSEGV family: root cause, fix, verification

Status: **implemented on mainline `modernize/baseline`** (base `d0b2dc019`).
Scope: the e12 card at doc/open-cards.md:110/111 — the deterministic
fulljit SIGSEGV family (115 SIGSEGV + 3 SIGBUS in the e12 `-j10` dynapogo
run; 109 non-WASM incl. es7/matchall, bigint64array, resizable-typedarray,
array_findlast, SpecialSymbolCapture) plus the load-only
blockscope-functionbinding crasher.

## 1. Reproduction and minimization

The e12 standalone repro reproduced at HEAD (rc=139, deterministic). A
fresh lldb backtrace with ASLR enabled (`target.disable-aslr false` — with
ASLR disabled the garbage read lands on mapped memory and the process
exits 0, one reason the defect is layout-flaky under debuggers):

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
  `-forceNative -off:simpleJit -bgJitDelay:0`. Negative control: with
  the fix reverted this smoke fails with rc=139 on the `-forceNative`
  leg (verified by rebuilding the broken Lower.cpp), so the gate now
  fails on any reintroduction.

## 5. Verification

| Check | Before (e12 / HEAD d0b2dc019) | After |
|---|---|---|
| Standalone repro matchall.js, dynapogo flags | rc=139 deterministic | **20/20 clean** |
| matchall.js default tier / `-mic:1 -bgjit-` | crash family context | 5/5 and 10/10 clean |
| Witness w1–w7 (class decl/expr/derived/extends, plain fn) | all class shapes rc=139 | all rc=0; 20/20 on w4 |
| bigint64array / resizable-typedarray / array_findlast dynapogo flags | SIGSEGV | 3/3 each clean |
| Full dynapogo suite `-j4` | 1907/398, 115×SIGSEGV + 3×SIGBUS (e12 `-j10`) | **2019/286, zero signal exits** — failures exactly at the 286 receipt base; +13 passes vs the 2006 base are later lanes' fixes (sweep/e11/e13) |
| Full dynapogo suite `-j10` rerun (load-only crasher) | 1907/398, blockscope-functionbinding SIGSEGV | **1994/311, zero signal exits**; blockscope-functionbinding clean; the +25 vs j4 are load-order flakes (matchall/bigint64array/s6-growable/wasm timeouts pass 5/5 standalone with their exact j10 flags — masked non-crash assertions now observable where the crash used to abort first) |
| interpreted variant (broken binary rebuilt for the control) | 2424/418 with **20 signal crashes** — the same family: classes.js, ES6SubclassableBuiltins, class-private-{fields,methods,jit-probes}, toStringTag, regex-prototype-properties, bug_OS16649604, typedarray_bugfixes | **2441/401, 3 signal crashes** = the pre-existing bug_OS21193960 + Array_TypeConfusion_bugs set (identical on broken and fixed) |
| no-JIT gates `check-regression` test / debug-static | 2051 / 2103 | **2051 / 2103** (out/e14-gates.log, rc=0 chain) |
| `zig build -Dprofile=jit check` | PASS (646 steps, pre-gap) | **PASS** incl. extended check_jit smoke + bytecode verify |
| Negative control (fix reverted, rebuilt) | — | `check_jit: FAIL: -forceNative -off:simpleJit -bgJitDelay:0 exited -11` immediately after the key-order line — the smoke catches the defect class |

## 6. Logs

- `out/e14-dynapogo-j4-after.log` — full dynapogo `-j4`: 2019/286, 0 signal.
- `out/e14-dynapogo-j10-after.log` — `-j10` rerun: 1994/311, 0 signal.
- `out/e14-interpreted-after.log` — interpreted variant, fixed: 2441/401, 3 pre-existing crashes (broken-binary control in /tmp during the session: 2424/418, 20 crashes).
- `out/e14-gates.log` — no-JIT gates (2051/0, 2103/0) + jit check PASS.

## 7. Residuals

- The e12 card's 3 SIGBUS and 6 WASM-tagged signal exits belong to the
  same 118-crash population; both after-suite runs show zero exits of
  either kind, so they were all the same defect under different memory
  layouts (a corrupted `length`/`name` Var read can fault at addresses
  other than 0x8 depending on the pointer bytes).
- Masked, non-crash assertions are now observable where the crash used
  to abort the script first (e.g. matchall test #15 lastIndex
  [[Set]]-on-failed-match-reset under one specific profile input at
  `-j10`); they pass standalone 5/5 with identical flags and are
  load-order flakes, but the lastIndex shape is worth a look whenever
  the regex-era lanes resume — not this lane's defect (it exists only
  as a nondeterministic tiering flake and predates the fix by being
  unreachable).
- No other unscaled constant-slot `IndirOpnd` store exists in the
  lowerer (audited: only LowerNewClassConstructor's two sites; slot 0's
  byte offset 0 is correct by identity).
- Linux JIT gates can now treat dynapogo as authoritative for this
  family; the standing Mac-only evidence cards (open-cards.md:103/104)
  still require Linux runs.
