# A2: bytecode serialization drops the lexical [[HomeObject]] marker

Lane `a2-serialization-fix`, base `modernize/baseline` (`a1e1f6b1b`).

## Bug

Commit `851d0646f` added a codegen-propagated lexical `[[HomeObject]]` marker
(`ParseableFunctionInfo::m_hasHomeObjectContext`,
[FunctionBody.h:1836](lib/Runtime/Base/FunctionBody.h:1836)) that
`GlobalObject::PerformEval` consults for the direct-eval `inMethod` decision
([GlobalObject.cpp:620-622](lib/Runtime/Library/GlobalObject.cpp:620)).
Codegen sets it (`ByteCodeGenerator.cpp:2694-2697` deferred reuse,
`:2916-2930` post-visit propagation) but the bytecode serializer never
persisted it: the `FunctionFlags` bitmask
([ByteCodeSerializer.cpp:212-237](lib/Runtime/ByteCode/ByteCodeSerializer.cpp:212))
had no bit for it, the `AddFunction` writer (`:2289-2311`) never set one, and
the `ReadFunctionBody` reader (`:4234-4249`) never restored it. `FunctionBody`
construction from cache defaults the marker to false
([FunctionBody.cpp:1702](lib/Runtime/Base/FunctionBody.cpp:1702)).

Result: bytecode-resident arrows lost their eval-super binding. A method arrow
doing `eval("super.value")` works from source (deferred reparse recomputes the
marker) but throws `SyntaxError: Invalid use of the 'super' keyword` when the
function body comes from serialized bytecode. Direct methods were unaffected
(their `FunctionInfo::HasHomeObj` is serialized via attributes and covers the
`evalInMethod` check independently).

Reviewer trap respected: `-ForceSerialized` does not round-trip the main
script (no-ops in `Jsrt.cpp` and `GlobalObject.cpp`); the genuine round-trip
CLI is `-Serialized`.

## Evidence (pre-fix, `out/jit/bin/ch` from unpatched baseline)

`/tmp/a2_homeobj.js`:

```js
class Base { get value() { return 41; } }
class Derived extends Base {
    arrowCaller() {
        var call = () => eval("super.value");
        return call();
    }
    method() { return eval("super.value"); }
}
```

- `ch /tmp/a2_homeobj.js` &rarr; `arrowCaller=41`, `method=41`
- `ch -Serialized /tmp/a2_homeobj.js` &rarr;
  `arrowCaller-THREW:SyntaxError:Invalid use of the 'super' keyword`,
  `method=41`

Breadth repros `/tmp/a2_homeobj2.js` (wrapper/escaped/nested/eval-created
arrows) and `/tmp/a2_homeobj3.js` (async arrow) cover the same marker through
more deserialization paths.

## Fix

`lib/Runtime/ByteCode/ByteCodeSerializer.cpp`, three lines following the exact
precedent of commit `2bdfc7c8b` (which added `ffIsMethod`/`ffIsClassMember` the
same way, with no format-version rotation — old readers mask unknown bits
out, old caches simply read the new bit as clear):

1. New `FunctionFlags` bit `ffHasHomeObjectContext = 0x4000000` (next free bit
   after `ffIsClassMember`).
2. Writer `ByteCodeBufferBuilder::AddFunction`: OR in
   `function->m_hasHomeObjectContext ? ffHasHomeObjectContext : 0` with the
   other `ParseableFunctionInfo`-level flags.
3. Reader `ByteCodeBufferReader::ReadFunctionBody`: restore
   `(*function)->m_hasHomeObjectContext` from the bit next to `m_isMethod` /
   `m_isClassMember`. The restore sits on the shared `ParseableFunctionInfo`
   pointer, so it covers both the `FunctionBody` (`has_ConstantCount`) and
   the deferred `ParseableFunctionInfo` paths, including stubs materialized
   later through `DeferDeserializeFunctionInfo::Deserialize`.

No `SerializableFunctionFields.h` entry and no cache-GUID rotation, matching
the `m_isMethod` precedent: the bit rides in the existing `BitFlags` word.

