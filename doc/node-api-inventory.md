# Node-API inventory (generated)

Generated from the vendored Node 22.22.3 headers in
`include/nodeapi/` at `NAPI_VERSION=8` by
`tools/nodeapi/gen_inventory.py`. Do not edit by hand —
regenerate with `python3 tools/nodeapi/gen_inventory.py`.
Normative source: `doc/node-api-plan.md` §4 (plan §1.1 counts).

## Summary

- `js_native_api.h`: 115 symbols visible at `NAPI_VERSION=8` (126 total declarations; 11 excluded: 3 experimental, 3 v9-only, 5 v10-only).
- `node_api.h`: 30 symbols visible at `NAPI_VERSION=8` (32 total declarations; 2 excluded: 1 v9-only, 1 v10-only).
- Provider target at v8: **145 symbols** (expected 115 + 30 = 145).

Visibility rule: a declaration is listed iff it is outside `NAPI_EXPERIMENTAL` blocks and outside `#if NAPI_VERSION >= 9/10` blocks, with `NAPI_EXPERIMENTAL` undefined and `NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED` undefined (the provider never defines it, so `napi_create_external_arraybuffer` and `napi_create_external_buffer` are included). `#if NAPI_VERSION >= 2/3/4/5/6/7/8` blocks are included at v8.

## Inventory (145 symbols at `NAPI_VERSION=8`)

