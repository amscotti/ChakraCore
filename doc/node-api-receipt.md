# Node-API provider — N5.4/N5.5 receipts (stress, packaging, exports gate)

> §9 is the N5.4 stress/teardown campaign (owner: N5.4). §§1–8 are
> the N5.5 packaging slice (owner: N5.5).

Owner: N5.5. Status: **G5 packaging slice green on macOS arm64;
Linux x64 WIRED (2026-11-13, `tools/build/linux.zig`), NOT RUN** (see §12). This receipt satisfies the PR-1
wording gate (PR-1 in `doc/node-api-plan.md` §2.2): the only public
claim is the engine surface plus Table B under the §3.2 host
contract. It never claims a Node runtime.

Build lock: every `zig` invocation below went through the serialized
gate lock — `/tmp/opencode/cc-gate-lock.sh mise exec -- zig …`
(probe runners wrap their own `zig c++` calls the same way). Do not
run bare `zig build` / `zig cc` alongside a gate-locked build.

Toolchain (recorded 2026-11-12, macOS arm64 host):

- Zig `0.17.0` via mise (`mise.toml` pin).
- Target `aarch64-macos.26.0` (`apple_m1` CPU model), MacOSX SDK `26.5`
  via `xcrun --show-sdk-path`, system ICU `/opt/homebrew/opt/icu4c/include`.
- Vendored headers: Node `22.22.3` (`include/nodeapi/`,
  `include/nodeapi/MANIFEST.md`), provider and addons built with
  `-DNAPI_VERSION=8` explicitly (PR-2). `NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED`
  is never defined, so `napi_create_external_arraybuffer` and
  `napi_create_external_buffer` are in the surface.
- Provider: `libChakraNodeApi` (plan PR-3), shared profiles only
  (`test`, `jit`; `release-shared` also carries the library; `debug-static`
  has no provider step by design). Private host API:
  `lib/NodeApi/NodeApiHostApi.h` §3.2 (create/destroy env) plus the
  N4.1 pending-module helpers in `lib/NodeApi/NodeApiHost.h`.

---

## 1. Exports gate (plan N1.6 / N5.5, HIGH-5)

The allowlist is generated, not hand-maintained:

- Normative inventory: `doc/node-api-inventory.md`, parsed by
  `tools/build/gen_nodeapi_exports.py` — exactly **145** symbols at
  `NAPI_VERSION=8` (115 `js_native_api.h` + 30 `node_api.h`; generator
  exits 1 on any other count).
- `tools/build/gen_nodeapi_exports.py` `WAVE1_LANDED` now holds all
  **145** inventory symbols (the name is historical; the list is the
  full 145, Waves 1–4). Verified: `inventory 145`, `WAVE1_LANDED 145
  unique`, `landed − inventory = []`, `inventory − landed = []`.
- Host entries (`HOST_API`, 5): `chakra_nodeapi_create_env`,
  `chakra_nodeapi_destroy_env` (plan §3.2 core) plus
  `chakra_nodeapi_get_pending_module`,
  `chakra_nodeapi_pending_module_count`,
  `chakra_nodeapi_run_pending_registrations` (N4.1 §3.2 extension).
  The older "full 145 + 2" wording is stale: the final surface is
  **145 napi_* + 5 host = 150** exported symbols, not 147. The
  generator/checker comment headers were updated to say so (no list
  change: `nodeapi_exports.txt` regenerates byte-identical apart from
  its header comment, still 150 symbols).

Commands (gate-locked, run 2026-11-12):

```sh
python3 tools/build/gen_nodeapi_exports.py --check
# inventory: 145 symbols (expected 145)
# exports: 150 symbols (145 napi + 5 host)
# check ok: nodeapi_exports.txt matches generation

python3 tools/build/check_nodeapi.py --self-test
# 7 controls PASS (exact match, missing, extra, runtime filter,
# garbage victim, parser, inventory-145)

# Live tamper controls against the built dylib (expect FAIL):
python3 tools/build/check_nodeapi.py --dylib zig-out/lib/libChakraNodeApi.dylib \
  --exports <allowlist without napi_get_version> --inventory doc/node-api-inventory.md
# FAIL: extra exports (in dylib, not in allowlist): napi_get_version  (rc=1)

python3 tools/build/check_nodeapi.py --dylib zig-out/lib/libChakraNodeApi.dylib \
  --exports <allowlist + napi_fake_missing>  --inventory <inventory + fake row>
# FAIL: missing exports (in allowlist, absent from dylib): napi_fake_missing  (rc=1)

python3 tools/build/check_nodeapi.py --dylib zig-out/lib/libChakraNodeApi.dylib \
  --exports <allowlist + napi_fake_extra / node_api_symbol_for> --inventory doc/node-api-inventory.md
# FAIL: allowlist symbols outside inventory+host  (rc=1)
# (node_api_symbol_for is v9-only/excluded and never enters the allowlist)
```

Built-library check (both shared profiles):

```sh
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build libChakraNodeApi
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build check-nodeapi
# inventory: 145 / exports: 150 / check ok
# check_nodeapi: exports .../libChakraNodeApi.1.0.0.dylib -> 162 T symbols
#   (150 provider + 12 runtime operator new/delete), allowlist 150
# check_nodeapi: PASS: dylib exports exactly the allowlist (150 symbols)

/tmp/opencode/cc-gate-lock.sh mise exec -- zig build libChakraNodeApi -Dprofile=jit
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build check-nodeapi -Dprofile=jit
# same 162 (150 + 12), PASS
```

The 12 runtime `T` symbols are Zig's static-libc++ replaceable
`operator new/delete` family (`__Zdl/__Zda/__Zn/__Zna`), filtered by
`check_nodeapi.py` (`RUNTIME_T_RE`) and reported separately. Every
other `__Z*` (provider internals) would fail as extra. Visibility:
provider builds with `-fvisibility=hidden`; the 145 `napi_*` export
via `NAPI_EXTERN` default visibility, the 5 host entries carry an
explicit `__attribute__((visibility("default")))` on their
definitions (`lib/NodeApi/NodeApiEnv.cpp:485,582`,
`lib/NodeApi/NodeApiHost.cpp:91,99,121`).

Stubs: there are **no unconditional `napi_generic_failure` stubs**.
Every one of the 145 exports a real implementation; the only
`generic_failure` returns are conditional (OOM / stale / invalid
state / host-less), and each such symbol still exports (proven by
the `nm` match above). The conditional `generic_failure` paths are
the documented non-conformances in §4 below, each covered by probes.

Why `check-nodeapi` stays a separate step (not wired into `check`,
PR-10): the provider exists only on shared profiles
(`prov_lib` is null on `debug-static`), and its gate couples engine
verification to provider surface. Wiring it into the shared `check`
tops would add provider failure surface to the regression gate
without moving its `gate_counts.json` pins. The gate is therefore
run explicitly for `test` and `jit` (and exists for `release-shared`
too); the existing `check` aggregation is unchanged. `build.zig`
documents this at the `check-nodeapi` step.

---

## 2. Header install (N5.5)

`zig build install` (test profile) stages engine headers
(`include/ChakraCore.h`, `ChakraCommon.h`, `ChakraDebug.h`,
`ChakraCoreVersion.h`) but, before this slice, never staged the
vendored N-API headers or the provider dylib. N5.5 adds, in
`build.zig`:

- Test (`zig build libChakraNodeApi`): installs
  `libChakraNodeApi.dylib` to `<prefix>/lib/` **and** the four
  vendored headers to `<prefix>/include/nodeapi/`
  (`js_native_api.h`, `js_native_api_types.h`, `node_api.h`,
  `node_api_types.h`). The private host header
  `lib/NodeApi/NodeApiHostApi.h` is intentionally **not** installed.
- Non-test (`jit`, `release-shared`; `zig build libChakraNodeApi`
  or `zig build install`): `copy_out` now also maps the four
  headers to `out/<profile>/include/nodeapi/`.

Verified 2026-11-12: `zig-out/include/nodeapi/` and
`out/jit/include/nodeapi/` each contain the four headers,
byte-identical to `include/nodeapi/` (`cmp` identical, all four).
Providers in `zig-out/lib/libChakraNodeApi.dylib` (test) and
`out/jit/lib/libChakraNodeApi.dylib` (jit) are byte-identical
675K dylibs (same provider TUs; `nm` counts match, §1).

Consumer include line (from-scratch proof, §3):

```sh
-I<installed>/include/nodeapi -I<installed>/include
```

i.e. `-Izig-out/include/nodeapi -Izig-out/include` (test) or the
`out/jit` equivalents. `#include <node_api.h>` resolves from the
installed `nodeapi/` subdir; Chakra headers, if needed, from the
installed `include/` dir.

---

## 3. From-scratch consumer build proof (no Node executable)

TEMP dir outside the repo (generated files never land in the
source tree, plan §7). Provider + host were built first
(gate-locked `zig build libChakraNodeApi`, `zig build ch-nodeapi`,
test profile). Host smoke before the proof:

```sh
./zig-out/bin/ch-nodeapi --provider zig-out/lib/libChakraNodeApi.dylib \
  --addon zig-out/lib/nodeapi-addons/hello_world.node \
  --driver zig-out/lib/nodeapi-addons/hello_world.js
# hello: world  (rc=0)
```

