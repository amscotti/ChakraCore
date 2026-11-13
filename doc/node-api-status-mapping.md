# Node-API status mapping (N0.6)

Status: **N0.6 deliverable**. Owner: N0.6. Date: 2026-11-11.
Companion to `doc/node-api-plan.md` §3.10 / PR-12 and
`doc/node-api-semantics.md` §6. Normative for N1.5 unless a probe
below proves otherwise.

Governing task: `doc/node-api-plan.md` §6 card N0.6 (extract from vendored
per-function docs: pending-allowlist, pending-vs-execute, `*_expected`
per getter; tabulate 145 v8 symbols; map `†N0.6-B1..B5`; confirm
`AMB-STATUS-1`, `AMB-SC-1`, `AMB-SC-2`, `AMB-REF-1`, `AMB-CB-2`).

## 0. Sources, method, and headline finding

**Vendored headers (primary source per plan PR-2 / N0.1):**

- `include/nodeapi/js_native_api.h` (SHA-256 `582e64ea…90d089705`, 607 lines)
- `include/nodeapi/node_api.h` (SHA-256 `2d456083…32a`, 265 lines)
- `include/nodeapi/js_native_api_types.h` / `node_api_types.h` (types + `napi_status` enum only)
- Pinned Node 22.22.3; manifest in `include/nodeapi/MANIFEST.md`; inventory in `doc/node-api-inventory.md` (145 symbols at `NAPI_VERSION=8`).

**Method:** every `//` comment in the four vendored headers was extracted
and assigned to its declaration. `/* */` block comments: 0 in both API
headers. The result is the inventory in Appendix A.

**Headline finding — the plan premise is wrong:** the vendored headers
contain **no per-function documentation of pending-exception behaviour
and no per-function `*_expected` statements**, with 8 exceptions total
(refs, one `get_reference_value` success case, one coerce warning, and the
`node_api_basic_env` GC-safety paragraph). The authoritative per-function
pending/`*_expected` documentation lives **upstream** in Node
`doc/api/n-api.md` (v22.22.3), which the vendored headers explicitly defer
to (`js_native_api_types.h:151-157` — “please also update … `doc/api/n-api.md`”).
`n-api.md` was **not vendored** by N0.1. This doc therefore tables all
three sources separately so N1.5 can implement from a single place:

| Source | What it proves |
|---|---|
| **A — Vendored headers** (`//` comments + declarations) | declarations, `basic_env` vs `env` typing, `napi_status` enum membership, 3 ref comments, 1 `get_reference_value` NULL case, coerce-runs-JS warning. **Silent on pending gates and `*_expected` for 141/145 functions.** |
| **B — Upstream docs** (`doc/api/n-api.md` v22.22.3, fetched for this task, NOT vendored) | global pending rule + 12 functions explicitly marked “*can be called even if there is a pending JavaScript exception*” + 12 Returns statements naming a specific `*_expected` code (plus 1 naming `invalid_arg` for `get_value_external`) + `create_reference` type list in prose. |
| **C — Upstream implementation** (`src/js_native_api_v8.cc` + `src/node_api.cc` + `src/js_native_api_v8.h` v22.22.3, fetched for this task, NOT vendored) | ground truth for Node/V8: `NAPI_PREAMBLE` gates (pending ⇒ `napi_pending_exception` without executing) vs `CHECK_ENV` only (executes), plus exact failure `napi_status` per type check. |

**Table semantics (column definitions):**

- `call-with-pending-ok?` = **C-implementation behaviour** (provider
  recommendation in §5). `No` means the implementation gates with
  `NAPI_PREAMBLE` and returns `napi_pending_exception` without executing;
  `Yes` means it executes and returns its own status with the pending
  exception preserved. The `notes` column records A and B per row so a
  second reader can verify the header silence without re-fetching upstream.
- `pending-returns` = what the entrygate returns when an exception is
  already pending. For `No` rows this is `napi_pending_exception`; for
  `Yes` rows it is “executes”. (A gated JS-calling function can *additionally*
  return `napi_pending_exception` after execution if the callee throws —
  noted in its row; that is a different path from the entry gate.)
- `type-mismatch status` = C-implementation failure code for that
  function's JS-value type check. `invalid_arg` is *not* a blanket fallback
  in Node — but it *is* what Node uses for many checks (see §2 disagreements).
  `n/a` means no JS-value type check exists (C-string/`void*`/integer inputs only).
  `none — ok+false` means wrong type is not an error at all (`is_*`, `typeof`, `get_new_target`, `get_reference_value`).

**How N1.5 should use this table:** implement the `Yes`/`No` gate exactly as
tabled (C column) and the `type-mismatch status` exactly as tabled. If
N1.5 decides to follow upstream docs B (12-function allowlist) instead of
implementation C (90-function allowlist), it must record that as a deliberate
divergence — B and C disagree on ~78 rows (see §2). The provider's pending
flag is provider-owned (JsRT has no native pending-exception state), so
either choice is “conforming” per B's “unspecified while pending” sentence —
but addon code may depend on C's leniency (see §5).



### Summary counts (C column)