| Symbol | Header | Group |
|---|---|---|
| `napi_acquire_threadsafe_function` | `node_api.h` | B5 |
| `napi_add_async_cleanup_hook` | `node_api.h` | B6 |
| `napi_add_env_cleanup_hook` | `node_api.h` | B6 |
| `napi_add_finalizer` | `js_native_api.h` | A3 |
| `napi_adjust_external_memory` | `js_native_api.h` | A6 |
| `napi_async_destroy` | `node_api.h` | B3 |
| `napi_async_init` | `node_api.h` | B3 |
| `napi_call_function` | `js_native_api.h` | A2 |
| `napi_call_threadsafe_function` | `node_api.h` | B5 |
| `napi_cancel_async_work` | `node_api.h` | B4 |
| `napi_check_object_type_tag` | `js_native_api.h` | A3 |
| `napi_close_callback_scope` | `node_api.h` | B3 |
| `napi_close_escapable_handle_scope` | `js_native_api.h` | A3 |
| `napi_close_handle_scope` | `js_native_api.h` | A3 |
| `napi_coerce_to_bool` | `js_native_api.h` | A2 |
| `napi_coerce_to_number` | `js_native_api.h` | A2 |
| `napi_coerce_to_object` | `js_native_api.h` | A2 |
| `napi_coerce_to_string` | `js_native_api.h` | A2 |
| `napi_create_array` | `js_native_api.h` | A2 |
| `napi_create_array_with_length` | `js_native_api.h` | A2 |
| `napi_create_arraybuffer` | `js_native_api.h` | A5 |
| `napi_create_async_work` | `node_api.h` | B4 |
| `napi_create_bigint_int64` | `js_native_api.h` | A1 |
| `napi_create_bigint_uint64` | `js_native_api.h` | A1 |
| `napi_create_bigint_words` | `js_native_api.h` | A1 |
| `napi_create_buffer` | `node_api.h` | B2 |
| `napi_create_buffer_copy` | `node_api.h` | B2 |
| `napi_create_dataview` | `js_native_api.h` | A5 |
| `napi_create_date` | `js_native_api.h` | A1 |
| `napi_create_double` | `js_native_api.h` | A1 |
| `napi_create_error` | `js_native_api.h` | A4 |
| `napi_create_external` | `js_native_api.h` | A3 |
| `napi_create_external_arraybuffer` | `js_native_api.h` | A5 |
| `napi_create_external_buffer` | `node_api.h` | B2 |
| `napi_create_function` | `js_native_api.h` | A2 |
| `napi_create_int32` | `js_native_api.h` | A1 |
| `napi_create_int64` | `js_native_api.h` | A1 |
| `napi_create_object` | `js_native_api.h` | A2 |
| `napi_create_promise` | `js_native_api.h` | A6 |
| `napi_create_range_error` | `js_native_api.h` | A4 |
| `napi_create_reference` | `js_native_api.h` | A3 |
| `napi_create_string_latin1` | `js_native_api.h` | A1 |
| `napi_create_string_utf16` | `js_native_api.h` | A1 |
| `napi_create_string_utf8` | `js_native_api.h` | A1 |
| `napi_create_symbol` | `js_native_api.h` | A1 |
| `napi_create_threadsafe_function` | `node_api.h` | B5 |
| `napi_create_type_error` | `js_native_api.h` | A4 |
| `napi_create_typedarray` | `js_native_api.h` | A5 |
| `napi_create_uint32` | `js_native_api.h` | A1 |
| `napi_define_class` | `js_native_api.h` | A2 |
| `napi_define_properties` | `js_native_api.h` | A2 |
| `napi_delete_async_work` | `node_api.h` | B4 |
| `napi_delete_element` | `js_native_api.h` | A2 |
| `napi_delete_property` | `js_native_api.h` | A2 |
| `napi_delete_reference` | `js_native_api.h` | A3 |
| `napi_detach_arraybuffer` | `js_native_api.h` | A5 |
| `napi_escape_handle` | `js_native_api.h` | A3 |
| `napi_fatal_error` | `node_api.h` | B1 |
| `napi_fatal_exception` | `node_api.h` | B1 |
| `napi_get_all_property_names` | `js_native_api.h` | A2 |
| `napi_get_and_clear_last_exception` | `js_native_api.h` | A4 |
| `napi_get_array_length` | `js_native_api.h` | A2 |
| `napi_get_arraybuffer_info` | `js_native_api.h` | A5 |
| `napi_get_boolean` | `js_native_api.h` | A1 |
| `napi_get_buffer_info` | `node_api.h` | B2 |
| `napi_get_cb_info` | `js_native_api.h` | A2 |
| `napi_get_dataview_info` | `js_native_api.h` | A5 |
| `napi_get_date_value` | `js_native_api.h` | A1 |
| `napi_get_element` | `js_native_api.h` | A2 |
| `napi_get_global` | `js_native_api.h` | A1 |
| `napi_get_instance_data` | `js_native_api.h` | A3 |
| `napi_get_last_error_info` | `js_native_api.h` | A4 |
| `napi_get_named_property` | `js_native_api.h` | A2 |
| `napi_get_new_target` | `js_native_api.h` | A2 |
| `napi_get_node_version` | `node_api.h` | B7 |
| `napi_get_null` | `js_native_api.h` | A1 |
| `napi_get_property` | `js_native_api.h` | A2 |
| `napi_get_property_names` | `js_native_api.h` | A2 |
| `napi_get_prototype` | `js_native_api.h` | A2 |
| `napi_get_reference_value` | `js_native_api.h` | A3 |
| `napi_get_threadsafe_function_context` | `node_api.h` | B5 |
| `napi_get_typedarray_info` | `js_native_api.h` | A5 |
| `napi_get_undefined` | `js_native_api.h` | A1 |
| `napi_get_uv_event_loop` | `node_api.h` | B4 |
| `napi_get_value_bigint_int64` | `js_native_api.h` | A1 |
| `napi_get_value_bigint_uint64` | `js_native_api.h` | A1 |
| `napi_get_value_bigint_words` | `js_native_api.h` | A1 |
| `napi_get_value_bool` | `js_native_api.h` | A1 |
| `napi_get_value_double` | `js_native_api.h` | A1 |
| `napi_get_value_external` | `js_native_api.h` | A3 |
| `napi_get_value_int32` | `js_native_api.h` | A1 |
| `napi_get_value_int64` | `js_native_api.h` | A1 |
| `napi_get_value_string_latin1` | `js_native_api.h` | A1 |
| `napi_get_value_string_utf16` | `js_native_api.h` | A1 |
| `napi_get_value_string_utf8` | `js_native_api.h` | A1 |
| `napi_get_value_uint32` | `js_native_api.h` | A1 |
| `napi_get_version` | `js_native_api.h` | A6 |
| `napi_has_element` | `js_native_api.h` | A2 |
| `napi_has_named_property` | `js_native_api.h` | A2 |
| `napi_has_own_property` | `js_native_api.h` | A2 |
| `napi_has_property` | `js_native_api.h` | A2 |
| `napi_instanceof` | `js_native_api.h` | A2 |
| `napi_is_array` | `js_native_api.h` | A2 |
| `napi_is_arraybuffer` | `js_native_api.h` | A5 |
| `napi_is_buffer` | `node_api.h` | B2 |
| `napi_is_dataview` | `js_native_api.h` | A5 |
| `napi_is_date` | `js_native_api.h` | A2 |
| `napi_is_detached_arraybuffer` | `js_native_api.h` | A5 |
| `napi_is_error` | `js_native_api.h` | A2, A4 |
| `napi_is_exception_pending` | `js_native_api.h` | A4 |
| `napi_is_promise` | `js_native_api.h` | A6 |
| `napi_is_typedarray` | `js_native_api.h` | A5 |
| `napi_make_callback` | `node_api.h` | B3 |
| `napi_module_register` | `node_api.h` | B1 |
| `napi_new_instance` | `js_native_api.h` | A2 |
| `napi_object_freeze` | `js_native_api.h` | A2 |
| `napi_object_seal` | `js_native_api.h` | A2 |
| `napi_open_callback_scope` | `node_api.h` | B3 |
| `napi_open_escapable_handle_scope` | `js_native_api.h` | A3 |
| `napi_open_handle_scope` | `js_native_api.h` | A3 |
| `napi_queue_async_work` | `node_api.h` | B4 |
| `napi_ref_threadsafe_function` | `node_api.h` | B5 |
| `napi_reference_ref` | `js_native_api.h` | A3 |
| `napi_reference_unref` | `js_native_api.h` | A3 |
| `napi_reject_deferred` | `js_native_api.h` | A6 |
| `napi_release_threadsafe_function` | `node_api.h` | B5 |
| `napi_remove_async_cleanup_hook` | `node_api.h` | B6 |
| `napi_remove_env_cleanup_hook` | `node_api.h` | B6 |
| `napi_remove_wrap` | `js_native_api.h` | A3 |
| `napi_resolve_deferred` | `js_native_api.h` | A6 |
| `napi_run_script` | `js_native_api.h` | A6 |
| `napi_set_element` | `js_native_api.h` | A2 |
| `napi_set_instance_data` | `js_native_api.h` | A3 |
| `napi_set_named_property` | `js_native_api.h` | A2 |
| `napi_set_property` | `js_native_api.h` | A2 |
| `napi_strict_equals` | `js_native_api.h` | A2 |
| `napi_throw` | `js_native_api.h` | A4 |
| `napi_throw_error` | `js_native_api.h` | A4 |
| `napi_throw_range_error` | `js_native_api.h` | A4 |
| `napi_throw_type_error` | `js_native_api.h` | A4 |
| `napi_type_tag_object` | `js_native_api.h` | A3 |
| `napi_typeof` | `js_native_api.h` | A2 |
| `napi_unref_threadsafe_function` | `node_api.h` | B5 |
| `napi_unwrap` | `js_native_api.h` | A3 |
| `napi_wrap` | `js_native_api.h` | A3 |

