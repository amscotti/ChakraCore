# C2-S1: async/generator private methods (instance and static)

Status: **implemented on `lane/s1-private-async-gen`** (base `0c01875b5`);
gates **2012/2064** (unchanged, count-preserving: no new test files).
Evidence: `test/es7/class-private-methods.js` 33/33 and
`test/es7/class-static-private-fields.js` 15/15 in normal,
`-force:deferparse` and `-noDeferParse` modes on release and debug-static
binaries; full `runtests.py --variants disable_jit` **2012 passed / 0
failed** on both binaries; `verify-bytecode` green. Scoped pinned-corpus
(`2e0a5676`) class async/generator-private subset: **272 fail-to-pass /
0 pass-to-fail** vs the base binary (704/704 scenarios pass).
A 35-case early-error matrix and a behavior matrix agree exactly with Node
26.5.0 in all three parse modes. This slice implements `async #m(){}`,
`*#m(){}`, `async *#m(){}` and the `static` forms. Async/generator private
accessors, static blocks, computed names and decorators remain out of
scope.

## Scope and observable semantics

- Installed functions have the correct kind: `AsyncFunction`,
  `GeneratorFunction`, `AsyncGeneratorFunction` (matching the
  corresponding public methods and Node, including `.constructor.name`),
  the source-spelling name `"#m"` and `length` from the formal parameters.
- Brand checks are identical to sync private methods: `LdPrivateMethod`
  checks the receiver brand and returns the shared function; writes throw
  `TypeError`; `#m in obj` is an own-brand check (`false` for unbranded
  objects, `TypeError` for primitives). Static brands belong to the
  defining constructor only. Duplicate private-name combination rules are
  unchanged across kinds (`async #m` + `#m`, `*#m` + `async *#m`,
  instance + static redeclarations, and `get #x` + `async #x` are all
  early errors through the existing `PrivateNameEntry` machinery).
- Generator behavior is synchronously observable: `yield`, `yield*`,
  `return()` and `throw()` through the private method. Async methods
  support `await` in the body and return promises; async generators support
  `await` + `yield` and `for-await` consumption.
- Line-terminator rule: `async [no LineTerminator here]` before the
  private name/star, matching Node exactly. `async\n#m(){}` parses as a
  field named `async` plus a sync private method; `async\n*#m(){}` as a
  field plus a generator; `*\n#m(){}` as a generator (no LT restriction
  between `*` and the name); `#x\n*gen(){}` as a private field plus a
  public generator via the existing ASI field rule. Same-line
  `async #x;`, `async #m = 1`, `*#m = 1` and `#x *gen(){}` stay
  `SyntaxError`, as in Node.
- Early errors: `async`/` *` on private accessors
  (`async get #x`, `*set #x`, `async *get #x`, including mixed
  `get #x` + `async set #x`) reject, as do `async static #m` (wrong
  modifier order) and `#constructor` in any async/generator spelling.
- Works in base and derived classes; `super` calls inside async/generator
  bodies resolve against the prototype (instance) or the base constructor
  (static), mirroring public methods. Instances created before prototype
  manipulation or freezing keep working because brands install at
  construction.

## Parser/AST design

One localized reorder in `Parser::ParseClassDecl` (`lib/Parser/Parse.cpp`):
the `async`-modifier lookahead and the `*` generator check now run
*before* the `tkPrivateName` consumption instead of after it (the old
`!isPrivateField` gate and the `!(isPrivateField && FHadNewLine)` guard
are removed as dead: at element start no private name of this iteration
has been consumed yet). Everything downstream is untouched:

- `isAsyncMethod`/`fFncGenerator` flow into the existing private-method
  branch, which rewinds to the private-name token and runs the ordinary
  `ParseFncDecl` method path, so function flags, `yield`/`await` keyword
  regions, home objects, deferred stubs and hints behave exactly like the
  public async/generator methods. Function hints keep the `#m` spelling.
- `isFieldDefinition` still rejects `async #x = 1` / `async #x;` via the
  `isAsyncMethod` guard, and `*#m = 1` / `#x *gen(){}` fall through to the
  method grammar, which fails on the unexpected token. The accessor path
  rejects `isAsyncMethod || isGenerator` before declaring the name, so no
  half-pair state leaks.
- Fast-scan (`buildAST == false`) and full parse share the identical
  token-driven logic; duplicate/private-environment errors fire in both.
  No new opcodes, PropertyIds, or runtime handlers: `SetPrivateDefinition`
  stores whatever function object `Emit(pnodeFnc)` creates, and brand
  install/dispatch opcodes are receiver-agnostic.

### Ownership/lifecycle record

- **Resource**: no new allocation. Reuses the per-name `JavascriptWeakMap`
  brand record (plus hidden accessor record) from the sibling slices; the
  stored definition is now an async/generator function object instead of a
  sync one.
- **Owner / invalidation / thread-context**: unchanged from
  [c2-s1-private-methods.md](c2-s1-private-methods.md): class-block const
  owns the map, weak per-instance/constructor keys, current-thread script
  execution, no async job or cross-thread access. Awaiting/yielding runs
  user code with a rooted receiver, exactly like awaiting in a public
  async method.
- **Failure**: wrong/primitive receiver, duplicate install, method writes
  throw `TypeError` through the existing paths; async rejection
  propagates as an ordinary rejected promise.

