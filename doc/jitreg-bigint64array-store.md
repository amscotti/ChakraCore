# JIT-REG card: bigint64array.js — in-bounds Object store on BigInt64Array throws TypeMismatch under FullJIT+PGO

Status: NOT a post-e14 regression. Reproduces identically at e14-green tree
69281b6e4 and pre-b5 tree 6c1470f9d (line 173). Narrowed to a concrete
shared-backend site (below, hypothesis with strong probe support — not yet
debugger-confirmed). Carded for follow-up: the correct fix reroutes BigInt
stores to the helper in delicate label/branch code shared with x64 — JIT
surgery, deliberately not done here.

## Failing test (1 subtest)

`test/es7/bigint64array.js`, compile flags `-ESBigInt -CollectGarbage`, test
"Out-of-bounds and detached stores convert before validation": line 173,
`view[10] = { valueOf() { calls++; return 3n; } }` on a length-1 view —
expected: converts exactly once (`calls===1`), no throw (OOB no-op). Actual:
`TypeError: Type mismatch` (VBSERR_TypeMismatch, `ToBigInt` family). 9/10 pass.

## Repro (deterministic, PGO path only)

```
CH=out/jit/bin/ch
$CH -ESBigInt -CollectGarbage -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- \
  -dynamicprofilecache:/tmp/jitreg/p_bi bigint64array.js -args summary -endargs  # pass
$CH -ESBigInt -CollectGarbage -forceNative -off:simpleJit -bgJitDelay:0 \
  -dynamicprofileinput:/tmp/jitreg/p_bi bigint64array.js -args summary -endargs  # 9/10 fail
```

Trigger-narrowing probes (all under the PGO recipe unless noted):

- In-bounds object store `view[0] = {valueOf...3n}` on length-2 view: THROWS
  `Type mismatch` WITHOUT calling `valueOf` (conversion skipped entirely).
- OOB object store: converts fine (`calls===1`, no-op store) — the OOB path
  reaches the runtime helper (`TypedArrayBase::SetItem` ->
  `DirectSetItem` -> `ToBigInt64Element` -> `ToBigInt`, all correct).
- OOB Number store: throws `TypeError: Type mismatch` (correct per spec).
- In-bounds Number/null/undefined stores: suite passes (those value kinds take
  different lowering paths, see below).
- Without profile / interpreted: all correct.

So the defect is exactly: **in-bounds** store of a **non-primitive TyVar**
value on a BigInt-element typed array skips ToBigInt and throws.

## Suspect site (read at HEAD, mechanism inferred from probes)

`Lowerer::GenerateStElem`-family, `lib/Backend/Lower.cpp:17782-17815` (HEAD):

```cpp
if (*pIsTypedArrayElement && isStore)
{
    ...OOB -> labelInlineSet (nop)...
    // For typed array, call ToNumber before we fallThrough.
    if (instr->GetSrc1()->GetType() == TyVar && !instr->GetSrc1()->GetValueType().IsPrimitive())
    {
        ...
        LowerUnaryHelperMem(toNumberInstr, IR::HelperOp_ConvNumber_Full);
    }
    InsertBranch(Js::OpCode::Br, labelFallthrough, instr);  // inline raw store
```

- There is NO BigInt-specific handling anywhere in `lib/Backend`; the fast
  path unconditionally applies ToNumber (`HelperOp_ConvNumber_Full`).
- `*pIsTypedArrayElement` is set from `baseValueType.IsLikelyTypedArray()`
  (`Lower.cpp:17260`), whose range (`Int8Array..CharArray`) INCLUDES the
  BigInt element types `Int64Array`/`Uint64Array` (`ValueTypes.h:119-120`,
  documented "not optimized by the JIT" yet still admitted to this path).
- With PGO the base is known typed-array so the fast path is taken; without
  PGO it goes generic-helper (correct). `view[0] = <int>` etc. take other
  (typed-src) lowering branches, which is why only the Object case visibly
  breaks.
- `ToNumber(object-3n)` throws `Type mismatch`, matching the observed error
  (vs the runtime slow path, which correctly calls
  `ToBigInt64Element`/`ToBigUint64Element` in `TypedArray.cpp:37-45`).

Suggested fix direction (follow-up lane): gate the ToNumber emission on a
provably-non-BigInt base AND route the BigInt case to `labelHelper` (just
skipping the conversion would fall through to the raw inline store with an
unconverted Var — unsafe). A `TypedArrayBase::IsBigIntElementType()`-style
predicate exists as a model (`TypedArray.h`), but Lower works in ValueType
terms, so a small ValueType predicate (Int64Array/Uint64Array range) is needed.
Verify with this card's PGO recipe + full dynapogo + x64 leg (shared file).

## Evidence it predates e14

Same as sibling cards: identical failure (line 173) at 69281b6e4 and 6c1470f9d;
test file unchanged since e14; e14 dynapogo leg never fed profiles. The
ToNumber fast path + BigInt-array combination dates to the fork's BigInt64Array
work (C2-S2, Oct 19), not to any post-e14 lane.

## Follow-up guidance

JIT-owner lane: implement the helper reroute, regression = this file 10/10
under the exact two-step PGO recipe above (profile is path-keyed; run from
`test/es7/`) + es7 folder dynapogo + no-JIT gates (shared lowering file —
confirm x64 unaffected).
