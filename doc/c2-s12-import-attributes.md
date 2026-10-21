# C2-S12: import attributes (`with`) end to end

Status: **implemented on `lane/s12-import-attributes`** (base `aab65ecfb`);
gates hold **2012/0 (test) and 2064/0 (debug-static)** with no
scenario-count movement (`tools/build/gate_counts.json` untouched; cases
added to already-registered files, one `.json` data fixture added beside
them). Out of scope: JIT lowering, `import.meta`, workers, `assert`-keyword
syntax, source-phase (`source`/`bytes`/`text`) imports.

## Scope and observable semantics

- Static `with` clauses parse on every position the grammar allows them:
  side-effect, default, named and namespace `import ... from`, and
  `export *` / `export { ... } from` re-exports
  (`ParseImport`, `ParseImportOrExportFromClause` in
  `lib/Parser/Parse.cpp`). `with (...)` is rejected; only `with { ... }`.
- Attribute keys are `IdentifierName` or string literals (reserved words
  allowed, numeric keys rejected); values must be string literals; duplicate
  keys after escape processing are the early error
  `ERRExpected ... duplicate` (`ERRDuplicateModuleAttribute` in
  `lib/Parser/perrors.h`); trailing commas allowed; a line terminator may
  precede `with`.
- Parse-time attribute lists are **sorted by key**, so
  `[[RequestedModules]]` identity and the child-module map are
  order-insensitive: `with { a: "1", b: "2" }` and `with { b: "2", a:
  "1" }` are the same request; missing `WithClause` and `with {}` are the
  empty request (`ModuleAttributeList`, `ModuleRequestList`,
  `ModuleRequestEntry` in `lib/Parser/ParserCommon.h`).
- Dynamic `import()` accepts an optional second `options` argument plus a
  trailing comma (`ParseImportCall`). ES2025 `GetOptionsObject`/
  `GetImportAttributes` semantics in `TryGetImportAttributes`
  (`lib/Runtime/Language/JavascriptOperators.cpp`): `undefined` means no
  attributes; `null` or any other non-`Object` (including primitive strings,
  numbers, symbols, bigints — `IsObject` is `TypeId`-based, so strings are
  rejected) is a `TypeError`; a `with` value that is `null` or a non-object
  is a `TypeError`; a non-string attribute value is a `TypeError`.
  Getter/proxy/symbol-key behavior: only own enumerable string-keyed
  properties are read; abrupt completions reject the import promise.
- Attribute-sensitive module identity: `GetModuleRequestKey`
  (`lib/Runtime/Language/SourceTextModuleRecord.cpp`) joins specifier and
  sorted `key=value` pairs with U+001F separators (doubled-separator
  escaping), so the same specifier with different attributes evaluates
  separately and identical attributes share the record.
  `BuildModuleAttributesObject` materializes the attributes object passed
  to the host; `GetChildModuleRecord` gained an attribute-taking overload
  and `requestedModuleList`/`SetParent`/`GetExportedNames`/
  `ModuleEvaluation`/`Initialize*` thread attributes through.
- Host contract is **purely additive** (`lib/Jsrt/ChakraCore.h`,
  `lib/Jsrt/Core/JsrtContextCore.h/.cpp`, `lib/Jsrt/Core/JsrtCore.cpp`,
  `lib/Runtime/Base/ScriptContext.h`): new
  `JsModuleHostInfo_FetchImportedModuleWithAttributesCallback (0x9)` /
  `..._FromScriptWithAttributesCallback (0xA)` and the matching
  `FetchImportedModuleWithAttributesCallBack` typedefs.
  `HostScriptContext::FetchImportedModule` takes an attributes parameter
  and routes to the new callback when present; when attributes are absent
  the legacy `(referencingModule, specifier)` path is untouched; when
  attributes are present but the host registered no new callback, the fetch
  fails with `E_NOTIMPL`, surfaced as a `TypeError` rejection.
