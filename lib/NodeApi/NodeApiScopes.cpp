//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Handle scopes implementation (plan §3.5, semantics §2). See the header for
// the reconciled N1.1 interface (nodeapi::CheckAlive thread/state guard),
// the ownership record, and per-function contracts. C++ implementation
// (GNU++17 via `zig c++`) until PR-4; the extern "C" surface is
// language-agnostic. No C++ exception crosses the extern "C" surface:
// every provider allocation on a napi_* path is nothrow/try-catch
// wrapped and maps to napi_generic_failure (OOM hardening).

#include "NodeApiScopes.h"
#include "NodeApiEnv.h" /* nodeapi::CheckAlive / SetLastError (HIGH-3) */

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

/* Refs-record contribution to the R-REF-14 total (defined in NodeApiRefs.cpp;
 * the scopes probe shim stubs it to 0). Declared here (not via
 * NodeApiRefs.h) so this TU stays link-independent of the refs header:
 * the scopes probe links this TU without the refs TU (plus the shim stub).
 * The symbol is test-hook only (see the total-counter comment in
 * NodeApiScopes.h); release builds never reference it. */
#ifdef CHAKRA_NODEAPI_TEST_HOOKS
extern "C" size_t NodeApiRefs_RefRootCount(napi_env env);
#endif

namespace scopes_impl {

/* One rooted slot. `id` is unique within the state (monotonic) so a
 * RootedValue can find its slot after other slots moved/closed. */
struct RootEntry {
  napi_value value;
  unsigned long long id;
};

/* The napi_handle_scope / napi_escapable_handle_scope handle IS this struct
 * (identity = pointer). It lives on the heap while on the stack; close
 * frees it. `scope_id` is state-monotonic and never reused, which lets the
 * RootedValue destructor distinguish "closed" from "address reallocated"
 * without dereferencing freed memory (it only scans live stack members). */
struct Scope {
  unsigned long long scope_id;
  bool escapable;
  bool escaped;
  bool implicit;
  bool is_default;
  std::vector<RootEntry> roots;
};

} /* namespace scopes_impl */

/* Per-env scope state. Completes the opaque NodeApiScopeState forward-
 * declared in the header (owned by the env: created at env creation,
 * freed after NodeApiScopes_DestroyForEnv). The probe shim only holds the
 * opaque pointer; this layout is private to this TU. Defined at global
 * scope so it completes ::NodeApiScopeState. */
struct NodeApiScopeState {
  std::vector<scopes_impl::Scope*> stack; /* stack[0] is always the default scope */
  unsigned long long next_scope_id = 1;
  unsigned long long next_entry_id = 1;
  size_t baseline = 0; /* root count at creation (0: default starts empty) */
};