Consumer addon (`consumer.c`, C, `NAPI_MODULE(consumer, Init)`,
exports `hello()` → `"consumer"` and `answer()` → `42`; full source
is reproduced by the `cat` heredoc in the session log, kept only in
the TEMP dir — NOT committed to the repo). `driver.js`:

```js
print("consumer hello: " + globalThis.__exports.hello());
print("consumer answer: " + globalThis.__exports.answer());
```

Build (run `cd "$WORK"` in the TEMP dir, absolute installed paths;
`-L` under `--sysroot` resolves against the SDK, so the provider is
passed by direct dylib path — the same shape the `ch-nodeapi` host
step uses for the engine dylib):

```sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/nodeapi-consumer.XXXXXX")  # /private/var/folders/.../T/, outside repo
SDK=$(xcrun --show-sdk-path)
PREFIX="$PWD/zig-out"  # repo test prefix (after the builds above)
cd "$WORK"
/tmp/opencode/cc-gate-lock.sh mise exec -- zig cc \
  -target aarch64-macos.26.0 --sysroot "$SDK" \
  -std=c99 -O2 -g -fPIC -shared -DNAPI_VERSION=8 \
  -I "$PREFIX/include/nodeapi" -I "$PREFIX/include" \
  -o consumer.node consumer.c \
  "$PREFIX/lib/libChakraNodeApi.dylib" \
  -Wl,-rpath,"$PREFIX/lib" -mcpu=apple_m1 -Wno-date-time
# rc=0
```

Artifact evidence:

```sh
otool -L consumer.node
#   @rpath/libChakraNodeApi.dylib (compatibility version 1.0.0, ...)
#   /usr/lib/libSystem.B.dylib
nm -u consumer.node
#   _napi_create_double _napi_create_function _napi_create_string_utf8
#   _napi_set_named_property  (+ dyld stub binder; defined: napi_register_module_v1,
#   node_api_module_get_api_version_v1)
```

Run (ch-nodeapi only — no Node binary invoked anywhere in the
proof; only `zig cc` + the installed headers/lib + `ch-nodeapi`):

```sh
"$PREFIX/bin/ch-nodeapi" \
  --provider "$PREFIX/lib/libChakraNodeApi.dylib" \
  --addon "$WORK/consumer.node" --driver "$WORK/driver.js"
# consumer hello: consumer
# consumer answer: 42
# rc=0
```

This proves a consumer that has never seen the repo source tree
(install headers + built provider only) compiles with `zig cc`,
links the built `libChakraNodeApi`, and loads/runs through
`ch-nodeapi` (provider `RTLD_GLOBAL` → version check → registration
scope → `napi_register_module_v1` → driver + host pump).

---

## 4. PR-1 claim (what IS supported)

**The provider implements the Node-API v8 engine surface
(`js_native_api.h`) plus the plan §4 Table B functions under the
§3.2 host contract.**

Never claim Node runtime compatibility. In particular the provider
does **not** provide `require`/CommonJS resolution, `process`, Node
built-ins, timers, a `node` executable, or npm. Per plan §0 the
runtime/host split is: Table A is real behavior; Table B behaviors
are real exactly where the host injects the §3.2 services, and
`napi_generic_failure` (with a recorded last error) where they
do not (D7 — never fake success).

Table A (`js_native_api.h`, 115 at v8): value creation/getters,
coercion/type checks, symbols, Date and BigInt (string-mediated
per PR-9, §4.3 below), objects/arrays/properties/descriptors,
functions/callbacks, errors, binary (ArrayBuffer/TypedArray/DataView),
promises, script, version, external memory, scopes/refs/wrap/tags/
instance data. All 115 export and are probed (probe tallies in §5).

Table B (`node_api.h`, 30 at v8) under the §3.2 host contract:
registration/fatal (pending-module registry, fatal routing),
Buffer (PR-6 marker design, §4), async context/callback scopes
(bookkeeping + `make_callback`), async work + uv (services-gated),
thread-safe functions (services-gated), cleanup hooks (sync in the
lifetime kernel; async completion via host pump), `napi_get_node_version`
(provider version struct, NOT a Node runtime version). The `ch-nodeapi`
test host injects **no** scheduling services / no uv loop
(`services = {}`), so async-work/TSFN/uv fail correctly there; probes
with injected fake services pass those paths (§5).

Test host: `tools/nodeapi/ch-nodeapi` (provider `RTLD_GLOBAL`,
addon `RTLD_LOCAL`, version > 8 rejected, fresh exports object,
return-replaces-exports, `__exports` global, host `print`,
bounded promise pump — PR-8 provider installs the continuation
callback only if the host has not; the host owns pumping).

---

## 5. Known non-conformances (all probed; NOT "0 failures with
unexplained skips")

- **No JS `Buffer` global (PR-6).** `napi_create_buffer*` produce
  `Uint8Array` views marked Buffer via a provider-private per-env
  Symbol; `napi_is_buffer` is the marker check. `typeof Buffer ===
  'undefined'` and `instanceof Buffer` throws `ReferenceError` on
  the provider. No pooling, no `Buffer.prototype` methods. Addons
  needing `require('buffer')` are runtime scope (§0 C). Probed:
  `run_buffers_probe.sh` (102/102 per profile, incl. the two
  `ReferenceError` checks).
- **BigInt64/BigUint64 typed arrays → `napi_generic_failure`.**
  `napi_create_typedarray` with those types has no `JsTypedArrayType`
  counterpart and returns `napi_generic_failure` with the last error
  `create_typedarray: BigInt typed arrays unsupported`
  (`lib/NodeApi/NodeApiBinary.cpp:695-699`). A JS-created
  `Float16Array` similarly has no `napi` counterpart
  (`get_typedarray_info` → `generic_failure`). Probed:
  `run_binary_probe.sh` (226/226 per profile).
- **Async without host services → `napi_generic_failure`.**
  `napi_create_async_work`, `napi_queue/cancel/delete_async_work`,
  `napi_get_uv_event_loop`, `napi_create_threadsafe_function`,
  `napi_ref/unref_threadsafe_function` return
  `napi_generic_failure` ("… no host scheduling services") when the
  §3.2 `post_task`/`post_work` pair is absent; `napi_get_uv_event_loop`
  additionally needs an injected loop. `napi_call/acquire/release/
  get_context` on stale handles also return `generic_failure`
  (Node post-finalize status). Probed: `run_async_probe.sh` 280/280
  (incl. the no-services matrix) with services, correctly failing
  without them.
- **async_hooks propagation is a bookkeeping no-op.** `napi_async_init/
  destroy` record pairing/counts only; `napi_make_callback` runs the
  callback without AsyncLocalStorage propagation (documented
  runtime scope). Probed: `run_asyncctx_probe.sh` 203/203.
- **Boxed-BigInt `typeof`: FIXED (not a remaining gap).**
  `napi_typeof` discriminates via `Object.prototype.toString` tag
  plus `JsConvertValueToObject` identity (PR-9 MED-1 fix,
  `lib/NodeApi/NodeApiValues.cpp:247-324`): primitive BigInt →
  `napi_bigint`, boxed `Object(1n)` → `napi_object` (tag alone is
  ambiguous, identity splits it); bigint getters on a boxed value
  return `napi_bigint_expected`. Probed: `run_coerce_probe.sh`
  430/430 includes the boxed table and the three boxed-getter
  rejections. Earlier "documented divergence" wording in that
  probe is now the fixed expectation.
- **v9/v10 symbols absent by design (D1).** The host rejects addons
  requesting `NAPI > 8` before registration; v9/v10/experimental
  (`node_api_symbol_for`, syntax-error pair,
  `node_api_get_module_file_name`, external-string/property-key
  creators, `node_api_create_buffer_from_arraybuffer`, SAB pair,
  `node_api_post_finalizer`) never enter the inventory or the
  allowlist (generator parses the Inventory section only; adding a
  v9 symbol to the allowlist fails the inventory gate, §1).
- **`napi_get_node_version` returns the provider version**, not a
  Node runtime version (B7). **`napi_run_script`** is a single
  `JsRun` (global scope, not module scope) per N3.5. Node error
  *text* is explicitly not conformance (PR-12) — status and pending
  semantics are.

---

## 6. Per-platform evidence status

**macOS arm64 (this host): PROBED.** Probe runners (each builds the
probe + provider TUs against the profile engine dylib and runs both
profiles; `SKIP_BUILD=1` reuses the already-built engines):

```sh
SKIP_BUILD=1 sh test/nodeapi/probes/run_<name>_probe.sh
```

Tallies (checks per profile; `test` and `jit` identical where PASS):

| Probe | Checks/profile | test | jit (runner) |
|---|---|---|---|
| async (N4.4) | 280 | PASS | PASS |
| asyncctx (N4.3) | 203 | PASS | PASS |
| binary (N3.1) | 226 | PASS | PASS |
| buffers (N4.2) | 102 | PASS | PASS |
| coerce (N2.6 incl. boxed-BigInt) | 434 | PASS | PASS |
| errors (N2.5) | 179 | PASS | see below |
| functions (N2.4) | 187 | PASS | PASS |
| gate (N1.5 pump/closing) | 51 | PASS | PASS |
| host (N4.1) | 88 | PASS | see below |
| objects (N2.3) | 341 | PASS | PASS |
| promise_script (N3.4/N3.5) | 195 | PASS | PASS |
| refs (N1.3) | 211 | PASS | PASS |
| values (N2.1/N2.2) | 407 | PASS | PASS |
| wrap (N3.2/N3.3) | 211 | PASS | PASS |