## Excluded at `NAPI_VERSION=8` (for audit)

| Symbol | Header | Reason |
|---|---|---|
| `node_api_create_buffer_from_arraybuffer` | `node_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_external_string_latin1` | `js_native_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_external_string_utf16` | `js_native_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_property_key_latin1` | `js_native_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_property_key_utf16` | `js_native_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_property_key_utf8` | `js_native_api.h` | v10-only (#if NAPI_VERSION >= 10) |
| `node_api_create_sharedarraybuffer` | `js_native_api.h` | #ifdef NAPI_EXPERIMENTAL |
| `node_api_create_syntax_error` | `js_native_api.h` | v9-only (#if NAPI_VERSION >= 9) |
| `node_api_get_module_file_name` | `node_api.h` | v9-only (#if NAPI_VERSION >= 9) |
| `node_api_is_sharedarraybuffer` | `js_native_api.h` | #ifdef NAPI_EXPERIMENTAL |
| `node_api_post_finalizer` | `js_native_api.h` | #ifdef NAPI_EXPERIMENTAL |
| `node_api_symbol_for` | `js_native_api.h` | v9-only (#if NAPI_VERSION >= 9) |
| `node_api_throw_syntax_error` | `js_native_api.h` | v9-only (#if NAPI_VERSION >= 9) |