namespace {

using RootEntry = scopes_impl::RootEntry;
using Scope = scopes_impl::Scope;
using ScopeState = NodeApiScopeState;

using Stack = std::vector<Scope*>;

Scope* AsScope(napi_handle_scope h) {
  return reinterpret_cast<Scope*>(h);
}

/* Pointer-identity lookup in the stack; -1 if absent. Never dereferences
 * the probe handle, so foreign/dangling handles are safe to test. */
long long FindIndex(const Stack& stack, const Scope* probe) {
  for (size_t i = 0; i < stack.size(); ++i) {
    if (stack[i] == probe) return static_cast<long long>(i);
  }
  return -1;
}

size_t TotalRoots(const Stack& stack) {
  size_t n = 0;
  for (Scope* s : stack) n += s->roots.size();
  return n;
}

/* Entry-guard light (HIGH-3): routes every mutating scope entry through
 * the reconciled N1.1 guard nodeapi::CheckAlive (live + Alive + owner
 * thread, R-ENV-4/R-ENV-11). Wrong thread => debug assert + release
 * napi_invalid_arg WITHOUT entering the wrong context and without
 * touching scope state (the check runs before any stack access);
 * Closing => napi_generic_failure (recorded by CheckAlive).
 * Pending-exception check is INTENTIONALLY skipped (R-SC-8).
 * Rooting helpers have an AllowClosing variant (below) so
 * teardown-compatible get_reference_value during Closing still roots
 * (released at destroy step 6b); open/close/escape stay strict. */
napi_status ValidateEnv(napi_env env, ScopeState** out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  napi_status vs = nodeapi::CheckAlive(env);
  if (vs != napi_ok) return vs;
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return napi_invalid_arg;
  *out = st;
  return napi_ok;
}

/* Teardown-compatible rooting guard: same thread/live checks as
 * ValidateEnv, but Closing is permitted (only Dead fails). Used ONLY
 * by RootIntoInnermost/CallerScope so get_reference_value during
 * Closing (teardown-compatible per Refs) still roots into the live
 * scope stack; the root is released at step 6b. Wrong-thread still
 * fails with assert/invalid_arg and never mutates. */
napi_status ValidateEnvAllowClosing(napi_env env, ScopeState** out) {
  if (env == nullptr || out == nullptr) return napi_invalid_arg;
  napi_status vs = nodeapi::CheckAliveAllowClosing(env);
  if (vs != napi_ok) return vs;
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return napi_invalid_arg;
  *out = st;
  return napi_ok;
}

/* Liveness of the rooting calls below: JsAddRef/JsRelease need a current
 * thread context (R3 / plan §3.2). The entry guard (N1.5) owns context
 * discipline; in debug we verify a context is current so a mis-entered
 * caller fails loudly instead of silently unrooting. Release builds skip
 * the check (JsRT reports the error and we map it). */
bool HasCurrentContext() {
  JsContextRef ctx = JS_INVALID_REFERENCE;
  if (JsGetCurrentContext(&ctx) != JsNoError) return false;
  return ctx != JS_INVALID_REFERENCE;
}

napi_status RootInto(napi_env env, Scope* scope, ScopeState* st, napi_value value) {
  assert(scope != nullptr && st != nullptr && value != nullptr);
  assert(HasCurrentContext());
  /* R1 ordering: the caller produced the value immediately before this
   * call with no intervening JS execution. JsAddRef on tagged values
   * (numbers/bools/null/undefined) is a documented no-op (R-REF-10):
   * the slot is still recorded (bookkeeping balances) but GC-safety is
   * vacuous -- the value is not collectable anyway. A JsRT failure is
   * propagated as generic failure without recording a slot. Provider
   * allocation failure (push_back) rolls back the JsAddRef so no root
   leaks, records last_error, and returns napi_generic_failure with no
   throw across the extern "C" boundary (OOM hardening). */
  JsErrorCode err = JsAddRef(reinterpret_cast<JsValueRef>(value), nullptr);
  if (err != JsNoError) return napi_generic_failure;
  try {
    scope->roots.push_back(RootEntry{value, st->next_entry_id++});
  } catch (...) {
    (void)JsRelease(reinterpret_cast<JsValueRef>(value), nullptr);
    nodeapi::SetLastError(env, napi_generic_failure, "scope root storage OOM");
    return napi_generic_failure;
  }
  return napi_ok;
}

/* Releases every root in the scope and frees the scope struct.
 * Callers must have removed (or be about to remove) `scope` from the
 * stack. After this returns the handle is dangling (UB to reuse,
 * AMB-SC-3). */
void ReleaseAndFreeScope(Scope* scope) {
  assert(scope != nullptr);
  for (const RootEntry& e : scope->roots) {
    /* JsRelease does not validate balance (R2). A failure here has no
     * recoverable action: the scope is closing regardless. In debug,
     * assert for visibility; release builds ignore (the counter, not
     * JsRT, detects imbalance -- R-REF-13). */
    JsErrorCode err = JsRelease(reinterpret_cast<JsValueRef>(e.value), nullptr);
    assert(err == JsNoError);
    (void)err;
  }
  scope->roots.clear();
  delete scope;
}

/* Nothrow push: returns nullptr on provider OOM (callers map to
 * napi_generic_failure, except CreateState which converts to bad_alloc
 * for the env-creation rollback path). The stack vector's push_back
 * failure deletes the fresh Scope so no record leaks. */
Scope* PushScope(ScopeState* st, bool escapable, bool implicit, bool is_default) {
  Scope* s = new (std::nothrow) Scope();
  if (s == nullptr) return nullptr;
  s->scope_id = st->next_scope_id++;
  s->escapable = escapable;
  s->escaped = false;
  s->implicit = implicit;
  s->is_default = is_default;
  try {
    st->stack.push_back(s);
  } catch (...) {
    delete s;
    return nullptr;
  }
  return s;
}

napi_status OpenImpl(napi_env env, napi_handle_scope* result, bool escapable,
                     bool implicit) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnv(env, &st);
  if (v != napi_ok) return v;
  if (result == nullptr) return napi_invalid_arg;
  if (st->stack.empty()) return napi_generic_failure; /* no default: env bug */
  Scope* ns = PushScope(st, escapable, implicit, false);
  if (ns == nullptr) {
    nodeapi::SetLastError(env, napi_generic_failure, "scope allocation OOM");
    return napi_generic_failure;
  }
  *result = reinterpret_cast<napi_handle_scope>(ns);
  return napi_ok;
}