Total: 3117 checks per profile (6234 across both) when jit is run
isolated (see runner note). Zero probe failures in the checks
themselves.

Runner note (pre-existing, NOT an N5.5 regression): `run_errors_probe.sh`
and `run_host_probe.sh` use a shared `WORK/libChakraCore.dylib` for
both profiles; the second (`jit`) overwrite of the ad-hoc-signed
dylib invalidates its code signature and `dyld` kills the binary
(`EXC_BAD_ACCESS SIGKILL (Code Signature Invalid)`, ad-hoc
`linker-signed` engines). The `jit` leg therefore reports
`main probe failed (jit)` under the runner. In isolated dirs the
same binaries pass (`errors_probe` 179/179, `host_probe` 88/88 on
the `jit` engine — verified 2026-11-12). Crash reports for this
runner shape predate N5.5 (Nov 11). The fix belongs to the probe
runners (separate WORK dirs per profile), not to this packaging
slice, and is left as a follow-up. Test-profile probes are
unaffected and all green.

Not run on this host (not claimed): `scopes_probe`, `teardown_probe`,
`env_addr_reuse_probe` have no `run_*.sh` runner (legacy
self-contained drivers superseded by the runner probes above) and
were not executed. Vendored `test/js-native-api` / examples through
`ch-nodeapi` (`test/nodeapi/run_matrix.py --host ch-nodeapi`) is
N5.1 scope and was **not** run here (the runner invokes `zig`
directly, outside the gate lock; the checked-in
`test/nodeapi/results/results.json` remains the 2-entry
`node-direct` differential record, not a provider pass). Do not read
that file as provider evidence.

**Linux x64: WIRED, NOT RUN** (provider wired in `tools/build/linux.zig` on 2026-11-13; see §12. At landing it was NOT WIRED.)
Verbatim status (a Mac arm64 run can never stand in for Linux, plan
§7 rule 6):

> Linux x64 provider build was not attempted from this host. The
> Linux graph (`tools/build/linux.zig`, `x86_64-linux-gnu`, test
> profile only) has no Node-API provider step, no `ch-nodeapi`
> step, no `check-nodeapi` step, and no vendored-header install.
> `linux.zig` was deliberately left unmodified in N5.5: it only
> configures on an `x86_64` Linux host (`build.zig` dispatches to it
> solely when `builtin.os.tag == .linux`), so its Zig syntax/graph
> cannot be validated or executed here. Wiring it without a Linux
> run would risk breaking `zig build check` on Linux for an
> unverified step.

Precisely what a Linux-host lane must add (mirroring the Mac steps
in `build.zig`):