| Set | Count | Functions |
|---|---|---|
| Provider target at v8 | 145 | inventory table |
| Gated (`No` → `napi_pending_exception` without executing) | 55 | see `No` rows |
| Executes with pending (`Yes` → own status) | 90 | see `Yes` rows (incl. `void` `napi_module_register` and `noreturn` `napi_fatal_error`) |
| Docs B callable-with-pending (explicit) | 12 | `get_last_error_info`, `get_and_clear_last_exception`, `is_exception_pending`, `fatal_error`, `close_handle_scope`, `close_escapable_handle_scope`, `escape_handle`, `delete_reference`, `delete_async_work`, `cancel_async_work`, `async_destroy`, `close_callback_scope` |
| Docs B callable ∩ C execute | 12 / 12 | all 12 docs-callable functions execute in C (consistent) |
| Docs B silent ∩ C gated | 55 | consistent (no doc says these *must* execute) |
| Docs B silent ∩ C execute | 78 | **divergence**: B does not mark these callable, C executes anyway (all `get_value_*` except `get_date_value`, all `is_*`, all simple `create_*`, all scope opens, all refs except via helper, all TSFN/cleanup/uv, etc.). See per-row notes. |

`node_api_basic_env` (GC-safe, “does not affect engine state”) functions in the
145 — headers prove this typing, docs/C confirm they never gate on engine
state, but **GC-safety ≠ pending-executability**: most take `basic_env`
yet C still gates some (`delete_reference` does not gate; `create_external`
*does* gate despite `basic_finalize` callback type — the gate is on the
call, not the callback):

`napi_get_last_error_info`, `napi_delete_reference`, `napi_get_version`,
`napi_adjust_external_memory`, `napi_set_instance_data`, `napi_get_instance_data`,
`napi_queue_async_work`, `napi_cancel_async_work`, `napi_get_node_version`,
`napi_get_uv_event_loop`, `napi_add_env_cleanup_hook`, `napi_remove_env_cleanup_hook`,
`napi_unref_threadsafe_function`, `napi_ref_threadsafe_function`,
`napi_add_async_cleanup_hook` (+ env-less `napi_get_threadsafe_function_context`,
`napi_call/acquire/release_threadsafe_function`, `napi_remove_async_cleanup_hook`).

## 1. Full 145-symbol table

Ordered alphabetically (inventory order).