/* Shared close core. Returns ok on clean LIFO close; mismatch (with
 * release) when the named scope is not the stack top, or when its kind
 * disagrees with the closing API's expectation (AMB-SC-2 + symmetric rule
 * for close_handle_scope on an escapable scope). Unknown/NULL handles are
 * invalid_arg without dereferencing. */
napi_status CloseImpl(napi_env env, napi_handle_scope handle, bool expect_escapable) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnv(env, &st);
  if (v != napi_ok) return v;
  if (handle == nullptr) return napi_invalid_arg;
  Scope* scope = AsScope(handle);
  long long idx = FindIndex(st->stack, scope);
  if (idx < 0) return napi_invalid_arg; /* foreign or already closed */
  /* The default scope's handle is never handed out (R-SC-7); a match
   * here means a fabricated handle reached us. Refuse, release nothing. */
  if (scope->is_default) return napi_generic_failure; /* env bug; impossible via valid handles */
  napi_status out = napi_ok;
  if (scope->escapable != expect_escapable) out = napi_handle_scope_mismatch;
  else if (static_cast<size_t>(idx) != st->stack.size() - 1) {
    out = napi_handle_scope_mismatch;
  }
  /* Remove ONLY the named scope; the rest of the stack is intact (R-SC-6).
   * Its roots are released on every path (no leak on misuse). */
  st->stack.erase(st->stack.begin() + static_cast<long long>(idx));
  ReleaseAndFreeScope(scope);
  return out;
}

} /* namespace */

extern "C" {

NodeApiScopeState* NodeApiScopes_CreateState(void) {
  auto* st = new ScopeState();
  if (PushScope(st, false, false, true) == nullptr) {
    /* Env-creation OOM is fatal, not a napi generic_failure: preserve
     * the Env.cpp rollback path (it catches std::bad_alloc from the
     * throwing `new ScopeState()` above). PushScope is nothrow, so
     * convert its nullptr back into the thrown path here. */
    delete st;
    throw std::bad_alloc();
  }
  st->baseline = TotalRoots(st->stack); /* 0 */
  return reinterpret_cast<NodeApiScopeState*>(st);
}

void NodeApiScopes_DestroyForEnv(napi_env env) {
  if (env == nullptr) return;
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return;
  /* Destroy step 6: close the whole stack top-down, incl. default.
   * Idempotent: a second call finds an empty stack and returns. */
  while (!st->stack.empty()) {
    Scope* top = st->stack.back();
    st->stack.pop_back();
    ReleaseAndFreeScope(top);
  }
}

void NodeApiScopes_FreeState(NodeApiScopeState* state) {
  if (state == nullptr) return;
  delete reinterpret_cast<ScopeState*>(state);
}

size_t NodeApiScopes_Depth(napi_env env) {
  if (env == nullptr) return 0;
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return 0;
  return st->stack.size();
}

napi_status NAPI_CDECL napi_open_handle_scope(napi_env env,
                                              napi_handle_scope* result) {
  return OpenImpl(env, result, false, false);
}

napi_status NAPI_CDECL napi_close_handle_scope(napi_env env,
                                               napi_handle_scope scope) {
  return CloseImpl(env, scope, false);
}

napi_status NAPI_CDECL napi_open_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope* result) {
  return OpenImpl(env, reinterpret_cast<napi_handle_scope*>(result), true, false);
}

napi_status NAPI_CDECL napi_close_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope scope) {
  return CloseImpl(env, reinterpret_cast<napi_handle_scope>(scope), true);
}

