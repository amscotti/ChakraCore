//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Node-API binary values (plan doc/node-api-plan.md task N3.1, A5 group;
// section 3.4 R4 borrow rules, section 3.7 finalizer discipline, section 4.3 gaps).
//
// Implements the v8 A5 binary surface:
//   napi_create_arraybuffer / napi_create_external_arraybuffer /
//   napi_get_arraybuffer_info / napi_detach_arraybuffer /
//   napi_is_detached_arraybuffer / napi_create_typedarray /
//   napi_get_typedarray_info / napi_create_dataview /
//   napi_get_dataview_info
// (napi_is_arraybuffer / napi_is_typedarray / napi_is_dataview are
// implemented in NodeApiValues.* N2.6 and are not duplicated here.)
//
// Status discipline (section 3.10 / doc/node-api-status-mapping.md C column):
//   - Gated (No): create_* (JS creation calls that may throw; validated-first
//     then EntryGuard allowPending=false)
//   - Executes (Yes): get_*_info, detach, is_detached (allowPending=true)
//   Type pre-checks produce specific codes per table; JsRT errors map per
//   section 3.10. Success clears last_error; failure sets it. Out-params are
//   written only on success.

#ifndef CHAKRA_NODEAPI_BINARY_H_
#define CHAKRA_NODEAPI_BINARY_H_

#include "js_native_api.h" /* napi_status, napi_value, NAPI_VERSION=8 */

#include <cstddef> /* size_t */
#include <cstdint> /* uint64_t */

// Drain external-ArrayBuffer finalizer records queued for env's generation,
// invoking each user finalizer once (env thread, context entered by the
// caller). No-op when env is not live; dead-generation records never queue
// (deleted at enqueue time), so no stale dispatch is possible.
void DrainExternalFinalizers(napi_env env);

// Destroy tail (call after the live registry entry is erased): delete any
// records still queued for `generation` WITHOUT invoking user finalizers.
// Pure native (no context/JS). Only that generation's queue is touched.
void NodeApiBinary_DestroyForEnv(napi_env env, uint64_t generation);

// Introspection for tests/probes (A8-1): live ExternalRecord objects, and
// the total number of records currently queued. Both are zero when every
// record was invoked-or-discarded exactly once.
size_t NodeApiBinary_ExternalLiveCount();
size_t NodeApiBinary_ExternalQueuedCount();

#endif /* CHAKRA_NODEAPI_BINARY_H_ */