| function | call-with-pending-ok? | pending-returns | type-mismatch status | notes |
|---|---|---|---|---|
| `napi_acquire_threadsafe_function` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_add_async_cleanup_hook` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `hook==NULL`) | B6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_add_env_cleanup_hook` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `fun==NULL`) | B6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_add_finalizer` | Yes | executes (own status; pending preserved) | `invalid_arg` if `js_object` not object (impl `IsObject` check; no `*_expected`) | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_adjust_external_memory` | Yes | executes (own status; pending preserved) | n/a (no JS value) | A6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_async_destroy` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `async_context==NULL`) | B3. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_async_init` | Yes | executes (own status; pending preserved) | `object_expected`/`string_expected` via `ToObject`/`ToString` (coercion; `null`/`undefined` fail) | B3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_call_function` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `func` not function — impl `CHECK_TO_FUNCTION` returns `invalid_arg`, NOT `function_expected`; `object_expected` if `recv` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_call_threadsafe_function` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_cancel_async_work` | Yes | executes (own status; pending preserved) | n/a (no JS value); already-started ⇒ `generic_failure` (docs) | B4. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_check_object_type_tag` | No | `napi_pending_exception` (no execute) | `object_expected` if `js_object` fails `ToObject`; untagged/mismatch ⇒ `napi_ok`+`false` (not error) | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_close_callback_scope` | Yes | executes (own status; pending preserved) | n/a (no JS value); `callback_scope_mismatch` if 0 scopes open | B3. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_close_escapable_handle_scope` | Yes | executes (own status; pending preserved) | n/a (scope kind); `handle_scope_mismatch` if 0 scopes open — impl shares one counter, cannot distinguish plain vs escapable (see AMB-SC-2) | A3. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_close_handle_scope` | Yes | executes (own status; pending preserved) | n/a (scope kind); `handle_scope_mismatch` if 0 scopes open — impl shares one counter | A3. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_coerce_to_bool` | No | `napi_pending_exception` (no execute) | none — any value coerces (never type-error; may throw ⇒ `pending_exception`) | A2. Header comment (js_native_api.h:174): “These APIs may execute user scripts.” Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_coerce_to_number` | No | `napi_pending_exception` (no execute) | none — any value coerces (never type-error; may throw ⇒ `pending_exception`) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_coerce_to_object` | No | `napi_pending_exception` (no execute) | none except `null`/`undefined` ⇒ `object_expected` (via `ToObject`); otherwise coerces, may throw | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_coerce_to_string` | No | `napi_pending_exception` (no execute) | none — any value coerces (never type-error; may throw ⇒ `pending_exception`) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_create_array` | Yes | executes (own status; pending preserved) | n/a | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_array_with_length` | Yes | executes (own status; pending preserved) | n/a | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_arraybuffer` | No | `napi_pending_exception` (no execute) | n/a (no JS input; `generic_failure` on alloc failure only) | A5. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_async_work` | Yes | executes (own status; pending preserved) | `object_expected`/`string_expected` via `ToObject`/`ToString` when non-NULL `async_resource` (coercion) | B4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_bigint_int64` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_bigint_uint64` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_bigint_words` | No | `napi_pending_exception` (no execute) | n/a (no JS input; `invalid_arg` if `word_count>INT_MAX`) | A1. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_buffer` | No | `napi_pending_exception` (no execute) | n/a | B2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_buffer_copy` | No | `napi_pending_exception` (no execute) | n/a | B2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_dataview` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `arraybuffer` not ArrayBuffer (impl `IsArrayBuffer`, NOT `arraybuffer_expected`); oversize ⇒ throws `RangeError`+`pending_exception` | A5. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_date` | No | `napi_pending_exception` (no execute) | n/a | A1. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_double` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_error` | Yes | executes (own status; pending preserved) | `string_expected` if `msg` not string (impl `IsString`); `code` non-string ⇒ `string_expected` via `set_error_code` | A4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_external` | No | `napi_pending_exception` (no execute) | n/a | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_external_arraybuffer` | No | `napi_pending_exception` (no execute) | n/a (no JS input; delegates to `create_external_buffer` then `get_typedarray_info`) | A5. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_external_buffer` | No | `napi_pending_exception` (no execute) | n/a (sandbox build ⇒ `no_external_buffers_allowed`) | B2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_function` | No | `napi_pending_exception` (no execute) | n/a | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_int32` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_int64` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_object` | Yes | executes (own status; pending preserved) | n/a | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_promise` | No | `napi_pending_exception` (no execute) | n/a (no JS input; `generic_failure` on alloc failure only) | A6. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_range_error` | Yes | executes (own status; pending preserved) | `string_expected` if `msg` not string (impl `IsString`) | A4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_reference` | Yes | executes (own status; pending preserved) | `invalid_arg` if `value` not object/function/symbol at `module_api_version<10` (impl `IsObject||IsFunction||IsSymbol`; external counts as object); no `*_expected` | A3. Header comment (js_native_api.h:330): weak iff initial_refcount==0, strong iff >0. No type list. Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_string_latin1` | Yes | executes (own status; pending preserved) | n/a (C string input) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_string_utf16` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_string_utf8` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_symbol` | Yes | executes (own status; pending preserved) | `string_expected` if `description` non-NULL and not string (impl `IsString`) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_threadsafe_function` | Yes | executes (own status; pending preserved) | `invalid_arg` if `func` not function (impl `CHECK_TO_FUNCTION`); `initial_thread_count==0` ⇒ `invalid_arg`; otherwise `object_expected`/`string_expected` via `ToObject`/`ToString` | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_type_error` | Yes | executes (own status; pending preserved) | `string_expected` if `msg` not string (impl `IsString`) | A4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_create_typedarray` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `arraybuffer` not ArrayBuffer (impl `IsArrayBuffer`, NOT `arraybuffer_expected`); bad `type`/alignment/length ⇒ `invalid_arg`/`RangeError` | A5. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_create_uint32` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_define_class` | No | `napi_pending_exception` (no execute) | `object_expected` not applicable; `name_expected` if descriptor name not Name, `invalid_arg` for NULL `constructor`/`properties` (binding args) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_define_properties` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject`; `name_expected` if descriptor key not Name; `invalid_arg` for NULL `properties` when `count>0` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_delete_async_work` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `work==NULL`) | B4. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_delete_element` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_delete_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_delete_reference` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `ref==NULL`); semantics double-delete ⇒ `invalid_arg` (provider design, not impl-checked) | A3. Header comment (js_native_api.h:337-338): delete/released/GC — no pending statement. Docs list callable-with-pending. |
| `napi_detach_arraybuffer` | Yes | executes (own status; pending preserved) | `arraybuffer_expected` if not ArrayBuffer; `detachable_arraybuffer_expected` if already detached / non-detachable (docs+impl agree) | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_escape_handle` | Yes | executes (own status; pending preserved) | `escape_called_twice` on second call (impl); non-escapable scope ⇒ impl UB (no check) — provider must return `invalid_arg` (AMB-SC-1) | A3. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_fatal_error` | Yes | n/a (`noreturn`; executes, never returns) | n/a (`void` noreturn, no status) | B1. Headers: silent (declaration only). Docs list callable-with-pending. Docs list callable-with-pending; impl has no pending check. |
| `napi_fatal_exception` | No | `napi_pending_exception` (no execute) | n/a (any `err` value accepted; `invalid_arg` only if `err==NULL`) | B1. Headers: silent (declaration only). Docs do not list (consistent with gated). On success it triggers `uncaughtException` and returns `napi_ok` (does NOT set pending); if already pending it returns `napi_pending_exception` without firing (plan §3.10, impl preamble). |
| `napi_get_all_property_names` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject`; bad `key_mode`/`key_conversion` enum ⇒ `invalid_arg` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_and_clear_last_exception` | Yes | executes (own status; pending preserved) | none — `NULL` result when no pending (not error) | A4. Headers: silent (declaration only). Docs list callable-with-pending. |
| `napi_get_array_length` | No | `napi_pending_exception` (no execute) | `array_expected` if not array (impl `IsArray`; docs silent, impl explicit) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_arraybuffer_info` | Yes | executes (own status; pending preserved) | `invalid_arg` if not ArrayBuffer/SharedArrayBuffer (impl `IsArrayBuffer||IsShared`, NOT `arraybuffer_expected`) | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_boolean` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_buffer_info` | Yes | executes (own status; pending preserved) | `invalid_arg` if not Buffer (impl `HasInstance`, NOT `object_expected`) | B2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_cb_info` | Yes | executes (own status; pending preserved) | none — bad `cbinfo` ⇒ `invalid_arg` only (binding arg, not JS type) | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_dataview_info` | Yes | executes (own status; pending preserved) | `invalid_arg` if not DataView (impl `IsDataView`, NOT `object_expected`) | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_date_value` | No | `napi_pending_exception` (no execute) | `date_expected` if not Date (docs+impl agree) | A1. Headers: silent (declaration only). Docs do not list (consistent with gated). Only `get_value_*` with preamble — gated; all other `get_value_*` execute. |
| `napi_get_element` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_global` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_instance_data` | Yes | executes (own status; pending preserved) | n/a | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_last_error_info` | Yes | executes (own status; pending preserved) | n/a (no JS value) | A4. Header: NO comment. Docs list callable-with-pending. |
| `napi_get_named_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_new_target` | Yes | executes (own status; pending preserved) | none — non-construct ⇒ `NULL` result with `napi_ok` (docs+impl agree) | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_node_version` | Yes | executes (own status; pending preserved) | n/a | B7. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_null` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_property_names` | No | `napi_pending_exception` (no execute) | `object_expected` (delegates to `get_all_property_names`) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_prototype` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_get_reference_value` | Yes | executes (own status; pending preserved) | none — collected weak ref ⇒ `napi_ok`+`NULL` (header+docs+impl agree) | A3. Header comment (js_native_api.h:359-361): successful call with NULL result when weak referent collected. Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_threadsafe_function_context` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_typedarray_info` | Yes | executes (own status; pending preserved) | `invalid_arg` if not TypedArray (impl `IsTypedArray`, NOT `object_expected`) | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_undefined` | Yes | executes (own status; pending preserved) | n/a | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_uv_event_loop` | Yes | executes (own status; pending preserved) | n/a | B4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_bigint_int64` | Yes | executes (own status; pending preserved) | `bigint_expected` if not BigInt (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_bigint_uint64` | Yes | executes (own status; pending preserved) | `bigint_expected` if not BigInt (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_bigint_words` | Yes | executes (own status; pending preserved) | `bigint_expected` if not BigInt (impl explicit; upstream docs say only `napi_ok` — impl wins) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_bool` | Yes | executes (own status; pending preserved) | `boolean_expected` if not boolean (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_double` | Yes | executes (own status; pending preserved) | `number_expected` if not Number (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_external` | Yes | executes (own status; pending preserved) | `invalid_arg` if not external (docs+impl agree; NOT `object_expected`) | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_int32` | Yes | executes (own status; pending preserved) | `number_expected` if not Number (docs+impl agree; truncation/`NaN→0` per docs) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_int64` | Yes | executes (own status; pending preserved) | `number_expected` if not Number (docs+impl agree; `±53` precision loss + non-finite→`0`) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_string_latin1` | Yes | executes (own status; pending preserved) | `string_expected` if not string (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_string_utf16` | Yes | executes (own status; pending preserved) | `string_expected` if not string (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_string_utf8` | Yes | executes (own status; pending preserved) | `string_expected` if not string (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_value_uint32` | Yes | executes (own status; pending preserved) | `number_expected` if not Number (docs+impl agree) | A1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_get_version` | Yes | executes (own status; pending preserved) | n/a | A6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_has_element` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_has_named_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_has_own_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject`; `name_expected` if `key` not Name (impl `IsName` — plan omits `name_expected`) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_has_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_instanceof` | No | `napi_pending_exception` (no execute) | `object_expected` if `constructor` fails `ToObject`; `function_expected` (+ throws `TypeError ERR_NAPI_CONS_FUNCTION`) if `constructor` is object but not function (only place `function_expected` is used) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_is_array` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` (not error) | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_arraybuffer` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_buffer` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | B2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_dataview` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_date` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_detached_arraybuffer` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` (impl checks both ArrayBuffer+Shared) | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_error` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A2/A4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_exception_pending` | Yes | executes (own status; pending preserved) | none | A4. Header: NO comment. Docs list callable-with-pending. |
| `napi_is_promise` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_is_typedarray` | Yes | executes (own status; pending preserved) | none — wrong type ⇒ `napi_ok`+`false` | A5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_make_callback` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `func` not function (impl `CHECK_TO_FUNCTION`); `object_expected` if `recv` fails `ToObject` | B3. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_module_register` | Yes | n/a (`void`; provider registry) | n/a (`void`, deprecated; no status) | B1. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). Deprecated; provider must implement pending-module registry (plan §1.3). |
| `napi_new_instance` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `constructor` not function (impl `CHECK_TO_FUNCTION`, NOT `function_expected`) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_object_freeze` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject`; `generic_failure` if `SetFrozen` fails (e.g. non-extensible proxy) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_object_seal` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject`; `generic_failure` if `SetSealed` fails | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_open_callback_scope` | Yes | executes (own status; pending preserved) | n/a (no JS type check; `invalid_arg` if `result==NULL`) | B3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_open_escapable_handle_scope` | Yes | executes (own status; pending preserved) | n/a | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_open_handle_scope` | Yes | executes (own status; pending preserved) | n/a | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_queue_async_work` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `work==NULL`) | B4. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_ref_threadsafe_function` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_reference_ref` | Yes | executes (own status; pending preserved) | none for type; collected-weak ⇒ impl still returns `napi_ok` (header says “error” — impl diverges; see AMB-REF-1 note) | A3. Header comment (js_native_api.h:342-346): “Calling this when the refcount is 0 and the object is unavailable results in an error.” No status named. Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_reference_unref` | Yes | executes (own status; pending preserved) | unref-at-0 ⇒ `generic_failure` (impl explicit `refcount()==0` check; header says “error” without naming code) | A3. Header comment (js_native_api.h:351-354): “Calling this when the refcount is already 0 results in an error.” No status named. Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_reject_deferred` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `deferred==NULL` (binding arg); `rejection` value itself any type (rejects with it) | A6. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_release_threadsafe_function` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_remove_async_cleanup_hook` | Yes | executes (own status; pending preserved) | n/a (no `env`; `invalid_arg` if `handle==NULL`) | B6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_remove_env_cleanup_hook` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `fun==NULL`) | B6. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_remove_wrap` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `js_object` not object or unwrapped/not-wrapped (impl `IsObject`+`IsExternal` private slot, NOT `object_expected`); `result==NULL` allowed (remove without retrieve) | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_resolve_deferred` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `deferred==NULL` (binding arg); `resolution` value itself any type | A6. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_run_script` | No | `napi_pending_exception` (no execute) | `string_expected` if `script` not string (impl explicit); compile/runtime throw ⇒ `pending_exception` via try-catch (see notes) | A6. Headers: silent (declaration only). Docs do not list (consistent with gated). After gate it may ALSO return `napi_pending_exception` if callee/script/coercion throws (try-catch → `GET_RETURN_STATUS`). |
| `napi_set_element` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_set_instance_data` | Yes | executes (own status; pending preserved) | n/a | A3. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_set_named_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_set_property` | No | `napi_pending_exception` (no execute) | `object_expected` if `object` fails `ToObject` | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_strict_equals` | No | `napi_pending_exception` (no execute) | none — any two values comparable (never type-error) | A2. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_throw` | No | `napi_pending_exception` (no execute) | none — any value throwable | A4. Headers: silent (declaration only). Docs do not list (consistent with gated). On success it SETS pending and returns `napi_ok`; if already pending it returns `napi_pending_exception` without re-throwing (plan §3.10, impl preamble). |
| `napi_throw_error` | No | `napi_pending_exception` (no execute) | none — `code`/`msg` are C strings (NULL allowed) | A4. Headers: silent (declaration only). Docs do not list (consistent with gated). On success it SETS pending and returns `napi_ok`; if already pending it returns `napi_pending_exception` without re-throwing (plan §3.10, impl preamble). |
| `napi_throw_range_error` | No | `napi_pending_exception` (no execute) | none — C strings (NULL allowed) | A4. Headers: silent (declaration only). Docs do not list (consistent with gated). On success it SETS pending and returns `napi_ok`; if already pending it returns `napi_pending_exception` without re-throwing (plan §3.10, impl preamble). |
| `napi_throw_type_error` | No | `napi_pending_exception` (no execute) | none — C strings (NULL allowed) | A4. Headers: silent (declaration only). Docs do not list (consistent with gated). On success it SETS pending and returns `napi_ok`; if already pending it returns `napi_pending_exception` without re-throwing (plan §3.10, impl preamble). |
| `napi_type_tag_object` | No | `napi_pending_exception` (no execute) | `object_expected` if fails `ToObject`; double-tag ⇒ `invalid_arg` (docs+impl agree) | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_typeof` | Yes | executes (own status; pending preserved) | none — any value has a `valuetype` (unreachable default ⇒ `invalid_arg` only for new V8 kinds) | A2. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_unref_threadsafe_function` | Yes | executes (own status; pending preserved) | n/a (no JS value; `invalid_arg` if `func==NULL`) | B5. Headers: silent (declaration only). Docs do NOT list as callable, but impl executes (divergence; see §0). |
| `napi_unwrap` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `js_object` not object or unwrapped (impl `IsObject`+`IsExternal`, NOT `object_expected`) | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |
| `napi_wrap` | No | `napi_pending_exception` (no execute) | `invalid_arg` if `js_object` not object or already wrapped (impl `IsObject`+private-slot check, NOT `object_expected`); `finalize_cb==NULL` required when `result!=NULL` | A3. Headers: silent (declaration only). Docs do not list (consistent with gated). |

## 2. Disagreements vs plan §3.10 (exact)

Plan §3.10 (lines 494-514) makes four testable claims. Checked against
A (headers), B (upstream docs), C (implementation). **Headers alone
prove none of these wrong** (headers make almost no claim), which is why
`doc/node-api-semantics.md` §6 is left untouched by this task (see §3).
Disagreements below are B/C-proven and need a plan amendment in a follow-up
once B/C are vendored — they are recorded here so implementers do not
treat §3.10 as settled.

1. **“Most APIs return `napi_pending_exception` without executing;
   exceptions per the vendored header docs.”** — *Wrong in both halves.*
   - A proves no exception list (silent). B's explicit list is 12
     callable, implying 133 gated (“most”). C's actual gate is 55 gated /
     90 execute — gated is the *minority*. 78 rows execute in C but are
     not listed in B. The “most” framing therefore matches neither source
     exactly. Provider must choose B (12) or C (90); §5 recommends C.
2. **“Type mismatch found by provider pre-check ⇒ specific
   `napi_*_expected` … produced by the provider, not by mapping JsRT's
   collapsed invalid-arg” + the 9-code list
   (`object/string/number/boolean/array/function/bigint/date/arraybuffer`)**.
   - B names a specific `*_expected` code in only 12 Returns statements
     (plus `invalid_arg` for `get_value_external`; see `type-mismatch`
     column). C uses a specific `*_expected` code in 58 of the 145 rows
     (`object_expected` dominates via `ToObject` coercion on ~26 object
     APIs; `string/number/boolean/bigint/date/array/name` on named
     getters; `function_expected` on `instanceof` only). For 16 functions
     C returns
     `napi_invalid_arg` on type mismatch where §3.10's “never blanket
     `invalid_arg`” implies a specific code: `create_reference`,
     `wrap`/`unwrap`/`remove_wrap`/`add_finalizer`, `get_value_external`,
     `get_arraybuffer_info`, `get_typedarray_info`, `get_dataview_info`,
     `get_buffer_info`, `create_typedarray`/`dataview` (ArrayBuffer check),
     `call_function`/`new_instance`/`make_callback`/`create_threadsafe`
     (function check via `CHECK_TO_FUNCTION` → `invalid_arg`).
   - §3.10 also **omits** `napi_name_expected` (C uses it in
     `has_own_property`, `define_properties`, `define_class`) and
     `napi_detachable_arraybuffer_expected` (C uses it in
     `detach_arraybuffer`; B names it). The 9-code list is incomplete.
   - `CHECK_TO_OBJECT` (`object_expected`) is a *coercion*, not a strict
     `IsObject` check: primitives box and succeed; only `null`/`undefined`
     (and failed coercions) produce `object_expected`. A provider pre-check
     using strict `JsGetValueType==Object` would be stricter than Node.
3. **“`JsErrorInExceptionState`, `JsErrorScriptException` ⇒
   `napi_pending_exception` (never cleared implicitly)” and
   “`JsErrorScriptCompile` (from `JsRun`) ⇒ `napi_pending_exception`” (§3.10
   table).** — *Unverifiable from headers; C-consistent but JsRT-specific.*
   C has no `JsErrorCode` mapping (it uses V8 try-catch → `GET_RETURN_STATUS`
   → `pending_exception`). The provider mapping is a design choice, not a
   Node-observed behaviour. No contradiction, but §3.10 should be labelled
   “provider design (JsRT mapping), not Node-documented” in the amendment.
4. **“Throw helpers … return `napi_ok` when no exception was already
   pending (if pending, return `napi_pending_exception` and leave it).”** —
   *Agrees with B+C and headers-by-omission.* C confirms: all four
   `napi_throw_*` + `napi_throw` use `NAPI_PREAMBLE` (gated). No change.

Additional B/C precision facts that refine (not contradict) §3.10:

- `napi_get_last_error_info` content is valid only until the *next* API call
  (B line “up until a Node-API function is called”). C clears `last_error`
  in every `NAPI_PREAMBLE` *and* every `napi_clear_last_error` success path.
  So a successful call **does** overwrite the previous error (see
  `AMB-STATUS-1` in §4). §3.10's “most recent failed call” is imprecise.
- `napi_call_function` / `napi_new_instance` / `napi_make_callback`
  callee-throw handling: C returns `napi_pending_exception` via try-catch
  (`CHECK_MAYBE_EMPTY_WITH_PREAMBLE`); `result` out-param is only written
  on success. §6.5's “never cleared implicitly” agrees.

## 3. `†N0.6-B1..B5` mapping (semantics §6)

Headers alone decide **none** of B1–B5 (all silent). Answers below are
B+C-proven and are the **provider recommendation** for N1.5. Per task
rules, `doc/node-api-semantics.md` §6 is **not amended** (amendment
requires header proof). §6 lines stay provisional with their current probe
IDs; probes must assert the C column below and record the amendment as
follow-up work once B/C are vendored.

| Row | Function | Header (A) | Upstream docs (B) | Implementation (C) | Provider answer (N1.5) | Probe(s) |
|---|---|---|---|---|---|---|
| `†N0.6-B1` | `napi_create_double` pending gate (§6.1) | silent | silent (not in 12; global “many return pending” does not name it) | **executes** (`CHECK_ENV_NOT_IN_GC` only; `v8::Number::New` cannot throw) | `Yes` — executes with pending; returns `napi_ok` + rooted double; pending preserved. §6.1's “gated” choice is B/C-wrong if C is followed (see §2 item 1). | P5-2b + P6-1a |
| `†N0.6-B2` | `napi_get_value_int32` gate + code (§6.2) | silent | code: **`napi_number_expected`** (Returns line, explicit); gate: silent | **executes** + **`number_expected`** (`IsNumber` check, `napi_clear_last_error`) | `Yes` + `number_expected`. §6.2's `number_expected` is B+C-correct; its assumed gate is C-wrong. `NaN`/`±Inf`⇒`0`, `>2^31-1` truncates (B). | P6-2a/b/c |
| `†N0.6-B3` | `napi_wrap` gate + codes (§6.3) | silent | silent (Returns `napi_ok` only; no double-wrap code named) | **gated** (via `Wrap` helper `NAPI_PREAMBLE`) + **`invalid_arg`** for non-object *and* double-wrap (impl `IsObject` + private-slot check) | `No` (gated) + `invalid_arg` (both). §6.3's gate is C-correct; its “non-object ⇒ `object_expected`” is C-wrong (C uses `invalid_arg`; `wrap` does *not* `ToObject`-coerce). | P5-2 + P7-1/8 |
| `†N0.6-B4` | `napi_delete_reference` allowlist + precedence (§6.4) | comment: delete/released/GC, no pending statement | **callable-with-pending** (explicit line) | **executes** (`CHECK_ENV` only, `delete` + `napi_clear_last_error`); never inspects exception state, so “allowlist wins over `InExceptionState`” holds trivially (delete always runs) | `Yes` + `napi_ok` (or `invalid_arg` iff `ref==NULL`); pending preserved. §6.4 is B+C-correct. | P5-3 + P3-4/5 |
| `†N0.6-B5` | `napi_call_function` gate + codes (§6.5) | silent | silent (Returns `napi_ok` only) | **gated** (`NAPI_PREAMBLE`) + **`invalid_arg`** if `func` not function (`CHECK_TO_FUNCTION`), **`object_expected`** if `recv` fails `ToObject`; callee-throw ⇒ `pending_exception` via try-catch | `No` + `invalid_arg`/`object_expected` as tabled. §6.5's gate is C-correct; its “non-function ⇒ `function_expected`” is C-wrong (only `instanceof` uses `function_expected`). | P5-2 + P6-3 + P8-1/3 |

**Change note (no edit made):** had headers contained B/C evidence,
§6.1 / §6.2-gate / §6.3-non-object-code / §6.5-function-code would have
required amendments. They do not, so §6 is untouched. The follow-up
amendment (once B/C are vendored) should: flip §6.1 creation to
allowlisted-execute, flip §6.2 to execute (keep `number_expected`),
flip §6.3 non-object to `invalid_arg`, flip §6.5 non-function to
`invalid_arg`, and add the 55/90 gate list to §6.

## 4. Ambiguity register — resolved vs still open

Headers decide **one** of the five assigned flags fully
(`AMB-REF-3` is not in the assigned list but is recorded for completeness).
The rest are header-silent; B/C evidence is given so the deciding probe is
unambiguous, but per task rules they remain **still-ambiguous from headers**
until B/C are vendored or the probe runs.

| Flag | Header (A) verdict | B+C evidence (for probe design) | Status | Deciding probe |
|---|---|---|---|---|
| `AMB-STATUS-1` (success clears `last_error`?) (§6 + App. B) | silent | B: content valid “until a Node-API function is called” (any call). C: `NAPI_PREAMBLE` + every success path calls `napi_clear_last_error` → success clears. Semantics spec choice (“leaves untouched”) is B/C-wrong. | **Still-ambiguous from headers; B/C-proven: success clears.** | P5-4: fail (e.g. `get_value_int32` on string ⇒ `number_expected`), then succeed (e.g. `create_double`), then `get_last_error_info` ⇒ `error_code==napi_ok`. |
| `AMB-SC-1` (`escape` on non-escapable/NULL scope status) (§2) | silent (59 scope comments absent; enum has `escape_called_twice` only) | C: **no check** — casts scope pointer to `EscapableHandleScopeWrapper` and reads `escape_called_` (UB on plain scope; no code). B: silent. | **Still-ambiguous (headers + B + C all silent/UB).** Provider must choose; semantics `invalid_arg` is a provider *design* decision, not Node-observed. | P2-4b (new): escape on plain scope + NULL scope; assert provider's chosen `invalid_arg` and no root move; vendored handle-scope tests as cross-check. |
| `AMB-SC-2` (close-escapable on plain scope: `mismatch` vs `invalid_arg`?) (§2) | silent | C: **single shared `open_handle_scopes` counter**, no kind flag — close-escapable on plain with count>0 *succeeds* in C (wrong-type `delete`, UB); with count==0 both return `handle_scope_mismatch`. B: “reverse order” only, no code. | **Still-ambiguous. C cannot distinguish kinds.** Semantics `mismatch` is only C-true for the empty-stack case; provider must track kind to implement it generally. | P2-5b: close-escapable on plain (count>0) + empty-stack close; assert two distinct behaviours if provider tracks kind, else record shared-counter choice. |
| `AMB-REF-1` (unref-when-0: `ok` vs `invalid_arg`?) (§3, R-REF-6) | **partially decides**: “results in an error” for both `ref`-on-collected and `unref`-at-0 — rules out semantics `napi_ok`-no-op, but **names no code** | C: `unref` at 0 ⇒ **`napi_generic_failure`** (explicit check). `ref` on collected (empty persistent) ⇒ **`napi_ok`** with count 0 (impl *no* error — diverges from header “error”). B: `ref`-on-collected “results in an error” (agree with header, disagree with C). | **Still-ambiguous on exact code from headers; B/C-proven split** (unref=`generic_failure`, ref-collected= header/docs error vs C ok). | P3-1 extension: `unref` at 0 ⇒ assert `generic_failure`; `ref` on collected ⇒ decide ok vs error against vendored `test_reference` + B (follow B/header, note C divergence as Node bug). |
| `AMB-CB-2` (throw-with-return-value: rooted or discarded?) (§5) | silent (no callback comments) | C: **discarded** (`FunctionCallbackWrapper::InvokeCallback`: only `Set` return if `!exceptionOccurred`). B: silent. | **Still-ambiguous from headers; B/C-proven: discarded (exception wins).** Semantics choice already correct. | P8-3b: callback returns non-NULL *and* throws; assert JS caller observes the thrown value (not the return) and return value not rooted in caller scope. |
| `AMB-REF-3` (collected-weak out-param: NULL-written vs untouched?) (§3) — *for completeness* | **DECIDED**: `js_native_api.h:359-361` — “still successful but the result is NULL” | B+C agree (explicit NULL). | **Resolved by headers.** P3-2 sentinel assertion satisfies N0.6; provider MUST write NULL. | P3-2 (sentinel) |

## 5. Recommendations for N1.5 (test list per row)

- One probe per table row (145 probes, grouped by plan task). Minimal
  N1.5 set: for each `No` row assert entry-gate `pending_exception` +
  no execution (e.g. create/wrap/call allocate nothing, throw helpers do
  not re-throw); for each `Yes` row assert execution + pending preserved
  (e.g. create/scope/ref/get_value still succeed while pending, except
  `throw` family which gates). §6 probes P5-2/P5-2b/P5-3/P5-4 remain the
  index entries; addendum probes: P5-2c (78 B-silent/C-execute rows bite`),
  P5-5 (type-mismatch code per row, especially `*_expected` vs
  `invalid_arg` split in §2 item 2), P5-6 (`name_expected`/`detachable` codes
  omitted from §3.10).
- **Gate choice:** implement C's 55/90 split (see §0). Gating everything
  per §3.10's “most” would reject 78 executions Node permits (e.g.
  `get_value_*`, `is_*`, `create_*` with pending) and break addons that
  inspect values during cleanup (the exact use case B's “minimal cleanup”
  sentence protects).