## Evidence

Build (Mac ARM64), local `.zig-cache`:

- `mise x -- zig build check --cache-dir "$PWD/.zig-cache"` — PASS
  (regression **2012 passed / 0 failed**, bytecode/native/smoke/metadata
  green).
- `mise x -- zig build check-regression -Dprofile=debug-static
  --cache-dir "$PWD/.zig-cache"` — PASS (regression **2064 passed /
  0 failed**).
- `mise x -- zig build verify-bytecode --cache-dir "$PWD/.zig-cache"` —
  green; checked-in nojit bytecode headers unchanged (no new opcodes).

Focused tests (`disable_jit`):

- `test/es7/class-private-methods.js` — **33/33** (25 existing + 8 new
  async/generator cases) in normal, `-force:deferparse` and
  `-noDeferParse` modes on release and debug-static binaries, plus the
  debug-static `-force:DeferParse -UseParserStateCache -ParserStateCache`
  mode.
- `test/es7/class-static-private-fields.js` — **15/15** in the same three
  modes on both binaries (2 stale async/generator rejections updated to
  `doesNotThrow`).
- `test/es7/class-private-fields.js`, `test/es7/class-static-fields.js` —
  pass in all three modes (unaffected).
- `python3 test/runtests.py -t -b "$PWD/zig-out/bin/ch" --variants
  disable_jit` — **2012 passed / 0 failed**; debug-static binary —
  **2064 passed / 0 failed** (`es7` has no new files, so
  `tools/build/gate_counts.json` is unchanged at test 2012 /
  debug-static 2064).
- Scoped pinned test262 subset (corpus `2e0a5676`, host flags
  `-nonative -Test262 -CollectGarbage`, controls green): the dedicated
  async/gen private-method directories under
  `test/language/{statements,expressions}/class/elements/`
  (`async-private-method`, `async-private-method-static`,
  `async-gen-private-method`, `async-gen-private-method-static`,
  `gen-private-method`, `gen-private-method-static`; 360 files):
  base binary **160 passed / 544 failed** of 704 scenarios
  (`class-methods-private` 80/272) vs slice binary **704 passed /
  0 failed** (`class-methods-private` 352/0). Per-file diff:
  **272 fail-to-pass, 0 pass-to-fail**. Raw outputs:
  `out/test262-results-asyncgen-base/` and
  `out/test262-results-asyncgen-new/` (scratch outputs, not committed).
- Node-differential probes (in `/tmp`, not committed): a 35-case
  early-error matrix (all six valid spellings, accessor rejections,
  wrong-order `async static`, cross-kind duplicates, field-form
  rejections, newline splits) agrees exactly with Node 26.5.0 in all
  three parse modes; a behavior matrix (kind/name/length/`instanceof
  Promise`, yield/yield\*/return/throw, await values, for-await sums,
  super in derived/static bodies, brand/`in` checks, post-freeze and
  post-`setPrototypeOf` access) matches Node value-for-value.

Test-pattern note: promise completion settles after the test body
returns, and unhandled rejections are silent under `ch`, so the file
routes all completion assertions through one chained
`testRunner.LoadScript(..., explicitAsyncTestExit=true)` snippet whose
single `_asyncExit` settles every step; any mismatch rejects the chain
and fails the test (verified with a mutated expectation: exact FAILED
attribution; restored file passes). Multiple concurrent snippets are
deliberately avoided: the wrapper publishes one global `_asyncExit`,
so a second snippet would overwrite the first one's exit.

## Residual work and limitations

- Async/generator private *accessors* are `SyntaxError`, matching public
  accessors and the language grammar (not in the language).
- Static blocks, computed class-element names and decorators are not
  implemented and remain `SyntaxError` (unchanged).
- `super.#m` is untouched (known out-of-scope item from the sibling
  slices).
- Direct eval cannot resolve private names (pre-existing residual,
  identical for sync members).
- Brands are `JavascriptWeakMap` entries rather than internal slots;
  GC-stress and weak-key lifetime under heavy pressure are not separately
  exercised.
- No JIT/backend support is claimed. **B4 must run the es7 private/static
  tests under SimpleJIT/FullJIT (dynapogo) before any JIT or default
  claim.**
- No new config flag or kill-switch, consistent with the sibling private
  slices.
- Test262 coverage here is the dedicated async/gen private-method
  subset only. The broader private-methods corpus (sync methods, accessors,
  brand checks, `private-methods/prod-*.js`, same-line/new-line RS tests)
  was already measured by the sibling slices and is covered for
  regressions by the green 2012/2064 in-tree suites; a full-corpus
  comparison was not re-run.

## Files

- `lib/Parser/Parse.cpp`: async/star detection moved before private-name
  consumption in `ParseClassDecl`; stale "never reaches this path"
  comment replaced with the async/generator private-method contract.
- `test/es7/class-private-methods.js`: 8 new cases (async kind/name/
  length, generator protocol, async-generator protocol, static forms,
  derived+super, prototype-manipulation brands, early-error/LT matrix,
  chained async completions); stale static async/generator rejections
  updated; header comment updated.
- `test/es7/class-static-private-fields.js`: 2 stale async/generator
  rejections updated to `doesNotThrow`; comment updated.