- `ch` host (`bin/ch/WScriptJsrt.h/.cpp`,
  `bin/ch/ChakraRtInterface.h/.cpp`) registers both new callbacks:
  `ReadModuleAttributes` reads the attributes object; `type: "json"` loads
  via `WrapJsonModuleSource`, which synthesizes
  `export default JSON.parse('<escaped>')` (escaping `'`, `\` and line
  terminators incl. U+2028/U+2029) so `__proto__` stays an own data
  property, the prototype is `Object.prototype`, the namespace exposes
  **only** `default`, and invalid JSON is a `SyntaxError` at evaluation.
  Attribute-bearing requests get an attribute-suffixed `moduleKey` parsed
  through `ModuleMessage::CreateWithAttributes` + `directParse`
  (`ParseModuleSourceOnRecord`) instead of the path-keyed
  `LoadModuleFromString`; empty attributes keep the legacy plain-path key.
  Non-empty unsupported `type` values fail the fetch (dynamic: `URIError`
  rejection; static: visible link error). `ParseSource` prefers the recorded
  `errorObject` over a synthesized compile error, and `PrintException`
  gained a position-less `at code (file:?)` fallback, so attribute fetch /
  link failures are visible instead of silent.
- Bytecode: one appended opcode `OP_ImportCallWithOptions` /
  `ImportCallWithOptions` (`lib/Runtime/ByteCode/OpCodes.h`,
  `ByteCodeEmitter.cpp` `EmitImportCall`, `InterpreterHandler.inl`
  `A2toB1`-style wiring, `InterpreterStackFrame.h/.cpp`). Appended at the
  end of the extended list so existing opcode numbering and checked-in
  bytecode are untouched (`verify-bytecode`: "Bytecode is up to date").
  No JIT lowering by design (interpreter-only until B4 lowers it;
  `Lower.cpp` fatals on the unlowered opcode under JIT, so verification is
  no-JIT/`disable_jit`).

## Parser/AST design

- `ParseOptionalWithClause` / `ParseWithClause` build a sorted
  `ModuleAttributeList` with duplicate detection; keys are stored as
  `IdentPtr`s, values are string literals only.
- `ParseNodeModule.requestedModules` is now a `ModuleRequestList` of
  `{ specifier, attributes }` pairs; static import/export entries carry
  `ModuleImportOrExportEntry.attributes` for the emitter and the runtime
  linker.
- Deferred/fast-scan/full-parse agreement is structural (no new scan-only
  shape): the clause is parsed by the same routine in every mode; verified
  in normal, `-forcedeferparse` and `-nodeferparse` modes (see Evidence).

## Bytecode/runtime design

No existing opcode number changes. `EmitImportCall` emits the legacy
`OP_ImportCall` for one-argument `import(spec)` and the appended
`OP_ImportCallWithOptions` once a second argument (or trailing comma) is
present; the interpreter's `A2`-form handler threads both operands into
`ImportCallImpl`, which validates options via `TryGetImportAttributes`
before touching the module map, so invalid options reject without
recording anything. Static linking resolves each `ModuleRequestEntry`
through the attribute-sensitive `GetModuleRequestKey` child map before
calling the host, and `ModuleEvaluation` propagates the request
attributes to nested evaluations.

### Ownership/lifecycle record

- **Resources**: one sorted `ModuleAttributeList` per static request
  (parse-time, recycler-owned via the parse-node arena); one fresh ordinary
  attributes object per validated dynamic import (`TryGetImportAttributes`
  builds it on the `ScriptContext` recycler); one synthesized JSON source
  string per `type: json` fetch (host-side, freed with the fetch).
- **Owner**: the `SourceTextModuleRecord` owns its `requestedModuleList`
  keys (including attribute suffixes in the child map); the attributes
  object passed to the host is rooted by the fetch call arguments for the
  duration of the fetch/link. No raw pointers cross the host boundary —
  attributes cross as a `JsValueRef` object (new callbacks) or not at all
  (legacy callbacks).
- **Invalidation**: none after link. Attribute keys/objects are ordinary
  GC-managed values; the child-module map holds records, not raw source.
- **User-code/reentrancy**: attribute objects are user objects — reads go
  through `GetProperty`/`GetOwnPropertyKeys` with implicit calls enabled,
  so getters, proxies and `Symbol` enumeration run user code and abrupt
  completions reject the dynamic import (static linking reports the
  recorded error object via `ParseSource`). `ch`'s JSON synthesis quotes
  but never evaluates file content (`JSON.parse` at module evaluation).
- **Failure**: `E_NOTIMPL` (host without the new callbacks + attributes
  present) becomes a `TypeError`; fetch failure becomes a link error
  (static) or promise rejection (dynamic); parse/eval failure of a JSON
  body is a `SyntaxError` at that module's evaluation.
- **Thread/context**: current-thread script execution on the active
  `ThreadContext`; host callbacks run on the same thread as the fetch. No
  new async job, TLS or cross-thread access.

## Evidence

Builds (Mac ARM64):

- `mise x -- zig build` — green, `zig-out/bin/ch` v1.13.0.0-beta
  (sha256 prefix `2909e974a38d2f4f`).
- `mise x -- zig build check` — rc=0: `Total: passed 2012, failed 0`
  (matches `tools/build/gate_counts.json` `test 2012`); `check_native`,
  `check_metadata` (minos, stacks, headers byte-identical, 8 JsRT exports),
  `check_smoke`, `verify-bytecode` ("Bytecode is up to date") all PASS.
- `mise x -- zig build check-regression -Dprofile=debug-static` —
  **PASS: Total: passed 2064, failed 0** (matches `debug-static 2064`).

Focused in-tree suites (release `ch`, `-nonative -ES6Module
-ESDynamicImport -MuteHostErrorMsg`, all three parse modes):

- `test/es6module/module-syntax.js` — **11/11** normal /
  `-forcedeferparse` / `-nodeferparse` (new: valid-`WithClause` entry —
  empty/default/named/namespace/star/re-export, string-literal keys,
  reserved-word keys, trailing comma, newline before `with`, three JSON
  module entries incl. `__proto__`-as-data and default-only namespace;
  new syntax-error entry — duplicates incl. post-escape duplicates,
  non-string values, missing colon, numeric keys, unterminated brace,
  duplicate keys in `export-from`; stale 3-arg `import()` assertion updated
  to ES2025 options-accepting behavior).
- `test/es6module/dynamic-module-functionality.js` — **21/21** in all
  three modes (new: options-argument entry — `undefined`/`{}`/
  `{with:{}}`/`{with:undefined}`/unknown-attrs fulfill; `null`, numeric
  options, non-object/string `with`, non-string value reject `TypeError`;
  unsupported `type` rejects; JSON dynamic import fulfills with parsed
  `default`; trailing-comma single-arg fulfills).
- `test/es6module/dynamic-module-import-specifier.js` — **3/3** in all
  three modes (stale 2-arg `import()` rejection updated to fulfillment).
- Full `test/es6module/` via `runtests.py --variants disable_jit`:
  **2012 passed, 0 failed**.

Manual probes (`/tmp/s12probe`, new `ch`): static plain/empty/plain-attr
loads pass; `with { type: "json" }` static gives `default.answer` with own
`__proto__` and default-only namespace; dynamic with attrs fulfills;
`null`/`with:23`/`type:23` reject `TypeError`; static bogus type is a
visible link error; `node --check` clean on both edited `.js` files.
`node v26.5.0` used as differential oracle for error classes on probes.

test262 (`tools/test262/run.py`, pinned checkout
`/Users/ascotti/Documents/code/ChakraCore/out/test262`,
`test/language/import/**/*.js` + `test/language/module-code/**/*.js`,
737 scenarios / 728 files; baseline binary
`/Users/ascotti/Documents/code/ChakraCore/zig-out/bin/ch` 1.13.0.0-beta):

- Lane: **565 passed / 172 failed**. Baseline (same patterns):
  **553 passed / 184 failed**.
- **fail-to-pass: 11**, all in scope: ten
  `language/import/import-attributes/json-*.js` (value-array/boolean/null/
  number/object/string, extensibility-array/object, idempotency,
  via-namespace) plus
  `language/module-code/import-attributes/import-attribute-empty.js`.
- **pass-to-fail: 0.**
- One 12th delta
  (`module-code/top-level-await/unobservable-global-async-evaluation-count-reset.js`)
  failed only in the baseline full-corpus run and passes 3/3 on both
  binaries in isolation — parallel-load timing flake in a TLA
  evaluation-count test untouched by this lane; not counted.
- Remaining in-scope failures are pre-existing on both binaries
  (out-of-scope proposals): `import-attributes/text-*.js` (5, `type:
  "text"` source-phase), `import-bytes/bytes-from-json.js` (source
  phase), `import-defer/*` (2, `import defer`), and
  `top-level-await/dynamic-import-of-waiting-module.js` (2 scenarios,
  TLA/dynamic-import orchestration).

## Fix round (review loop): string options accepted + uninstantiated generators

Two defects found while diagnosing test #21 ("Dynamic import with options
argument") fulfilling all five `rejectBody` subcases under
`testRunner.LoadScript` while standalone probes rejected correctly:

- D1 engine (`lib/Runtime/Language/JavascriptOperators.cpp`,
  `TryGetImportAttributes`): options/`with` validation used
  `VarIs<RecyclableObject>`, which admits primitive strings
  (`JavascriptString` derives from `RecyclableObject`), so
  `import(x, "any string")` was silently treated as no-options. Fixed to
  `JavascriptOperators::IsObject`, which is `TypeId`-based
  (`TypeIds_String = 7 < TypeIds_LastJavascriptPrimitiveType = 22`), so
  every primitive — strings, numbers, symbols, bigints — and `null`
  reject with `TypeError`, matching ES2025 `GetOptionsObject`. Same fix
  for the `with` value.
- D2 test (`test/es6module/dynamic-module-functionality.js`):
  `fulfillBody`/`rejectBody` interpolated `" + optionsSource + "` /
  `" + checkSource + "` **inside template literals**, where that is
  literal text, not concatenation — every generated case actually ran
  `import('ModuleSimpleExport.js', " + optionsSource + ");`, i.e. a
  single primitive-string second argument. That is why the subcases
  fulfilled (via D1) in-harness and standalone alike. Fixed to
  `${optionsSource}` / `${checkSource}`, and added two regression
  subcases (`import(x, 'options string')` and `{ with: 'str' }` both
  reject `TypeError`).

After both fixes the entry passes 21/21 repeatedly in normal,
`-forcedeferparse` and `-nodeferparse` modes (one transient 2-subcase
fulfillment observed on the first post-fix run, unreproduced in 5+
subsequent runs of both harness modes — async timing, not a code path).

Also in this round: removed a trailing-whitespace line in
`SourceTextModuleRecord.cpp`, and added a `.gitattributes`
`whitespace=cr-at-eol` line for `test/es6module/module-syntax.js`
(CRLF file, same rationale as the S9/S11/C2-S14 entries) so the appended
CRLF lines are not flagged by `git diff --check` (now clean).

## Residual work and limitations

- **B4 must lower `OP_ImportCallWithOptions`** (`Lower.cpp` fatals on the
  unlowered opcode under JIT) and run the es6module suites under
  SimpleJIT/FullJIT (dynapogo) before any JIT or default claim. All lane
  verification is interpreter/`disable_jit`.
- `assert`-keyword attribute syntax is not accepted (ES2025 removed it;
  out of scope).
- `type` values other than `"json"` are host-rejected (fetch failure),
  including on otherwise-valid JS — `ch` only synthesizes JSON modules.
  Unknown non-`type` keys pass through to the host and are ignored.
- Dynamic `import()` of a JSON module in `ch` resolves relative to the
  referring script path; remote URLs are unsupported (pre-existing host
  limitation).
- `JsGetOwnPropertyNames` was added to the `ch` hook table for the
  host `ReadModuleAttributes` enumeration; older `ch` binaries without the
  export leave a null hook — hosts must take the lane's `ch` (or provide
  the export).
- Static-link attribute read errors surface through the recorded
  `errorObject`; if a host reports neither, the position-less `at code
  (file:?)` fallback prints instead of staying silent.

## Files

- `lib/Parser/ParserCommon.h`: `ModuleAttributeEntry/List`,
  `ModuleRequestEntry/List`; `ParseNodeModule.requestedModules` is now a
  `ModuleRequestList`.
- `lib/Parser/ptree.h`, `lib/Parser/Parse.h`, `lib/Parser/Parse.cpp`:
  `ParseOptionalWithClause`/`ParseWithClause`, wiring into
  `ParseImport`/`ParseImportOrExportFromClause`/`ParseImportCall`
  (optional 2nd `options` arg + trailing comma).
- `lib/Parser/perrors.h`: `ERRDuplicateModuleAttribute`.
- `lib/Runtime/Language/SourceTextModuleRecord.h/.cpp`:
  `GetModuleRequestKey`, `BuildModuleAttributesObject`,
  attribute-taking `GetChildModuleRecord` overload, threaded
  `requestedModuleList`/`SetParent`/`GetExportedNames`/`ModuleEvaluation`/
  `Initialize*`.
- `lib/Runtime/Language/JavascriptOperators.h/.cpp`:
  `TryGetImportAttributes`, `ImportCallImpl` (fix round: `IsObject`
  validation).
- `lib/Runtime/ByteCode/OpCodes.h`, `ByteCodeEmitter.cpp`,
  `InterpreterHandler.inl`, `InterpreterStackFrame.h/.cpp`:
  appended `OP_ImportCallWithOptions` + `EmitImportCall` + interpreter
  wiring.
- `lib/Jsrt/ChakraCore.h`, `lib/Jsrt/Core/JsrtContextCore.h/.cpp`,
  `JsrtCore.cpp`, `lib/Runtime/Base/ScriptContext.h`: additive host
  contract (`0x9`/`0xA` callbacks, attributes-plumbed fetch).
- `bin/ch/ChakraRtInterface.h/.cpp`, `bin/ch/WScriptJsrt.h/.cpp`:
  callback registration, `ReadModuleAttributes`,
  `WrapJsonModuleSource`, `FetchImportedModuleHelperWithAttributes`,
  `ModuleMessage::CreateWithAttributes` + `directParse`, attribute-suffixed
  keys, `PrintException`/`ParseSource` hardening.
- `test/es6module/module-syntax.js`, `dynamic-module-functionality.js`,
  `dynamic-module-import-specifier.js`: coverage entries (fix round:
  generator interpolation + string-primitive regression subcases).
  `test/es6module/importattrs-fixture.json`: new JSON-module data fixture
  (headerless by data-file convention, like `.baseline` files; not an
  `rlexe.xml` entry — not a runnable test).
- `.gitattributes`: `test/es6module/module-syntax.js whitespace=cr-at-eol`.
- `tools/build/gate_counts.json`: untouched (2012/2064 hold).

## Fix round (loop-1 review defects D1-D3)

All three loop-1 defects reproduced on the lane binary before fixing
(D1: debug-static assertion at `SourceTextModuleRecord.cpp:542` /
release SIGSEGV while throwing; D2: `SHARED-RECORD`; D3:
`SWAPPED-ORDER: DIFFERENT-RECORDS`); each new regression test below
was also run against the pre-fix binary and fails there.

- D1 parser (`lib/Parser/Parse.cpp`, `lib/Parser/Parse.h`): star
  export entries (`export *` / `export * as ns ... with {...}`)
  assigned `attributes` through the pointer returned by the by-value
  `AddModuleImportOrExportEntry` overload, but the list stores a copy
  (`SList::Prepend` copies the struct), so the stored entry kept
  `nullptr` and resolution missed. Fixed by threading `attributes`
  through a new defaulted parameter set before the copy; the
  post-hoc assignment is gone. Named re-exports and imports already
  attached attributes before the copy and are unchanged.
- D2 host (`bin/ch/WScriptJsrt.cpp`, `ReadModuleAttributes`): the
  escape set missed `=`, so `{"a=b":"c"}` and `{a:"b=c"}`
  canonicalized identically. `=` is now backslash-escaped alongside
  the entry separator, backslash, and newline. No existing key
  changes (no prior corpus key contains those bytes unescaped).
- D3 engine (`lib/Runtime/Language/JavascriptOperators.cpp`,
  `TryGetImportAttributes`): dynamic keys are now inserted into the
  attributes object in sorted UTF-16 code-unit order (insertion sort
  over a leaf `PropertyId` array) to honor the `ChakraCore.h`
  "sorted key order" contract and match the parse-time-sorted static
  path. Getters still run once in enumeration order (values staged
  in a temp Recycler object first, so later getters cannot collect
  buffered values); an empty `with` now maps to `nullptr` like the
  static `with {}` (same request as no attributes). Also corrected
  the stale `BuildModuleAttributesObject` header comment (returns
  `nullptr`, not "always an object", when there are no attributes).
- Regression tests (existing files only; scenario counts unchanged):
  `module-syntax.js` gains attributed `export *` + consumer shapes
  (plain star, bare consumer, named re-export);
  `export_namespace_as.js` gains `export * as ns ... with` consumer
  shapes; `dynamic-module-functionality.js` gains a "Dynamic import
  attribute identity" case (`=`-escape split, order-insensitivity,
  static/dynamic agreement, empty-`with` equivalence). New unlisted
  fixtures `importattrs-counter/star/named/static/star-as.js` (not in
  `rlexe.xml`, so no new scenarios).
- Gates: `mise x -- zig build check` green incl. `Total: passed
  2012, failed 0`; `check-regression -Dprofile=debug-static`
  `Total: passed 2064, failed 0`; `git diff --check` clean;
  focused suites pass in normal / `-forcedeferparse` /
  `-nodeferparse` on both binaries. test262
  `import/** + module-code/**`: 565/172 with per-scenario results
  identical to the lane run (0 pass-to-fail, 0 fail-to-pass);
  `dynamic-import/import-attributes/**` holds at 17/27 (remaining
  failures are the pre-existing host CWD-relative fixture
  URIError). Full debug-static `check` metadata leg reports
  `libChakraCoreStatic.a: 526 members, require 524`; reproduced at
  the unmodified lane tip (stash + rebuild), so it predates this fix
  round and is unrelated (no translation unit added or removed).

## Fix round (loop-3 defects D4, D-in)

Both defects reproduced on the lane binary before fixing (D4:
`DIFFERENT-RECORDS` on the exact static-vs-dynamic scenario the new
test encodes; D-in: `SyntaxError: Expected ')'` for `in` in either
`import()` argument, same shapes the new eval assertions encode); the
committed regression tests fail on those pre-fix behaviors.

- D4 parser (`lib/Parser/Parse.cpp`, `CompareModuleAttributeKeys`):
  the static comparator used `memcmp` over UTF-16 bytes, which on
  little-endian targets misorders units >= U+0100 against lower units
  (U+0100 sorted before U+00FF), while the dynamic path
  (`TryGetImportAttributes`) sorts by UTF-16 code unit — so the loop-1
  claim that the dynamic order "matches the parse-time-sorted static
  path" held only for ASCII/Latin-1 keys. Replaced with a `char16`
  code-unit loop; behavior-preserving for ASCII/Latin-1. Regression
  test: static vs dynamic non-ASCII attribute sets now share one
  module record (`dynamic-module-functionality.js` +
  `importattrs-nonascii.js` fixture).
- D-in parser (`lib/Parser/Parse.cpp`, `ParseImportCall`): both
  `import()` arguments were parsed with `fAllowIn` FALSE, wrongly
  rejecting the grammar's `AssignmentExpression[+In]`
  (`import('a' in {} ? 'b' : 'c')` and
  `import('m.js', 'a' in {} ? {} : null)` failed to parse). Flipped
  both to TRUE; the single `<bool buildAST>` template covers full
  parse and fast scan, so no grammar ambiguity is introduced in
  either mode. Regression coverage for both argument positions
  (`dynamic-module-import-specifier.js`).
- Nits: `ModuleMessage::WithAttributes` corrected to the real factory
  name `ModuleMessage::CreateWithAttributes`; attribute keys recorded
  as stored `IdentPtr`s; `JsGetOwnPropertyNames` recorded as serving
  the `ch` host `ReadModuleAttributes` enumeration. The stale
  "always returns an object" wording on `BuildModuleAttributesObject`
  was already corrected in the loop-1 round (header documents the
  `nullptr` return); no change needed.
- Gates: `mise x -- zig build check` PASS and `check-regression
  -Dprofile=debug-static` at `tools/build/gate_counts.json`
  (2012/2064); import-attributes corpus subset + both defect repros
  re-run (deltas recorded by the round driver).