- **Vendoring gap (N0.5/N0.1 follow-up):** vendor `doc/api/n-api.md`
  v22.22.3 (or at least its Error handling + per-function Returns +
  12 callable lines) and pin C file paths/hashes, then amend plan §3.10
  and semantics §6 in the same change (plan §4 rule: generated inventory
  wins; same principle applies to status mapping). Until then, this file
  is the single source for N1.5 and MUST be second-reader checked
  (Appendix B).
- JsRT mapping caution: `napi_object_expected` via `ToObject` coerces
  primitives — provider pre-check must replicate coercion (box), not
  strict `IsObject`. `CHECK_TO_FUNCTION` → `invalid_arg` (not
  `function_expected`) must be replicated unless implementers deliberately
  diverge and record it.

## Appendix A. Header-comment inventory (proof of silence)

Vendored comments assigned to declarations (`js_native_api.h` unless noted):
- §“Getters for defined singletons” / “Methods to create …” / “Methods to get …” /
  “Methods to coerce …” / “Methods to work with Objects/Arrays/compare/Functions/
  callbacks/external/lifespan/errors/catching/binary/version/Promises/script/
  memory/Dates/BigInt/Object/Instance data/detaching/type tagging” — section
  banners only, no semantics.
- `:282` “Gets all callback info … (Ugly, but faster.)” + `:284-290`
  `get_cb_info` param notes (argc/argv/this/data). No pending/code.