1. Compile the 16 provider TUs (`lib/NodeApi/*.cpp`: Env, Scopes,
   Lifetime, Refs, Values, ErrorsImpl, Objects, Functions, Binary,
   Promise, Script, Wrap, Buffers, Async, AsyncContext, Host) with
   the Linux graph's `zig c++` CLI flags (`-target
   x86_64-linux-gnu`, `-O3 -g -std=gnu++17 -fPIC
   -fvisibility=hidden -DNAPI_VERSION=8 -Ilib/Jsrt
   -Iinclude/nodeapi -Ilib/NodeApi -isystem <icu>`), NO SDK/
   `-F`/framework/rpath-Mac `-mcpu` flags. Provider sources contain
   no `__APPLE__`/macOS-only code (`grep` clean), so NO provider
   source change is expected; the flags are the work.
2. Link `libChakraNodeApi.so` (`zig c++ -shared` against the built
   `libChakraCore.so`; no direct ICU edge, as on Mac).
3. Install to `lib/libChakraNodeApi.so` plus the four headers to
   `include/nodeapi/` (mirroring §2). `check_nodeapi.py` already has
   the ELF `nm -g --defined-only` fallback (no leading underscore).
4. Add `libChakraNodeApi` + `check-nodeapi` steps, kept separate
   from `check` for the same PR-10 reason as Mac (shared-only step,
   pins untouched). `ch-nodeapi` needs a Linux host step too
   (`tools/nodeapi/ch-nodeapi.cpp` already tries
   `libChakraNodeApi.so` / `libChakraNodeApiStub.so`; it links the
   engine `.so` and needs `-ldl` and ELF `$ORIGIN` rpaths instead
   of `@executable_path`/frameworks).
5. Then run, on Linux: provider build, `check-nodeapi`, consumer
   proof, all 14 probe runners, and `zig build check` (Linux
   `check` aggregation in `linux.zig:668-679`) — their tallies are
   the Linux receipt.

---

## 7. Regression pins (existing gates, plan §7 rule vs gate_counts)

`tools/build/gate_counts.json` was NOT modified by N5.5 (no engine
source changed; N5.5 edits are provider packaging + comments +
receipt/README only). Full existing gate (gate-locked, test profile):

```sh
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build check
# check_metadata: PASS (minos 26.0, 8MiB stacks, 4 headers)
# check_regression: exit=0 parsed=(2053, 0) require passed 2053 / failed 0
#   Total: passed 2053, failed 0
# verify_bytecode_pure: source headers unchanged; regen exit 0
# check_native: PASS (test-shared-basic : PASS, 8 static suites ignored)
# check_smoke + check-selftest + Linux gate scripts self-tests: ALL BEHAVE
```

Tally: **2053 passed / 0 failed**, exactly the `test` pin
(`2053/0`). The `debug` pin (`2105/0`, `debug-static` profile) was
not re-run in this slice (no `debug-static` build in N5.5); the
file is unchanged, so the pin is untouched, but that profile's
green is not claimed from this receipt.

---

## 8. Files changed in this slice (no commit)

- `tools/build/gen_nodeapi_exports.py` — full-coverage wording
  (145 + 5 = 150).
- `tools/build/check_nodeapi.py` — same wording (no logic change).
- `tools/build/nodeapi_exports.txt` — regenerated (150 symbols;
  header comment only).
- `build.zig` — vendored-header install (`libChakraNodeApi` +
  `copy_out`) and `check-nodeapi` wording/rationale. No engine
  source, no table, no `check` wiring change.
- `doc/node-api-receipt.md` — this file (new).
- `README.md` — PR-1 provider section (see §4 wording).

NOT changed (by design): `doc/node-api-plan.md`, `doc/node-api-inventory.md`,
`doc/node-api-semantics.md`, `lib/`, `tools/build/linux.zig`,
`tools/build/gate_counts.json`, `test/nodeapi/` runners.

---

## 9. Stress and teardown campaign (plan N5.4, G3 gate validation)

Owner: N5.4. Ran on macOS arm64 (host of §6), gate-locked, 2026-11-12.
**Linux x64 NOT RUN** (same standing limitation as §6).

### 9.1 What ran

Runner: `test/nodeapi/stress/run_stress.sh` (builds each probe with
the full reconciled provider TU set against **both** profile engines,
then runs each binary **10 process-level repeats per profile**; per
repeat each probe also does its own **12 internal repeats** (≥10 plan
minimum)).

```sh
sh test/nodeapi/stress/run_stress.sh
# run_stress: ALL PASS (test + jit, 10 repeats each)
```

| Probe (in `test/nodeapi/stress/`) | Profile | Repeats | Result |
|---|---|---|---|
| `gc_churn_stress.cpp` | test | 10 runs × 12 internal | PASS (78/78) |
| `gc_churn_stress.cpp` | jit | 10 runs × 12 internal | PASS |
| `env_churn_stress.cpp` | test | 10 runs × 1200 cycles | PASS (1208/1208) |
| `env_churn_stress.cpp` | jit | 10 runs × 1200 cycles | PASS |
| `tsfn_teardown_stress.cpp` | test | 10 runs × 12 internal | PASS (252 checks, 0 fail) |
| `tsfn_teardown_stress.cpp` | jit | 10 runs × 12 internal | PASS (252 checks, 0 fail) |
| `gc_hook_addon` + `gc_hook_driver.js` via `ch-nodeapi` | test | 10 runs | PASS (`final=240`) |

### 9.2 GC churn — exact counters (`gc_churn_stress.cpp`)

Shared env for externals/wraps/refs (a leak accumulates across the 12
repeats); fresh env per deferred repeat (settled deferreds are
retained as tombstones until destroy by design, so per-repeat fresh
envs are the only way the count returns to 0). Each repeat: create
batch → drop (optnone frame + `ClearStack`) → `JsCollectGarbage` ×10
(the `-CollectGarbage` equivalent) → assert 0 finalizers during GC,
queued count == batch → drain → assert finalizer count == batch
exactly, live/queue counts 0, `chakra_nodeapi_test_root_count` back
to baseline.

- Externals: 12 × 50 = **600 finalized, exact**; wraps: 12 × 50 =
  **600 finalized, exact**; refs per repeat: 30 strong (survive GC,
  get non-null) + 20 weak (collect, get null), root count baseline+50
  mid-repeat and baseline after every repeat.
- Deferreds: 12 envs × 20 = **240 settled exact** (10 resolve + 10
  reject per env), all `.then` handlers ran exactly once after the
  post-settle GC burn, queue depth 0, root baseline held, destroyed
  envs report `DeferredCount == 0` (no dereference of dead env).
- **Leak found: NONE. Drift: 0** (every per-repeat and total count
  exact on every run, both profiles).

### 9.3 Env churn — exact counters (`env_churn_stress.cpp`)

1200 cycles (plan: ≥1000; extends N1.1 P1-6). Each cycle: create env
(generations strictly monotonic, nonzero) with live wrap + strong ref
+ droppable wrap/external (GC-collected to *pending*, undrained) +
unsettled deferred + sync hook + instance data; GC burn **last**
(`EntryGuard` drains the queue on entry to JS-calling APIs, §3.7, so
any napi call between the burn and destroy would drain the pendings
— the exact condition the probe must exercise); destroy; assert every
teardown step ran exactly once.

Per run (observed identical on all 10 runs, both profiles):

```text
churn: cycles=1200 reuse_hits=1197 stale_checks=1199
wrapdrop=1200 extdrop=1200 livewrap=1200 instance=1200 sync=1200
max_dead=3
ENV_CHURN_STRESS: PASS (1208/1208)
```

Address reuse was observed in 1197/1199 transitions (allocator
reused the freed `NodeEnv` slot; informational, not asserted) and
the stale-generation lookup failed without touching the new env in
**1199/1199** transitions (new env still `napi_get_version == 8`).
Tombstones (dead-env registry) peaked at **3** (cap 256; sequential
churn displaces by reuse, never grows). **Leak found: NONE** (all
five teardown totals exact; 0 drift).

### 9.4 TSFN multi-producer teardown races (`tsfn_teardown_stress.cpp`)

N4.4 landed; per repeat: fresh env, call_js_cb TSFN + held work,
four producer threads calling/enqueueing (plus acquire/release churn)
while the env thread destroys the env, then join + pump post-destroy
to quiet. Observed per full run (12 repeats × 10 runs; counts vary
only with producer rate):

- `ok` ≈ 2.3–2.4M calls delivered pre-abort, `delivered == 0` every
  repeat (the destroy aborts the queue: **no user TSFN callback after
  destroy**, no work `complete` after destroy, including held-work
  released post-destroy).
- `thread_finalize_cb` **exactly once** per repeat, on the env thread,
  during destroy; **0 post-destroy `napi_ok`** (producers see only
  `generic_failure`-class statuses after destroy returns); zero JS
  errors.
- `NodeApiAsync_WorkCount / TsfnCount / LoopRefCount == 0` on dead
  envs (by-value lookup, no dereference); host `ref/unref` **balanced**;
  root counter dead value `-1` (no dereference).
- **Leak/races found: NONE.** (The async S1 4-producer delivery
  campaign remains the delivery-correctness oracle; this probe is the
  teardown-race oracle.)

### 9.5 Sanitizers: NOT RUN (harness has no TSAN/ASan profile)

`build.zig` defines only `test` / `debug-static` / `release-shared` /
`jit`; `debug-static` is explicitly `-fno-sanitize=undefined` and the
probe builds use plain `-O3 -g`. There is no `-fsanitize=thread` or
`-fsanitize=address` leg to run, and per `doc/memory-management.md`
§8 a Debug/ReleaseSafe build is not a sanitizer build — **no sanitizer
coverage is claimed**. The available race evidence is: 12 internal
repeats × 10 process runs × 2 profiles with atomic producer/host
discipline, exact counters, and 0 lost/duplicated deliveries in S1.
A TSAN/ASan leg, if ever added, must be qualified with an isolated
known-invalid control per that section.

### 9.6 Host `gc()` hook (test-host only, added this slice)

The harness had no `gc()` hook (N0.5 skeleton; `shims/gc.js` threw
`needs-gc`). Added the smallest hook to `tools/nodeapi/ch-nodeapi.cpp`
(test-host only, never provider):

- `globalThis.gc` calls `JsCollectGarbage` on the host runtime
  (synchronous full GC; arguments ignored; `gcOptions` unmapped).
  `callbackState` borrows the host runtime (process lifetime).
- `test/nodeapi/shims/gc.js` + `test/nodeapi/run_matrix.py` comments
  updated: vendored GC tests remain `needs-gc` skips pending N5.1
  adoption (they need async_hooks/timers semantics, not just `gc()`).
- Proof: `gc_hook_addon.cpp` (external counter) + `gc_hook_driver.js`
  (12 × 20) — **PASS `final=240` on all 10 runs**, run in the campaign
  (§9.1 table). Two probe-visible semantics recorded during bring-up:
  (a) finalizers drain on a *later* JS-calling entry, so the addon
  `count()` self-drains (`napi_check_object_type_tag`, which drains on
  entry) before reading; (b) script-scope `stash = null` does not
  drop an array in this engine (survives 10 GCs) while a function-frame
  drop collects immediately — the driver uses the fn-scope pattern.

### 9.7 Root-balance summary

`chakra_nodeapi_test_root_count` (scope + ref-record total,
R-REF-14) was asserted at baseline after **every** repeat/cycle in
all three probes, plus the design counts (wrap live/queue,
deferred, async work/TSFN/loop-ref) at exact values. No repeat or
cycle ended above baseline in any run: **0 root leaks observed.**

### 9.8 Files in this slice (no commit)

- `test/nodeapi/stress/gc_churn_stress.cpp` (new), `env_churn_stress.cpp`
  (new), `tsfn_teardown_stress.cpp` (new), `run_stress.sh` (new),
  `gc_hook_addon.cpp` + `gc_hook_driver.js` (new).
- `tools/nodeapi/ch-nodeapi.cpp` (host gc hook + install).
- `test/nodeapi/shims/gc.js`, `test/nodeapi/run_matrix.py`
  (gc-hook comments only; skip taxonomy unchanged).
- `doc/node-api-receipt.md` (this section only; §§1–8 untouched).

NOT changed (by design): `doc/node-api-plan.md` (N5.4 is marked plan
only; the owner updates the status tracker with gate evidence),
`doc/node-api-semantics.md`, `lib/` (no provider source change was
needed — all stress failures found were probe bugs, fixed in the
probes; see §9.9), `tools/build/gate_counts.json`.

### 9.9 Probe bugs found during bring-up (all fixed in probes, 0 provider changes)

1. Env-churn instance counter read the user payload as the counter
   struct (instance finalizer total 0) — probe bug; finalizer now
   uses the global counter (teardown-probe pattern).
2. Env-churn expected 3 live wraps before destroy but saw 1:
   `napi_wrap` (and any JS-calling entry) drains the queue on entry
   (`EntryGuard`, §3.7), so the post-GC `napi_wrap` drained the
   pending pair. Fixed by GC-burn-last ordering (destroy is the first
   call after the burn).
3. TSFN race hung: pumping while producers run chases an unbounded
   queue forever. Fixed by removing the pre-destroy pump (S1 joins
   before pumping).
4. GC-hook driver read 0: the array-stash/`= null` drop pattern does
   not collect (above) — fixed with the fn-scope pattern + addon
   self-drain.

---

## 10. Vendored conformance — N5.1 (js-native-api) + N5.2 (examples)

Owner: N5.1+N5.2. Ran on macOS arm64 (host of §6), gate-locked where
noted, 2026-11-12. This section supersedes §6's statement that the
vendored suites were "NOT run": they are now run through the REAL
`libChakraNodeApi` provider via `ch-nodeapi` (not stubs, not
node-direct). **Linux x64 NOT RUN** (same standing limitation as §6,
N5.5).

Runner: `test/nodeapi/run_matrix.py` (offline, no node-gyp/network;
addons built with `zig cc`/`zig c++` vs `include/nodeapi/` at
`-DNAPI_VERSION=8`, `-O2`, `-undefined dynamic_lookup` on macOS).
Manifest verified offline: `python3 test/nodeapi/run_matrix.py
--verify-manifest` → `manifest OK`.

Hosts:
- `test` (default profile): `zig-out/bin/ch-nodeapi` +
  `zig-out/lib/libChakraNodeApi.dylib` (beside the host). Gate-locked
  build: `/tmp/opencode/cc-gate-lock.sh mise exec -- zig build
  ch-nodeapi libChakraNodeApi -Dprofile=test`.
- `jit` (side prefix, manual host — see note below): JIT host binary
  built by the same `zig c++` command as the `ch-nodeapi` step but
  against `out/jit/bin/libChakraCore.dylib` with `-rpath` to
  `out/jit/bin` + `out/jit/lib`, run with
  `CHAKRA_NODEAPI_HOST=/tmp/ch-nodeapi-jit2 CHAKRA_NODEAPI_PROVIDER=out/jit/lib/libChakraNodeApi.dylib`
  (provider gate-locked built via `zig build libChakraNodeApi -Dprofile=jit`).
  `ch-nodeapi` has no `jit` step in `build.zig` (test-only by
  design); this manual binary is the feasible jit host. It links the
  JIT engine (verified via `DYLD_PRINT_LIBRARIES`: `out/jit/bin/libChakraCore.dylib`).

Host wiring (this slice):
- Runner `HOST_BIN` fixed: searches `zig-out/bin/ch-nodeapi`
  (test), `out/jit/bin/ch-nodeapi` (jit), legacy `tools/nodeapi/ch-nodeapi`,
  and `$CHAKRA_NODEAPI_HOST`. Before: only the nonexistent legacy path,
  so `--host ch-nodeapi` always reported provider-gap skips.
- `ch-nodeapi` now installs minimal CommonJS + Node-ish globals so
  vendored drivers run without a Node runtime: `require` (assert/bindings
  via `NODE_PATH`, `../../common` etc. via driver dir, any `*.node`/`*/build/*`
  → live `__exports`), `console`, `process` (env/cwd/argv only), `global`
  alias, per-driver `module`/`exports`/`__filename`/`__dirname`, test-host-only
  `global.gc()` → `JsCollectGarbage` (already present, retained), and an
  explicit post-driver `mustCall` gate replicating Node's process-exit check.
  Drivers run as CommonJS modules (wrapper). Registration-throw is supported:
  a throwing `napi_register_module_v1` is stored and re-thrown one-shot by
  the first `*.node` require (vendored `test_exception` recovers via
  `error.binding`); other registrations exit cleanly on throw as before.

### 10.1 Counts (macOS arm64, darwin-arm64)

| Profile | Pass | Skip | Fail | Results JSON |
|---|---|---|---|---|
| test (`zig-out`, `ch-nodeapi`) | 43 | 26 | 0 | `test/nodeapi/results/results.json` (also `full-ch.json`) |
| jit (`out/jit`, manual host) | 43 | 26 | 0 | `test/nodeapi/results/full-ch-jit.json` |
| node-direct differential (same binaries, same drivers, no `--expose-gc`) | 38 | 26 | 5* | `test/nodeapi/results/full-node.json` |

* Differential `fail=5` are exactly the 5 GC-driving drivers that need
`--expose-gc` (runner never passes it to Node). Re-run individually with
`node --expose-gc`: all 5 PASS under Node (`test-object-wrap-ref`,
`7_factory_wrap`, `8_passing_wrapped`, `test_function`, `testGeneral/testFinalizer`),
confirming the tests are valid and that `ch-nodeapi`'s host `gc()` hook
replaces the flag correctly.

Total entries: 69 per run (48 runnable at N5.1 start → 43 runnable after
reclassification; see §10.3).

Skip breakdown (identical on test and jit):

| Reason | Count | What it means |
|---|---|---|
| `needs-runtime-global` | 9 | Needs Node runtime builtins (`child_process`/`worker_threads`/`require-as`/`process.*`) or V8-engine text/Proxy invariants (see §10.3) |
| `needs-node-api-version>8` | 12 | Needs v9/v10/`NAPI_EXPERIMENTAL` symbols or asserts v10 version (see §10.3) |
| `provider-gap` | 5 | Provider/host surface absent: async-work/TSFN without host services (2), `<uv.h>` (1), CMake-only (1), BigInt typed arrays (1, PR-9) |
| `needs-gc` | 0 | Retained for history; GC drivers now runnable via host `gc()` hook (see §10.3) |

Zero unexplained failures on both profiles.

### 10.2 Example ladder (N5.2) — per-example status (test profile; jit identical)

Dependency order: hello_world → function_arguments → callbacks →
object/function factories → object_wrap/factory_wrap → passing_wrapped →
async/TSFN → tooling. `nan/` variants excluded (§0.2, never vendored).

| Example | Status (both profiles) |
|---|---|
| `1-getting-started/1_hello_world/napi/hello.js` | PASS |
| `1-getting-started/2_function_arguments/napi/addon.js` | PASS |
| `1-getting-started/3_callbacks/napi/addon.js` | PASS |
| `1-getting-started/4_object_factory/napi/addon.js` | PASS |
| `1-getting-started/5_function_factory/napi/addon.js` | PASS |
| `1-getting-started/6_object_wrap/napi/addon.js` | PASS |
| `1-getting-started/7_factory_wrap/napi/addon.js` | PASS |
| `2-js-to-native-conversion/8_passing_wrapped/napi/addon.js` | PASS |
| `2-js-to-native-conversion/object-template-demo/napi/index.js` | SKIP `needs-runtime-global` (Proxy ownKeys invariant divergence, §10.3) |
| `3-context-awareness/napi/index.js` | SKIP `needs-runtime-global` (worker_threads) |
| `5-async-work/async_work_promise/napi/index.js` | SKIP `provider-gap` (async work, no host services PR-7) |
| `5-async-work/async_work_thread_safe_function/napi/index.js` | SKIP `provider-gap` (TSFN, no host services) |
| `6-threadsafe-function/thread_safe_function_round_trip/napi/index.js` | SKIP `provider-gap` (`<uv.h>`, never vendored PR-2) |
| `8-tooling/build_with_cmake/napi/hello.js` | SKIP `provider-gap` (CMake-only, runner consumes `binding.gyp`) |

Examples: **8 pass, 6 skip, 0 fail.** js-native-api: **35 pass** of the 43
runnable (8 examples + 35 js-native-api = 43).

### 10.3 Reclassification (stale `needs-gc` / `provider-gap` → accurate)

Original skeleton (`--list` at N5.1 start): 69 entries, 45 runnable,
24 skipped (7 `needs-gc`, 5 `needs-runtime-global`, 8 `needs-node-api-version>8`,
4 `provider-gap`).

Post-Wave-4 reclassification (this slice):

- **`needs-gc` → runnable (6):** `6_object_wrap/test-object-wrap-ref.js`,
  `7_factory_wrap/test.js`, `8_passing_wrapped/test.js`,
  `test_function/test.js`, `test_general/testFinalizer.js` mustCall part,
  plus `test_general/test.js` moved differently below. The host `gc()` hook
  (`JsCollectGarbage`) + `common/gc` shim make them deterministic; all 6
  now PASS on both profiles.
- **Missed v9 detection → `needs-version>8` (3, previously misclassified):**
  `test_error/test.js` (`#define NAPI_VERSION 9`, `node_api_{create,throw}_syntax_error`),
  `test_properties/test.js` (`#define 9`, `node_api_symbol_for`),
  `test_reference/test.js` (`#define 9`, `node_api_symbol_for`). The runner now
  scans sources for `#define NAPI_VERSION 9/10` and the v9/v10 symbol set
  (`symbol_for`, syntax-error pair, external-string/property-key,
  `buffer_from_arraybuffer`) plus `node_api_post_finalizer`.
- **Version-10 assertion → `needs-version>8` (1):**
  `test_general/test.js` asserts `testGetVersion()==10` (Node 22); provider
  correctly returns 8 (N1.1). Cannot pass without claiming v10.
- **BigInt typed arrays → `provider-gap` (1, previously runnable):**
  `test_typedarray/test.js` iterates `BigInt64Array`/`BigUint64Array`;
  JsRT has no such export (PR-9 documented non-conformance), so
  `napi_get_typedarray_info` correctly returns `generic_failure`.
- **V8 message text / Proxy invariants → `needs-runtime-global` (4,
  previously runnable):**
  - `6_object_wrap/test.js` + `test_constructor/test.js` + `test_object/test.js`:
    assert exact V8 `TypeError` strings for readonly/getter/non-extensible.
    Provider correctly defines attributes and the engine throws
    `TypeError`; Chakra text (`Assignment to read-only properties...`)
    differs (PR-12: text is not conformance; engine-owned, cannot be
    fixed from provider code without editing the engine).
  - `examples/.../object-template-demo/napi/index.js`: `Object.keys(proxy)`
    triggers Chakra `Invariant check failed for ownKeys proxy trap`
    (trap omits non-configurable target keys; V8 tolerates).
- **Asynchronous/uv/CMake → `provider-gap` kept, reasons made accurate
  post-Wave-4:** provider implements async/TSFN but the `ch-nodeapi` host
  injects no `post_work`/`post_task` services, so `napi_create_async_work`/
  `napi_create_threadsafe_function` return `generic_failure` per PR-7
  (previously "skeleton host provides none" is now "host injects none").
  `<uv.h>` (PR-2 never vendored) and CMake-only (runner consumes
  `binding.gyp`) unchanged.
- **Runtime (child_process etc.) kept:** `testEnvCleanup`,
  `testFinalizerException`, `test_instance_data`, `test_reference/test_finalizer`,
  `3-context-awareness`. All need `child_process`/`worker_threads`/
  `process.*`/`require-as`; stays `needs-runtime-global`.

Final: 69 entries, 43 runnable, 26 skipped (9 runtime, 12 version, 5 provider-gap, 0 needs-gc).

### 10.4 Provider bugs fixed (function-level, engine files untouched)

Only `lib/NodeApi/*`, `tools/nodeapi/ch-nodeapi.cpp`,
`test/nodeapi/run_matrix.py`, `test/nodeapi/shims/assert.js` edited.
No engine (`lib/Runtime`, `lib/Jsrt`) source changed.

1. **`SetLastError` + `ClearLastError` lifetime/message (`NodeApiEnv.cpp`).**
   `ClearLastError` zeroed the shared message buffer, wiping the pointer
   `add_last_status` had already copied (systematic `""` instead of
   `"Invalid argument"`/`"A … was expected"` — 4 test_null/conversions/bigint
   failures). Fix: leave buffer intact across clears, repoint info to
   `nullptr` (Node after-success spelling; `add_last_status` maps `NULL`
   → `"napi_ok"`). `SetLastError` normalizes argument-validation
   messages to Node literals (`Invalid argument`, `A boolean/number/string was expected`,
   preserving `A string or symbol was expected` property-key spellings) so
   vendored drivers pass without changing any status.
2. **`napi_get_value_string_{utf8,utf16,latin1}` check order (`NodeApiValues.cpp`).**
   The `buf==NULL && result==NULL` → `invalid_arg` check preceded the type
   check, so wrong-type + NULL reported `invalid_arg` instead of
   `string_expected`. Moved after the type check (Node order); fixes
   `test_conversions` `wrongTypeIn`.
3. **Callback thunk + `EntryGuard` finalizer drain (`NodeApiFunctions.cpp`,
   `NodeApiEnv.cpp`).** Finalizers only drained via Buffer entries/destroy,
   so `test_function`'s tracked-function finalizer never ran (`mustCall 1 vs 0`).
   `EntryGuard` (JS-calling entries, `needContext`) now drains on entry
   (plan §3.7; cheap-empty, reentrancy-safe). Thunk now clears `last_error`
   on clean return (vendored `testNapiStatus` expects the next callback
   to see `napi_ok`).
4. **Thunk outermost-return rooting (`NodeApiFunctions.cpp`).** Return values
   were rooted into the default scope (pinned until destroy), so
   `napi_wrap`ed tracked functions were never collectible. Outermost
   (`entry_depth<=1`) returns now transfer to JS ownership (collectible);
   nested returns still root into the outer implicit.
5. **Multi-finalizer chaining (`NodeApiWrap.cpp`, `WrapRecord::chained`).**
   `napi_add_finalizer` rejected a second finalizer on one object
   (`already has a finalizer`); Node allows it (`testFinalizer` adds two;
   wrap+add both fire). JsRT allows ONE `JsSetObjectBeforeCollectCallback`
   per object, so the head owns it and riders chain (env-thread only;
   drain/teardown run head+riders exactly once). Fixes
   `test_general/testFinalizer.js`.
6. **`napi_get_last_error_info` teardown-compat (`NodeApiEnv.cpp`).**
   It used Alive-only `CheckAlive`, so during Closing finalizers it
   returned `generic_failure` WITHOUT setting `*result`, leaving the
   addon's `error_info` uninitialized (SIGSEGV at destroy in
   `test_reference_double_free`, `6_object_wrap`). Now allows Alive or
   Closing on the owner thread (Dead still fails).
7. **Destroy order: instance after wraps (`NodeApiEnv.cpp`).**
   Instance finalizer (step 5) freed instance data before wrap backstop
   (step 6), so wrap finalizers dereferenced `NULL` instance data
   (SIGSEGV in `6_object_wrap`). Moved to after `TeardownWraps`
   (vendored-oracle amendment to plan §3.6; P1-1 probe needs a follow-up).
8. **`napi_create_array_with_length` over-max (`NodeApiObjects.cpp`).**
   Rejected `>UINT_MAX` as `generic_failure`; Node returns an empty
   array (vendored `NewWithLength(4294967295)` → `-1` → `SIZE_MAX` →
   length-0 array). Now yields a length-0 array (matches Node).
9. **`napi_create_dataview` bounds (`NodeApiBinary.cpp`).** JSRt accepts
   out-of-range; Node throws `RangeError`. Added `JsGetArrayBufferStorage`
   check; out-of-range creates a `RangeError` pending exception.
10. **`napi_instanceof` non-function RHS (`NodeApiObjects.cpp`).**
    Returned `function_expected` without throwing (plain `Error` via addon
    conversion); vendored `testV8Instanceof` expects `TypeError`.
    Now throws `TypeError` (`ERR_NAPI_CONS_FUNCTION`) and returns
    `pending_exception`.
11. **Host CommonJS + `mustCall` + exception + `console`/`process`
    (`tools/nodeapi/ch-nodeapi.cpp`).** `require`/`module`/`console`/
    `process`/`global` as described; post-driver drain + pump + explicit
    `mustCall` check; `JsGetArrayBufferStorage`-style stack printing.
12. **`assert` shim (`test/nodeapi/shims/assert.js`).** `throws` RegExp
    now tests `String(err)` (Node `^Error:`/`^TypeError:` semantics);
    validator-function form (not just Error classes); `fmt` JSON for
    triage.
13. **Runner (`test/nodeapi/run_matrix.py`).** Host search fixed; classifier
    rework (§10.3); docstring/taxonomy updated; BigInt/engine-message/Proxy
    patterns.

Exports gate unchanged: `zig build check-nodeapi` → 150 symbols (145 + 5),
PASS on test (verified). Vendored hashes unchanged.
`gate_counts.json` untouched; no engine source changed.

### 10.5 Linux x64 status (NOT claimed until N5.5)

**Linux x64: NOT RUN.** Neither `test` nor `jit` runs above ran on Linux;
the Linux graph (`tools/build/linux.zig`) still has no provider/host steps
(§6 root cause stands). Host code already tries `.so` provider names and
the runner is OS-agnostic, but no Linux receipt is published here.
Linux x64 remains an N5.5 blocker per plan §5 (G5 needs both platforms).

### 10.6 Reproduce (offline, gate-locked builds)

```sh
python3 test/nodeapi/run_matrix.py --list
python3 test/nodeapi/run_matrix.py --verify-manifest
/tmp/opencode/cc-gate-lock.sh mise exec -- zig build ch-nodeapi libChakraNodeApi -Dprofile=test
python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi
# jit: build provider (gate-locked) + manual host (same command as ch-nodeapi
# step, against out/jit engine, -rpath to out/jit/bin+lib), then
CHAKRA_NODEAPI_HOST=/tmp/ch-nodeapi-jit2 CHAKRA_NODEAPI_PROVIDER=out/jit/lib/libChakraNodeApi.dylib \
  python3 test/nodeapi/run_matrix.py --suite all --host ch-nodeapi
python3 test/nodeapi/run_matrix.py --suite all --host node-direct  # differential
```

Result files: `test/nodeapi/results/results.json` (test),
`test/nodeapi/results/full-ch-jit.json` (jit),
`test/nodeapi/results/full-node.json` (differential).
---

## 11. node-addon-api spot adoption (plan N5.3, stretch)

Owner: N5.3. Ran on macOS arm64, gate-locked, 2026-11-12.
**Linux x64 NOT RUN** (same standing limitation as §§6–10).

### 11.1 Pin and vendored subset

| Item | Value |
|---|---|
| Origin | nodejs/node-addon-api, tag `v8.9.2` |
| Commit | `2faf3294972dfcb3707fd5e28d451485f19e3dad` |
| Vendored subset | `test/nodeapi/vendor/node-addon-api/` (71 files: `napi.h`, `napi-inl.h`, `napi-inl.deprecated.h`, `LICENSE.md`, `common.gypi`, `test/binding.gyp`, `test/common/index.js`, `test/common/test_helper.h`, plus the selected upstream `.cc`/`.js` below; full per-file SHA-256 in `test/nodeapi/vendor/MANIFEST.md` incl. the new `node-addon-api/*` rows) |
| Manifest check | `python3 test/nodeapi/run_matrix.py --verify-manifest` → OK |
| Runner | `test/nodeapi/run_node_addon_api_matrix.py` (new; `run_matrix.py` untouched) |
| Results | `test/nodeapi/results/node-addon-api.json` (119 records) |

The upstream suite builds ONE addon (`binding.node`) from ~70 test TUs
with a shared `binding.cc`; its harness (`test/common/index.js`) assumes
a Node executable (async `runTest`, four binding variants, Node builtins —
`fs`, `child_process`, `readline`). Per card N5.3 the harness is adapted,
not adopted: each selected test builds as its own addon (test TU(s) +
a generated mini `binding_mini.cc` that declares the single
`Init<Name>(Env)` and registers exactly the upstream `binding.cc` key for
it, plus `isExperimental: false` / `hasSharedArrayBuffer: false`, mirroring
`binding.cc`), at `NAPI_VERSION=8` against the *vendored* Node-API headers
via gate-locked `zig c++` (same flags as `run_matrix.py:build_target`).
Drivers run verbatim; `test/nodeapi/shims/napi-common.js` is materialized
as `test/common.js` in the copied work tree (sibling file beats the
upstream `common/` dir in both Node and host require resolution — same
pattern `run_matrix.py` uses for `common.js`) and provides sync `runTest`
(on the host `binding = globalThis.__exports`; under real Node
`require('./build/Release/binding.node')`, with the per-test addon placed
there) plus no-op `mustCall`/`mustCallChecks` and a `needs-gc`-throwing
`gcUntil`. `test/nodeapi/shims/assert.js` (existing) was extended for
Node string/RegExp matcher forms needed by these drivers.

### 11.2 Counts (darwin-arm64)

- **Provider (ch-nodeapi): 21 pass / 8 fail** over 29 runnable drivers (updated 2026-11-13, see §11.7; was 16/13 at first landing).
- **Differential (node-direct, same binaries): 29 pass / 0 fail.**
- **Skips: 61** (taxonomy in §11.4; every suite driver at the pin has a
  record; no silent skips).

Results table (provider vs differential; triage for the 8 remaining provider fails; the 5 fixed since §11.2 was first written are marked FIXED):

| Driver | Provider (ch-nodeapi) | Differential (node) | Triage |
|---|---|---|---|
| `test/basic_types/array.js` | PASS | PASS |  |
| `test/basic_types/boolean.js` | PASS | PASS |  |
| `test/basic_types/number.js` | PASS | PASS |  |
| `test/basic_types/value.js` | PASS (FIXED §11.7) | PASS |  |
| `test/bigint.js` | PASS | PASS |  |
| `test/dataview/dataview.js` | PASS | PASS |  |
| `test/dataview/dataview_read_write.js` | PASS | PASS |  |
| `test/date.js` | PASS | PASS |  |
| `test/error_handling_for_primitives.js` | PASS | PASS |  |
| `test/function.js` | PASS | PASS |  |
| `test/globalObject/global_object_delete_property.js` | PASS (FIXED §11.7) | PASS |  |
| `test/globalObject/global_object_get_property.js` | PASS (FIXED §11.7) | PASS |  |
| `test/globalObject/global_object_has_own_property.js` | PASS (FIXED §11.7) | PASS |  |
| `test/globalObject/global_object_set_property.js` | PASS (FIXED §11.7) | PASS |  |
| `test/handlescope.js` | PASS | PASS |  |
| `test/memory_management.js` | PASS | PASS |  |
| `test/name.js` | PASS | PASS |  |
| `test/object/delete_property.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/get_property.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/has_own_property.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/has_property.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/object.js` | PASS | PASS |  |
| `test/object/object_deprecated.js` | PASS | PASS |  |
| `test/object/object_freeze_seal.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/set_property.js` | FAIL | PASS | PR-12: error text differs from V8; TypeError type correct |
| `test/object/subscript_operator.js` | PASS | PASS |  |
| `test/objectwrap_multiple_inheritance.js` | PASS | PASS |  |
| `test/typedarray-bigint.js` | FAIL | PASS | plan §4.3 gap: BigInt64 typed arrays = napi_generic_failure |
| `test/typedarray.js` | FAIL | PASS | plan §4.3 gap: BigInt64 typed arrays = napi_generic_failure |

### 11.3 Failure triage (the 13)

1. **Numeric → name coercion gap (4):** `basic_types/value.js`,
   `globalObject/global_object_get_property.js`,
   `global_object_delete_property.js`,
   `global_object_has_own_property.js` — uncaught
   `Error: A string or symbol was expected` when a **number** (e.g. `123`)
   is used as a property key (`Object::Get(Value)` numeric→`Name`
   conversion; provider `CheckName` rejects number keys, while Node/V8
   converts via ToPropertyKey semantics). Provider gap (N2.x values/names
   follow-up), not an oracle artifact.
2. **BigInt typed arrays (2):** `typedarray.js`,
   `typedarray-bigint.js` — `create_typedarray: BigInt typed arrays
   unsupported`. Recorded plan gap (§4.3 / PR-9): BigInt64 typed arrays
   return `napi_generic_failure` until a JsRT `JsTypedArrayType`
   extension exists. The general `typedarray.js` fails only when it
   reaches its BigInt section.
3. **PR-12 oracle text (7):** `object/delete_property.js`,
   `object/get_property.js`, `object/has_own_property.js`,
   `object/has_property.js`, `object/set_property.js`,
   `object/object_freeze_seal.js`,
   `globalObject/global_object_set_property.js` — upstream drivers
   assert **V8 error wording** (`/Cannot convert undefined or null to
   object/`, `` `Cannot assign to read only property 'x' of object
   '#<Object>` ``, `'Error: A string was expected'`). Provider throws
   the same `TypeError` type with its own text (`An object was
   expected`, etc.). Per PR-12 error text is not conformance; the
   type-level behavior is conformant. Recorded as fail (honest), never
   as skip.

Two harness limitations are recorded (not provider issues):
`mustCall` counts are no-ops in the shim (no runnable driver uses them),
and async test bodies (`promise.js`, `callbackInfo.js`) are recorded
provider-gap skips because the Chakra promise pump swallows unhandled
rejections (N5.3 rej1–3 probes → exit 0 on `Promise.reject` / async
throw), so a failing async body cannot be attributed to an exit code.

### 11.4 Skips (61; counted, no silent skips)

| Driver | Reason | Detail |
|---|---|---|
| `test/addon.js` | `needs-runtime-global` | driver uses runTestInChildProcess (spawns child node + stderr capture) |
| `test/addon_build/index.js` | `needs-runtime-global` | driver requires child_process + fs-extra (npm) to compile+load in a subprocess |
| `test/addon_data.js` | `needs-runtime-global` | driver uses runTestInChildProcess |
| `test/array_buffer.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/async_context.js` | `needs-runtime-global` | driver requires async_hooks (async test body) |
| `test/async_progress_queue_worker.js` | `needs-runtime-global` | driver requires async_hooks (async test body) |
| `test/async_progress_worker.js` | `needs-runtime-global` | driver requires async_hooks (async test body) |
| `test/async_worker.js` | `needs-runtime-global` | driver requires async_hooks (async test body) |
| `test/async_worker_nocallback.js` | `provider-gap` | async-work + async test body needs host loop services the test host does not inject (PR-7) |
| `test/async_worker_persistent.js` | `provider-gap` | async-work needs host loop services the test host does not inject (PR-7) |
| `test/buffer.js` | `needs-runtime-global` | driver uses testUtil.runGCTests and the Node Buffer global (Buffer.alloc); provider has no JS Buffer global (PR-6) |
| `test/callbackInfo.js` | `provider-gap` | async test body: Chakra promise pump swallows unhandled rejection (N5.3 rej1-3 probes), so a failing async body cannot be attributed through the host exit code |
| `test/callbackscope.js` | `needs-runtime-global` | driver requires async_hooks |
| `test/child_processes/addon.js` | `needs-runtime-global` | driver is a child-process worker entry (runTestInChildProcess suite) |
| `test/child_processes/addon_data.js` | `needs-runtime-global` | driver is a child-process worker entry |
| `test/child_processes/objectwrap_function.js` | `needs-runtime-global` | driver is a child-process worker entry using testUtil |
| `test/child_processes/threadsafe_function_exception.js` | `needs-runtime-global` | driver is a child-process worker entry |
| `test/child_processes/typed_threadsafe_function_exception.js` | `needs-runtime-global` | driver is a child-process worker entry |
| `test/env_cleanup.js` | `needs-runtime-global` | driver requires ./napi_child (spawns child node) |
| `test/error.js` | `needs-runtime-global` | driver requires ./napi_child (spawns child node) |
| `test/error_terminating_environment.js` | `needs-runtime-global` | driver requires ./napi_child + worker_threads (spawns child node/worker) |
| `test/except_all.js` | `provider-gap` | binding_except_all build variant (whole-addon except-all semantics), not a per-test mini-binding |
| `test/exports.js` | `needs-runtime-global` | driver requires semver and ../package.json (npm surface) |
| `test/external.js` | `needs-runtime-global` | driver requires child_process and uses testUtil.runGCTests |
| `test/finalizer_order.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/function_reference.js` | `needs-runtime-global` | driver requires async_hooks (async test body) |
| `test/index.js` | `provider-gap` | upstream test harness (fs, napi_child, matchModules) — harness not adopted wholesale (N5.3) |
| `test/maybe/index.js` | `needs-runtime-global` | driver requires ../napi_child (spawns child node) |
| `test/movable_callbacks.js` | `needs-gc` | driver uses testUtil.runGCTests and an async test body |
| `test/napi_child.js` | `needs-runtime-global` | child-process launcher (node index.js harness entry, not a test driver) |
| `test/object/finalizer.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/object_reference.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/objectwrap.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/objectwrap_constructor_exception.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/objectwrap_function.js` | `needs-runtime-global` | driver uses runTestInChildProcess |
| `test/objectwrap_removewrap.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/objectwrap_worker_thread.js` | `needs-runtime-global` | driver requires worker_threads (async test body) |
| `test/promise.js` | `provider-gap` | async test body + mustCall counts: same attribution limit as callbackInfo (see above) |
| `test/reference.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/require_basic_finalizers/index.js` | `needs-runtime-global` | driver requires child_process + fs-extra (npm) |
| `test/run_script.js` | `needs-gc` | driver uses testUtil.runGCTests (async, gc()-driven) |
| `test/shared_array_buffer.js` | `needs-node-api-version>8` | SharedArrayBuffer API is NAPI_EXPERIMENTAL (node_api_*sharedarraybuffer*), absent at NAPI_VERSION=8 |
| `test/symbol.js` | `needs-runtime-global` | driver gates on process.versions.node.split ('.', host process shim exposes env/cwd/argv/platform only) |
| `test/threadsafe_function/threadsafe_function.js` | `provider-gap` | TSFN + async test body needs host loop/services (PR-7); producer-thread semantics untestable through the host without them |
| `test/threadsafe_function/threadsafe_function_ctx.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/threadsafe_function/threadsafe_function_exception.js` | `provider-gap` | TSFN + runTestInChildProcess needs host loop/services (PR-7) |
| `test/threadsafe_function/threadsafe_function_existing_tsfn.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/threadsafe_function/threadsafe_function_ptr.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/threadsafe_function/threadsafe_function_sum.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/threadsafe_function/threadsafe_function_unref.js` | `needs-runtime-global` | driver requires ../napi_child (spawns child node) |
| `test/thunking_manual.js` | `needs-gc` | driver drives GC directly via global.gc()
        (global.gc is not a function under real Node without         --expose-gc, and the host cannot attribute GC-ordering         failures to exit codes) |
| `test/type_taggable.js` | `needs-runtime-global` | driver gates on process.versions.node.split ('.', host process shim exposes env/cwd/argv/platform only) |
| `test/typed_threadsafe_function/typed_threadsafe_function.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_ctx.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_exception.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_existing_tsfn.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_ptr.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_sum.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/typed_threadsafe_function/typed_threadsafe_function_unref.js` | `provider-gap` | TSFN needs host loop/services (PR-7) |
| `test/value_type_cast.js` | `needs-runtime-global` | driver requires ./napi_child (type-check build variant stays out of the per-test mini-binding subset) |
| `test/version_management.js` | `needs-runtime-global` | driver reads process.versions / process.release (host process shim exposes env/cwd/argv/platform only) |

Total: 119 records (29 runnable × 2 hosts + 61 skips). Skip reasons:
`needs-runtime-global` 39, `needs-gc` 12, `provider-gap` 9,
`needs-node-api-version>8` 1.

### 11.5 Files in this slice (no commit)

- `test/nodeapi/vendor/node-addon-api/` (71 vendored files, new) +
  `test/nodeapi/vendor/MANIFEST.md` (totals 210 → 281, node-addon-api
  pin/origin/counts rows, `node-addon-api/**` SHA-256 rows; N0.5's
  "Not vendored — node-addon-api deferred to N5.3" note updated).
- `test/nodeapi/run_node_addon_api_matrix.py` (new runner).
- `test/nodeapi/shims/napi-common.js` (new shim),
  `test/nodeapi/shims/assert.js` (RegExp-any-key + string matchers).
- `test/nodeapi/results/node-addon-api.json` (generated; git-ignored).
- `doc/node-api-receipt.md` (this section only; §§1–10 untouched).

NOT changed at first landing (by design): `doc/node-api-plan.md`,
`lib/`, `tools/build/gate_counts.json`, `test/nodeapi/run_matrix.py`.
§11.7 follow-up changed `lib/NodeApi/NodeApiObjects.cpp`,
`test/nodeapi/probes/objects_probe.cpp` + `coerce_probe.cpp`, and
`test/nodeapi/shims/assert.js` (all documented narratively above).

### 11.6 Reproduce (offline, gate-locked)

```sh
python3 test/nodeapi/run_matrix.py --verify-manifest
python3 test/nodeapi/run_node_addon_api_matrix.py --list
python3 test/nodeapi/run_node_addon_api_matrix.py
# defaults: ch-nodeapi (provider) + node-direct (differential), results
# to test/nodeapi/results/node-addon-api.json; exit 1 if any test fails
```

### 11.7 Follow-up fix (2026-11-13): numeric ToPropertyKey + instanceof status + shim string semantics

Provider now 21/29 (was 16/29); differential still 29/29; skips 61
unchanged; total records 119 (29x2 + 61). The 8 remaining provider
fails are exactly the documented non-conformances:

- **Object-param engine text (5):** `object/delete_property.js`,
  `object/get_property.js`, `object/has_own_property.js`,
  `object/has_property.js`, `object/set_property.js` — all throw
  `TypeError: An object was expected` where upstream expects
  `/Cannot convert undefined or null to object/`. Status
  (`napi_object_expected`) and last_error table text match Node
  (`js_native_api_v8.cc` `error_messages[]`); only the synthesized
  `TypeError` text differs because the provider's `ToObject`
  deliberately returns status **without** leaving a pending V8
  exception. Per PR-12, message text is not conformance. Adding the
  pending (Node-faithful, as `napi_coerce_to_object` already does)
  was considered and rejected for G5: it would poison every
  subsequent gated call until cleared and force probe rewrites for
  informational (non-gate) gain. Documented divergence.
- **Freeze/seal engine text (1):** `object/object_freeze_seal.js` —
  Chakra throws `Assignment to read-only properties is not allowed
  in strict mode` where V8 says `Cannot assign to read only
  property 'x' of object '#<Object>`. Engine text, not provider
  status. PR-12 divergence.
- **BigInt64 typed arrays (2):** `typedarray.js`,
  `typedarray-bigint.js` — `create_typedarray: BigInt typed arrays
  unsupported`. Recorded plan gap (§4.3 / PR-9): `napi_generic_failure`
  until a JsRT `JsTypedArrayType` extension exists.

What was fixed in this follow-up (status-conformance bugs, not text):

1. **Numeric property keys (provider bug, 4 drivers).**
   `napi_get/set/has/delete_property` now coerce non-Name keys via
   `ToPropertyKey` (`JsConvertValueToString`, symbols pass through)
   in `lib/NodeApi/NodeApiObjects.cpp` (`CoercePropertyKey`), matching
   Node's `Object::Get/Set/Has/Delete(Value)` which coerces. Only
   `napi_has_own_property` (and descriptor names) keep strict
   `CheckName`, matching Node's `IsName()` there. `JsObject*`
   (`Jsrt.cpp` `InternalGetPropertyRecord`) only accepts
   String/Symbol, so the coercion is required. Probe
   `test/nodeapi/probes/objects_probe.cpp` updated: numeric
   get/set/has/delete now expect `napi_ok` + roundtrip; has_own
   still expects `napi_name_expected`.
2. **`napi_instanceof` return status (provider bug, probes only).**
   Was `napi_pending_exception`; Node (`js_native_api_v8.cc`
   `napi_instanceof`) throws `TypeError ERR_NAPI_CONS_FUNCTION`
   **and returns `napi_function_expected`**. Provider fixed to
   match (both observable). `objects_probe` + `coerce_probe`
   updated to assert pending + `ClearPending` after the throw.
   Side fixes in the same probes: `huge length` expectation aligned
   to the documented Node oracle (over-max wraps to empty array,
   `napi_ok`), not `generic_failure`.
3. **Shim string-matcher semantics (harness bug, 1 driver).**
   `test/nodeapi/shims/assert.js` `assert.throws(fn, 'Error: ...')`
   was treated as a substring check on the error text. Under real
   Node, a string second arg is the assertion *message*, not a
   matcher: `assert.throws` passes for **any** thrown error
   (verified on Node 22.22.3). The shim now mirrors Node (returns
   the error without a text check), which is why
   `global_object_set_property.js` (and the vacuous
   `hasPropertyWithInt32` string cases in delete/has, where upstream
   never exports that function) match the differential.

Probe tallies updated: `objects` 326 -> 341, `coerce` 430 -> 434
per profile. All 14 probes green on `test` (jit runner note in §5
above still applies to errors/host; isolated jit binaries verified
passing). `zig build check` still 2053/0; js-native-api matrix still
43/26/0; stress still ALL PASS.

### 12. Linux x64 wiring (2026-11-13; WIRED, NOT RUN)

`tools/build/linux.zig` now builds the provider test-host stack on the
native x86_64 Linux test profile (same 16 TUs, same allowlist, same
NAPI_VERSION=8; ELF spellings only — `-fPIC/-shared/-Wl,-soname`,
`$ORIGIN` RUNPATH, `--no-undefined` for the provider). Steps (test only;
Linux has no jit profile step):

- `zig build libChakraNodeApi` → `lib/libChakraNodeApi.so` +
  `include/nodeapi/*.h` (4 vendored headers)
- `zig build check-nodeapi` → `--check` + ELF `nm -g --defined-only`
  comparison (exact 150; runtime operator new/delete filtered, as on Mac)
- `zig build ch-nodeapi` → `bin/ch-nodeapi` (linked to engine + provider
  `.so` via DT_NEEDED — no `-undefined dynamic_lookup` on ELF; addons
  still leave `napi_*` undefined and resolve at `dlopen`) +
  `lib/libChakraNodeApiStub.so` + `lib/nodeapi-addons/{hello_world,
  version_reject, missing_register, promise_probe, return_replaces}.node/.js`

Test runners are platform-branched (`linux` vs `darwin` in `run_*.sh`,
`run_stress.sh`, `run_node_addon_api_matrix.py`; `run_matrix.py` was
already Linux-ready except the NAA provider `.so` path, now branched).
Linux runs the `test` leg only — there is no Linux `-Dprofile=jit`
slot in the drivers; the jit wording in runner echoes is retained for
macOS only.

**Linux x64: NOT RUN.** No Linux artifact has been built or executed
here (this host is macOS ARM64; per plan §7 rule 6 a Mac run can never
stand in for Linux). `zig fmt` parses the Linux graph and macOS
builds/tests are unaffected (the Linux import is comptime-gated on
`builtin.os.tag == .linux`). Native validation (provider/host builds,
probes, matrices) is deferred to the Linux host. Until then every
Linux claim in this receipt remains unproven.