napi_status NAPI_CDECL napi_escape_handle(napi_env env,
                                          napi_escapable_handle_scope scope,
                                          napi_value escapee,
                                          napi_value* result) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnv(env, &st);
  if (v != napi_ok) return v;
  if (scope == nullptr || escapee == nullptr || result == nullptr) {
    return napi_invalid_arg;
  }
  Scope* s = AsScope(reinterpret_cast<napi_handle_scope>(scope));
  long long idx = FindIndex(st->stack, s);
  if (idx < 0) return napi_invalid_arg;
  if (!s->escapable) return napi_invalid_arg; /* AMB-SC-1 spec choice */
  if (s->escaped) {
    /* R-SC-4: second escape moves nothing; the first escape stands. */
    *result = escapee;
    return napi_escape_called_twice;
  }
  /* Locate the escapee's slot in this scope's list (identity by handle +
   * presence). AMB-SC-4: a value not rooted here is invalid_arg and does
   * NOT set the escaped flag, so a later valid escape can still succeed. */
  size_t pos = s->roots.size();
  for (size_t i = 0; i < s->roots.size(); ++i) {
    if (s->roots[i].value == escapee) {
      pos = i;
      break;
    }
  }
  if (pos == s->roots.size()) return napi_invalid_arg;
  if (idx == 0) return napi_generic_failure; /* no parent: env bug */

  /* Move: transfer the entry (same id) to the parent list with NO
   * AddRef/Release pair -- ownership transfers, the JsAddRef count is
   * unchanged. The value now survives this scope's close (R-SC-3/5).
   * Parent push first (OOM-safe): if the parent push throws, the entry
   * stays in the escapable scope and the escape reports generic_failure
   * with no flag set, so a retry is possible. Only on successful push
   * is the inner slot erased. */
  RootEntry entry = s->roots[pos];
  try {
    st->stack[static_cast<size_t>(idx) - 1]->roots.push_back(entry);
  } catch (...) {
    nodeapi::SetLastError(env, napi_generic_failure, "escape root storage OOM");
    return napi_generic_failure;
  }
  s->roots.erase(s->roots.begin() + static_cast<long long>(pos));
  s->escaped = true;
  *result = escapee;
  return napi_ok;
}

napi_status NodeApiScopes_RootIntoInnermost(napi_env env, napi_value value) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnvAllowClosing(env, &st);
  if (v != napi_ok) return v;
  if (value == nullptr) return napi_invalid_arg;
  if (st->stack.empty()) return napi_generic_failure;
  return RootInto(env, st->stack.back(), st, value);
}

napi_status NodeApiScopes_RootIntoCallerScope(napi_env env, napi_value value) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnvAllowClosing(env, &st);
  if (v != napi_ok) return v;
  if (value == nullptr) return napi_invalid_arg;
  /* Caller scope = one below the innermost (implicit) scope. */
  if (st->stack.size() < 2) return napi_handle_scope_mismatch;
  return RootInto(env, st->stack[st->stack.size() - 2], st, value);
}

napi_status NodeApiScopes_OpenImplicit(napi_env env, napi_handle_scope* result) {
  return OpenImpl(env, result, false, true);
}

int /*bool*/ NodeApiScopes_IsOpen(napi_env env, napi_handle_scope scope) {
  if (env == nullptr || scope == nullptr) return 0;
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return 0;
  return FindIndex(st->stack, AsScope(scope)) >= 0 ? 1 : 0;
}

napi_status NodeApiScopes_EnforceImplicitBalance(napi_env env, size_t entry_depth) {
  ScopeState* st = nullptr;
  napi_status v = ValidateEnv(env, &st);
  if (v != napi_ok) return v;
  size_t depth = st->stack.size();
  size_t expected = entry_depth + 1; /* +1 = the implicit scope */
  if (depth == expected) return napi_ok;
  if (depth > expected) {
    /* The callback left extra scopes open: unwind them (releasing their
     * roots) so depth == expected. The thunk still closes its implicit
     * scope normally afterwards. */
    while (st->stack.size() > expected) {
      Scope* top = st->stack.back();
      st->stack.pop_back();
      ReleaseAndFreeScope(top);
    }
    return napi_handle_scope_mismatch;
  }
  /* Underflow: the callback closed the implicit scope (or more). The
   * thunk reports and continues; the stack is left as-is (nothing to
   * unwind). Depth < entry_depth means the callback also closed an
   * outer/env scope: that is exactly the provider-bug the check exists
   * to surface -- it is reported, never silently repaired. */
  return napi_handle_scope_mismatch;
}