Regression test: `test/es7/rlexe.xml` gains a fourth `eval-super-early.js`
entry with `-Serialized` (plus `exclude_forceserialized`, the convention used
by the other `-Serialized` entries), closing the matrix gap where the file ran
only under default / `force:deferparse` / `noDeferParse`.

Required follow-through: builtin bytecode headers. The fix changes
serialization output for 34 Intl helper functions (arrows nested in
object-literal methods, which inherit the marker per the codegen propagation
rule): their `BitFlags` words flip `0x01C141A2`→`0x05C141A2` /
`0x00D041A2`→`0x04D041A2` — in every case exactly `+0x04000000`, the new bit.
Causation was proved by regenerating `Intl.js` bytecode with the pre-fix
binary (byte-identical to the checked-in header) versus the fixed binary
(34 one-byte flips). So per the repo process (`doc/b5-guid-rotation.md`) a
dual-variant regen was run:

- Staged variant-correct binaries at `out/noJit/debug/` (test profile
  `zig-out/bin/ch` + dylib) and `out/jit/debug/` (`out/jit/bin/ch` + dylib).
- `python3 tools/regenByteCode.py --skip-build` regenerated all four headers
  (`JsBuiltIn[.nojit].bc.[64b|32b].h`) and minted a new cache GUID
  `8cac7d38-1737-440d-8dde-6202814d0e16` (was
  `55c5da6d-dcde-42f9-8112-f7f698fcc96c`), since the format changed.
- Binaries were rebuilt afterwards because the headers are compiled into the
  engine.

## Verification

Built with the repo gate: `/tmp/opencode/cc-gate-lock.sh mise x -- zig build
-Dprofile=jit` (exit 0; `out/jit/bin/ch` + `libChakraCore.dylib` installed).
Pre-fix binary reproduced the bug (`arrowCaller-THREW:SyntaxError` under
`-Serialized`); post-fix binary:

- `/tmp/a2_homeobj.js`, plain and `-Serialized`: `arrowCaller=41`,
  `method=41` in both modes.
- `/tmp/a2_homeobj2.js`, plain and `-Serialized`: `wrap=1`, `escaped=1`,
  `nested=1`, `evalArrow=1` in both modes.
- `/tmp/a2_homeobj3.js`, plain and `-Serialized`: `asyncArrow=5` in both
  modes.

Test suites (all through `/tmp/opencode/cc-gate-lock.sh`):

- Repros on the rebuilt JIT binary (`out/jit/bin/ch`, final headers):
  `arrowCaller=41` / `method=41`,
  `wrap=escaped=nested=evalArrow=1`, `asyncArrow=5` — identical in plain
  and `-Serialized` modes.
- New `eval-super-early.js -Serialized` entry: prints `pass` with the fixed
  binary in all four flag combos (default / `-force:deferparse` /
  `-noDeferParse` / `-Serialized`); on the pre-fix binary the `-Serialized`
  combo throws an uncaught `SyntaxError` (failing the test), the other three
  pass — so the entry guards exactly this fix.
- `test/runtests.py --test --variants=disable_jit` on `test/es7` (absolute
  path; the runner silently selects zero tests for a relative folder path):
  `es7: passed 110, failed 0`. Same for `--debug --variants=disable_jit`:
  `110/0`.
- Full `zig build check` (default test profile): exit 0.
  `check_regression: parsed=(2055, 0) require passed 2055 / failed 0`;
  `verify_bytecode_pure`: `Bytecode is up to date` (after regen);
  native/smoke/metadata/self-tests all pass.
- Gate pin bumped accordingly: `tools/build/gate_counts.json`
  test 2054→2055, debug 2106→2107 (debug selection verified on es7 at
  110/0 with the test binary; the full debug-static gate was not run).

Note: an early folder-scoped `runtests.py es7` invocation with a relative
path silently ran zero tests (exit 0); always pass `$PWD/test/es7` or run
from `test/`. The debug-static full gate was not run (separate heavyweight
build); only the es7 debug selection was exercised.
