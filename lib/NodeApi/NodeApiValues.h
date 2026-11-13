//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API scalar + string values (plan doc/node-api-plan.md tasks N2.1/N2.2/N2.6;
// semantics doc/node-api-semantics.md section 6; status mapping doc/node-api-status-mapping.md).
//
// Implements the v8 A1 scalar/string/coercion surface:
//   napi_get_undefined / napi_get_null / napi_get_global / napi_get_boolean,
//   napi_create_double / napi_create_int32 / napi_create_uint32 / napi_create_int64,
//   napi_get_value_double / napi_get_value_int32 / napi_get_value_uint32 /
//   napi_get_value_int64 / napi_get_value_bool,
//   napi_create_string_latin1 / napi_create_string_utf8 / napi_create_string_utf16,
//   napi_get_value_string_latin1 / napi_get_value_string_utf8 /
//   napi_get_value_string_utf16,
//   (N2.6) napi_typeof / napi_coerce_to_bool / napi_coerce_to_number /
//   napi_coerce_to_object / napi_coerce_to_string / napi_strict_equals /
//   napi_is_array / napi_is_typedarray / napi_is_dataview /
//   napi_is_arraybuffer / napi_is_date / napi_create_symbol /
//   napi_create_date / napi_get_date_value / napi_create_bigint_int64 /
//   napi_create_bigint_uint64 / napi_create_bigint_words /
//   napi_get_value_bigint_int64 / napi_get_value_bigint_uint64 /
//   napi_get_value_bigint_words.
// N2.6 notes: napi_is_error is implemented in NodeApiErrorsImpl.*
// (N2.5) and napi_instanceof in NodeApiObjects.* (N2.3); neither is
// duplicated here. napi_symbol_for / napi_create_named_function do not
// exist at v8 (plan section 4.1 note) and are not implemented.
//
// All napi_* symbols are declared in the vendored include/nodeapi/js_native_api.h
// (NAPI_VERSION=8); this header only records the lane's contract. The provider
// calls only the public JsRT C API (ChakraCommon.h / ChakraCore.h).
//
// Conversion semantics (Node v22.22.3 doc/api/n-api.md + src/js_native_api_v8.cc,
// which doc/node-api-status-mapping.md columns B/C cite; vendored headers are
// silent on these except the string-copy banners and the AUTO_LENGTH macro):
//   - Creates via double: create_int64 notes loss of precision outside
//     +-(2**53-1) (doc napi_create_int64; Node Number::New(double)).
//     create_int32/uint32/double are exact in double for their ranges.
//   - Getters pre-check the JsGetValueType and return the specific
//     number_expected / boolean_expected / string_expected (doc Returns
//     statements + impl IsNumber/IsBoolean/IsString). Out-param NULL is
//     napi_invalid_arg with no write (impl CHECK_ARG). For string gets the
//     Node rule is conditional: buf==NULL => result is required (size
//     query); buf!=NULL => result is optional (impl conditionals there).
//   - napi_get_value_int32: non-finite => 0; otherwise truncate toward
//     zero then keep the bottom 32 bits (doc: "If the number exceeds the
//     range ... truncated to the equivalent of the bottom 32 bits"; impl
//     Int32Value). napi_get_value_int64: non-finite => 0; outside
//     +-(2**53-1) loses precision (doc); impl special-cases non-finite to 0
//     and otherwise IntegerValue (saturating, not wrapping - see .cpp).
//     napi_get_value_uint32: impl Uint32Value = ToUint32 (non-finite => 0,
//     trunc, modulo 2**32; doc names only number_expected, so the truncation
//     rule is the impl's, recorded here).
//   - napi_get_value_string_*: buf==NULL => size query (result required);
//     buf!=NULL && bufsize==0 => no write, result 0 if supplied; otherwise
//     copy bufsize-1 units/bytes, null-terminate, result = copied count.
//     UTF-8 truncated copies never split a multi-byte sequence (V8
//     WriteUtf8 "Will not write partial UTF-8 sequences"; Chakra
//     EncodeInto asserts instead of truncating, so the provider copies
//     via a full-length temporary and truncates manually - see .cpp).
//   - Latin1: ISO-8859-1. Create maps each byte 0..255 to U+0000..U+00FF
//     via UTF-16. Get maps each UTF-16 code unit's low byte (truncation),
//     matching V8 WriteOneByte (differential: U+0100 -> 0x00; U+1F600
//     surrogates D83D DE00 -> 0x3D 0x00). JsCopyStringOneByte is NOT used
//     (plan N2.2: transcode explicitly through UTF-16).
//   - NAPI_AUTO_LENGTH (SIZE_MAX) is resolved provider-side: strlen for
//     char* (bytes) and wcslen-equivalent for char16_t* (code units until
//     NUL). Explicit lengths allow embedded NULs. Length > INT_MAX (and
//     not AUTO) => napi_invalid_arg (impl CHECK_NEW_STRING_ARGS).
//
// Pending-exception discipline: the N2.1/N2.2 functions (19) are C-column Yes
// (CHECK_ENV_NOT_IN_GC, doc/node-api-status-mapping.md table): they
// execute with a pending exception and preserve it. N2.6 follows the same
// table per function: Yes (execute with pending) = napi_typeof,
// napi_create_symbol, napi_create_bigint_int64/uint64,
// napi_get_value_bigint_int64/uint64/words, napi_is_array /
// napi_is_typedarray / napi_is_dataview / napi_is_arraybuffer / napi_is_date;
// No (gated: napi_pending_exception without executing) = napi_coerce_to_*,
// napi_strict_equals, napi_create_date, napi_get_date_value,
// napi_create_bigint_words. The shared nodeapi::EntryGuard is constructed
// with allowPending per that column, allowClosing=false, needContext=true.
// Last-error: success clears (AMB-STATUS-1, except
// napi_get_last_error_info itself which is N1.1); failure sets it.
//
// N2.6 semantics (Node v22.22.3 doc/api/n-api.md + src/js_native_api_v8.cc,
// plus live Node v26.5.0 differential recorded in the .cpp):
//   - napi_typeof: JsGetValueType maps Undefined/Null/Number/String/
//     Boolean/Symbol/Function directly; Error/Array/ArrayBuffer/
//     TypedArray/DataView all report napi_object (Node's typeof). For
//     JsObject, discrimination is: JsHasExternalData true =>
//     napi_external (a JsCreateExternalObject value, including NULL data:
//     the check is external-object-ness, not data non-null), else the
//     Object.prototype.toString tag "[object BigInt]" AND
//     JsConvertValueToObject identity (MED-1 fix: fresh wrapper =>
//     primitive BigInt => napi_bigint; same object => boxed
//     Object(1n) => napi_object, as Node reports; the tag alone
//     cannot split them). PR-9: JsGetValueType reports a BigInt
//     primitive as JsObject — verified empirically. Residuals of the
//     D8 toString path (PR-9, known): (a) a tampered
//     Object.prototype.toString or global Object breaks the tag check
//     (Node uses V8 internals); (b) a Proxy of an array reports
//     napi_object from typeof and false from napi_is_array
//     (JsGetValueType gives JsObject for proxies; Node's IsArray
//     unwraps). A proxy of an external reports napi_object
//     (JsHasExternalData does not expose the target in this build),
//     matching Node.
//   - Coercions use JsRT's JsConvertValueTo{Boolean,Number,String,Object}
//     directly (engine's specification order for valueOf/toString is
//     therefore preserved: to_number tries valueOf then toString;
//     to_string the reverse). napi_coerce_to_object(null/undefined) is
//     special-cased: it runs the conversion to obtain the engine's
//     pending TypeError and returns napi_object_expected, leaving the
//     TypeError pending — Node-differential verified (status
//     napi_object_expected with a pending exception). A synthesized
//     TypeError covers engines that fail to leave pending.
//     Symbol conversions throw TypeError for to_number/to_string
//     (pending preserved); to_object boxes symbols. to_bool never calls
//     user code (objects are true).
//   - napi_strict_equals is JsStrictEquals (===): NaN never equal,
//     +0/-0 equal, 1n !== 1.
//   - napi_create_symbol: description NULL => Symbol() via the Symbol
//     intrinsic, NOT JsCreateSymbol(NULL) (the JsRT NULL path yields the
//     empty-string description). Non-NULL description must be a
//     string, else napi_string_expected (JsCreateSymbol would ToString
//     it; the provider pre-checks). The NULL path calls JS (Symbol())
//     and this build tolerates the pending exception there
//     (engine-verified), so the C-Yes row is honored.
//     ENGINE DEVIATION (verified via script probe +
//     lib/Runtime/Library/JavascriptSymbol.cpp EntrySymbol /
//     EntryDescription): Chakra stores the EMPTY STRING as the
//     description for Symbol() and its description getter returns ""
//     for it, so `.description === undefined` is unreachable in this
//     build (spec/Node: Symbol().description === undefined while
//     Symbol('').description === ""). The intrinsic delegation is still
//     the faithful route (provider == Symbol() once the engine is
//     fixed); the vendored test_symbol.c never asserts .description,
//     so this deviation has no vendored-conformance impact. The probe
//     asserts the engine truth (NULL-desc description === "").
//   - Date: create via JsConstructObject on the global Date constructor
//     (NaN/Infinity => Invalid Date, fractional times truncate — engine
//     semantics, matching the Date constructor); get via the
//     Date.prototype getTime call (NaN for Invalid Date); is_date /
//     get_date_value's date_expected pre-check via a prototype-chain
//     walk to the context's Date.prototype (N2.5 pattern: no user code,
//     unlike JsInstanceOf). Wrong-type get_date_value =>
//     napi_date_expected. Tampered Date/getTime globals degrade
//     (is_date false / generic_failure) — documented PR-9 limit.
//   - BigInt (PR-9 string-mediated), Node-differential verified
//     (test/nodeapi/probes/coerce_probe.cpp + a standalone addon run
//     under node v26.5.0): creation via BigInt(decimalString) intrinsic
//     (int64/uint64 via std::to_string; words via little-endian-limb to
//     decimal conversion with sign_bit). Gets via ToString conversion +
//     decimal parse into little-endian limbs. int64/uint64 result is the
//     low 64 bits modulo 2^64 on every input (lossy cases wrap two's
//     complement); lossless is true iff the value fits (int64: -2^63 <=
//     v < 2^63; uint64: 0 <= v < 2^64). lossless and result are both
//     required (NULL => napi_invalid_arg, no write); zero => (0, true).
//     Words get: *word_count is always the REQUIRED count (0 for 0n, 1
//     for 1n, 2 for 2^64) and sign_bit the sign (0 for zero); words may
//     be NULL (size/sign query) and sign_bit may be NULL, but
//     word_count is required (NULL => napi_invalid_arg). A short buffer
//     copies min(required, provided) least-significant limbs, no error.
//     Words create: words is required (NULL => napi_invalid_arg, even
//     for count 0 — Node differential), word_count > INT_MAX =>
//     napi_invalid_arg (before any allocation), word_count > 16384
//     (conversion-cap; see .cpp) => pending RangeError
//     "Maximum BigInt size exceeded" (V8-matching message; the cap is a
//     documented v1 limit of the O(n^2) string path). Non-bigint input
//     to any get => napi_bigint_expected (JsGetValueType != JsObject,
//     external, tag mismatch, or boxed — MED-1 fix: a boxed BigInt
//     Object(1n) fails the ToObject-identity check and is rejected).
//   - napi_instanceof (N2.3, NodeApiObjects.cpp) and napi_is_error
//     (N2.5, NodeApiErrorsImpl.cpp) are not implemented here; the probe
//     links all three TUs so a duplicate-symbol break would fail at link
//     time.
//
// Rooting (R1): every returned napi_value is rooted into the innermost
// open scope via NodeApiScopes_RootIntoInnermost before the call returns.
// On rooting failure the value is not published (out-param untouched).
// Number/boolean/string getters return no napi_value and take no root.
//
// Ownership record (memory-management.md section 3; plan N2.2 string half):
//   Resource: (a) napi_value handles returned by this lane (JsValueRef).
//     Domain: Recycler heap (engine values). Owner: innermost scope stack
//     entry (NodeApiScopes) until scope close / destroy step 6; transfer at
//     creation (create -> root, synchronously, no intervening JS). Release:
//     JsRelease at scope close. Tagged primitives (numbers/bools/null/
//     undefined) record a slot but JsAddRef is a documented no-op (R-REF-10);
//     GC-safety is vacuous for those.
//   (b) Transcode scratch (Latin1 create/get only): provider native heap
//     std::vector<std::uint16_t> temporary scoped to the call (no
//     retention, freed before return). Domain: provider heap via C++
//     allocator bound to the provider; backing lifetime == this call.
//     Invalidation: function return. Thread/context: env thread with the
//     env context entered. Reentrancy: no JS runs while the scratch lives
//     (JsCreateStringUtf16 / JsCopyStringUtf16 copies, then scratch dies).
//     Accounting: no root-count contribution. OOM (vector allocation)
//     => napi_generic_failure with last-error recorded, nothing published,
//     buffer untouched.
//   (c) N2.6 transient intrinsic lookups (Object / BigInt / Date / Symbol
//     globals and their prototype methods; decimal scratch strings and
//     limb vectors): fetched fresh per call and held by TmpRoot (RAII
//     JsAddRef/JsRelease pair, never the scope stack) for the duration of
//     the call only. There is deliberately NO per-env cache (N2.3
//     precedent): a cache would need a NodeEnv field (layout owned by
//     N1.1) or a cross-env side table; re-fetch costs a few JsGetProperty
//     calls. No retention, no per-env state, so nothing to release at
//     destroy. The words->decimal conversion scratch (limb copy +
//     1e9 groups) is C++ heap, freed before return; OOM =>
//     napi_generic_failure, nothing published.
//   (d) napi_create_bigint_words over-cap RangeError: created and
//     JsSetException'd inside the call (TmpRoot'd across the set; the
//     engine holds it as the pending exception afterwards).
//   Tests: test/nodeapi/probes/values_probe.cpp (run_values_probe.sh)
//   and test/nodeapi/probes/coerce_probe.cpp (run_coerce_probe.sh).
//   Node-differential receipts (lossless/wrap/words/coerce-null) are
//   recorded in the probe header and .cpp comments.
//   Thread/context: owner thread only (R3/R-ENV-11) via EntryGuard.
//
// Language: C++17 (GNU++17 via `zig c++`, matching Wave 1) behind the
// extern "C" napi_* surface (PR-4 decision record: C++ through provider
// v1). The C surface is language-agnostic.

#ifndef CHAKRA_NODEAPI_VALUES_H_
#define CHAKRA_NODEAPI_VALUES_H_

#include "js_native_api.h" /* napi_status, napi_value, NAPI_VERSION=8 */

#endif /* CHAKRA_NODEAPI_VALUES_H_ */