#ifdef CHAKRA_NODEAPI_TEST_HOOKS
size_t chakra_nodeapi_test_root_count(napi_env env) {
  if (env == nullptr) return static_cast<size_t>(-1);
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return static_cast<size_t>(-1);
  /* R-REF-14 unified total: scope roots (including ref-get results
   * via HIGH-4) + live ref-record roots from the refs TU. */
  size_t scope_n = TotalRoots(st->stack);
  size_t refs_n = NodeApiRefs_RefRootCount(env);
  return scope_n + refs_n;
}

size_t chakra_nodeapi_test_root_baseline(napi_env env) {
  if (env == nullptr) return static_cast<size_t>(-1);
  ScopeState* st = NodeApiEnv_ScopeState(env);
  if (st == nullptr) return static_cast<size_t>(-1);
  return st->baseline;
}

size_t chakra_nodeapi_test_scope_depth(napi_env env) {
  return NodeApiScopes_Depth(env);
}
#endif /* CHAKRA_NODEAPI_TEST_HOOKS */

} /* extern "C" */

/* ----- RootedValue (C++ RAII; see header) ----- */

namespace NodeApiScopes {

RootedValue::RootedValue(napi_env env, napi_value value)
    : env_(env), value_(value) {
  if (env == nullptr || value == nullptr) return;
  /* HIGH-3: never mutate scope state cross-thread. CheckAlive asserts
   * (debug) / returns invalid_arg (release) on wrong-thread or
   * Closing/dead envs; the wrapper stays inert on any failure. */
  if (nodeapi::CheckAlive(env) != napi_ok) return;
  auto* st = reinterpret_cast<ScopeState*>(NodeApiEnv_ScopeState(env));
  if (st == nullptr || st->stack.empty()) return;
  Scope* s = st->stack.back();
  if (RootInto(env, s, st, value) != napi_ok) return;
  scope_ = s;
  scope_id_ = s->scope_id;
  entry_id_ = s->roots.back().id;
  active_ = true;
}

RootedValue::~RootedValue() { reset(); }

RootedValue::RootedValue(RootedValue&& other) noexcept
    : env_(other.env_),
      value_(other.value_),
      scope_(other.scope_),
      scope_id_(other.scope_id_),
      entry_id_(other.entry_id_),
      active_(other.active_) {
  other.active_ = false;
  other.value_ = nullptr;
}

RootedValue& RootedValue::operator=(RootedValue&& other) noexcept {
  if (this != &other) {
    reset();
    env_ = other.env_;
    value_ = other.value_;
    scope_ = other.scope_;
    scope_id_ = other.scope_id_;
    entry_id_ = other.entry_id_;
    active_ = other.active_;
    other.active_ = false;
    other.value_ = nullptr;
  }
  return *this;
}

napi_value RootedValue::release() {
  napi_value v = value_;
  reset();
  return v;
}

void RootedValue::reset() {
  if (!active_) return;
  active_ = false;
  if (env_ == nullptr) return;
  /* HIGH-3: never mutate scope state cross-thread. On wrong-thread,
   * Closing, or dead envs, leave the slot for destroy step 6 to
   * release (batched; no cross-thread mutation, no double release). */
  if (nodeapi::CheckAlive(env_) != napi_ok) return;
  auto* st = reinterpret_cast<ScopeState*>(NodeApiEnv_ScopeState(env_));
  if (st == nullptr) return;
  /* Find the live scope by id (scope_id is never reused, so a reallocated
   * Scope at the old address is not mistaken for ours). If the scope was
   * closed (slot already released) or the slot was escaped away, the
   * search misses and this is a no-op -- crucially, no double release.
   * Only stack members are dereferenced; they are all live. */
  Scope* live = nullptr;
  for (Scope* s : st->stack) {
    if (s->scope_id == scope_id_) {
      live = s;
      break;
    }
  }
  if (live == nullptr) return;
  for (size_t i = 0; i < live->roots.size(); ++i) {
    if (live->roots[i].id == entry_id_) {
      napi_value v = live->roots[i].value;
      live->roots.erase(live->roots.begin() + static_cast<long long>(i));
      assert(HasCurrentContext());
      JsErrorCode err = JsRelease(reinterpret_cast<JsValueRef>(v), nullptr);
      assert(err == JsNoError);
      (void)err;
      break;
    }
  }
}

} /* namespace NodeApiScopes */