- `:158,162,166` string-copy notes (“Copies LATIN-1/UTF-8/UTF-16 … into a
  buffer”). No codes.
- `:174` “These APIs may execute user scripts.” (coerce paragraph).
- `:330` “Set initial_refcount to 0 for a weak reference …” (`create_reference`).
- `:337-338` delete-reference GC note; `:342-346` ref-increment + “error” (no code);
  `:351-354` unref-decrement + “error” (no code); `:359-361` get-ref NULL success.
- `js_native_api_types.h:41-60` `node_api_basic_env` GC-safety paragraph
  (may/must-not-call during GC; NOT a pending-exception statement).
- `js_native_api_types.h:151-157` deferral to `js_native_api_v8.cc` error
  strings + `doc/api/n-api.md` (proof that headers are not the status oracle).
- `js_native_api_types.h:125-150` `napi_status` enum (membership only).
- `node_api.h` comments: registration deprecation, buffer/async/uv/TSFN
  banners, `uv_loop` return note. **Zero pending/`*_expected` comments.**

Total per-function semantic comments: 7 declarations (refs ×4, coerce ×1,
`get_cb_info` ×1, `create_reference` ×1 — the latter two counted in refs
family). All other 138/145 declarations have **no comment**.

## Appendix B. Second-reader checklist (N0.6 Done-when)

Second reader MUST verify 10 random rows against this file's cited sources:

- [ ] `napi_get_value_int32` — B `number_expected` Returns; C executes (`IsNumber`).
- [ ] `napi_wrap` — C gated via helper; `invalid_arg` (not `object_expected`).
- [ ] `napi_delete_reference` — B + C callable-pending.
- [ ] `napi_call_function` — C gated; `invalid_arg` (not `function_expected`).
- [ ] `napi_escape_handle` — B + C callable; `escape_called_twice` second call.
- [ ] `napi_close_handle_scope` (empty-stack) — C `handle_scope_mismatch`.
- [ ] `napi_reference_unref` at 0 — C `generic_failure` vs header “error”.
- [ ] `napi_get_last_error_info` — B validity window; C `clear_last_error`.
- [ ] `napi_get_array_length` — C `array_expected`.
- [ ] `napi_detach_arraybuffer` — B+C `detachable_arraybuffer_expected`.

Checks: (a) table has 145 data rows, columns exact; (b) 55 `No` / 90 `Yes`;
(c) §6 untouched (re-verify per task rule); (d) counts recompute from table;
(e) upstream fetch versions are v22.22.3 (headers + n-api.md + .cc).

