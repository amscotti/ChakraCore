//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//
// Self-contained N1.2 scope probe driver (plan task N1.2, semantics §2 P2-*).
//
// Why a driver and not an addon + ch-nodeapi run: the real provider
// (libChakraNodeApi) does not exist yet -- N1.1 has not landed, so there is
// no chakra_nodeapi_create_env to drive an addon through. Per the task card,
// the probe is therefore a self-contained C++ driver using the private host
// API shape (ScopesProbe_CreateEnv/DestroyEnv in scopes_shim.cpp, which
// implements the Assumed N1.1 Interface from NodeApiScopes.h). It links
// lib/NodeApi/NodeApiScopes.cpp directly against the engine dylib. When N1.1
// lands, the same cases run through ch-nodeapi + a scopes addon unchanged.
//
// Each case prints [PASS]/[FAIL] + root-counter checkpoints (R-REF-14) and
// the process exits nonzero on any failure. GC probes null their value
// locals + ClearStack before JsCollectGarbage, mirroring
// bin/NativeTests/JsRTApiTest.cpp WeakReferenceTest (Chakra tracks
// stack-resident JsRefs; a lingering local would keep the value alive).
// Weak handles are JsAddRef-rooted for the test window (R6).
//
// Build/run: tools/nodeapi/run_probe.sh (both profiles via gate lock).

#include "NodeApiScopes.h"
#include "scopes_shim.h"
#include "ChakraCommon.h"
#include "ChakraCore.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#ifndef CHAKRA_NODEAPI_TEST_HOOKS
#error "scopes_probe requires -DCHAKRA_NODEAPI_TEST_HOOKS (root counter)"
#endif

namespace {

int g_failures = 0;
JsRuntimeHandle g_runtime = JS_INVALID_RUNTIME_HANDLE;
JsContextRef g_ctx = JS_INVALID_REFERENCE;
napi_env g_env = nullptr;

size_t Cnt() { return chakra_nodeapi_test_root_count(g_env); }
size_t Base() { return chakra_nodeapi_test_root_baseline(g_env); }
size_t Depth() { return NodeApiScopes_Depth(g_env); }

void Check(bool ok, const char* id, const char* what) {
  std::printf("[%s] %s: %s (count=%zu baseline=%zu depth=%zu)\n",
              ok ? "PASS" : "FAIL", id, what, Cnt(), Base(), Depth());
  if (!ok) g_failures++;
}

void CheckStatus(napi_status got, napi_status want, const char* id,
                 const char* what) {
  bool ok = got == want;
  std::printf("[%s] %s: %s (status=%d want=%d, count=%zu baseline=%zu depth=%zu)\n",
              ok ? "PASS" : "FAIL", id, what, static_cast<int>(got),
              static_cast<int>(want), Cnt(), Base(), Depth());
  if (!ok) g_failures++;
}

/* Mirror of bin/NativeTests ClearStack: zero a volatile buffer so stale
 * JsRef copies on the stack do not keep values alive across GC. */
__attribute__((noinline)) void ClearStack() {
  constexpr int kN = 256;
  volatile void* dummy[kN];
  volatile unsigned char* bp = reinterpret_cast<volatile unsigned char*>(dummy);
  while (bp < reinterpret_cast<volatile unsigned char*>(dummy) + sizeof(dummy)) {
    *bp++ = 0;
  }
}

void ForceGC() {
  for (int i = 0; i < 10; i++) JsCollectGarbage(g_runtime);
}

/* Root a weak handle for the probe window (R6), deref, then unroot. */
bool WeakAlive(JsWeakRef weak) {
  if (weak == JS_INVALID_REFERENCE) return false;
  JsValueRef v = JS_INVALID_REFERENCE;
  bool ok = JsGetWeakReferenceValue(weak, &v) == JsNoError &&
            v != JS_INVALID_REFERENCE;
  return ok;
}

/* Create an object rooted in the innermost scope; returns the napi_value. */
napi_value CreateObj() {
  napi_value v = nullptr;
  napi_create_object(g_env, &v);
  return v;
}

/* ---- P2-3: inner-scope value collected after close (weak ref + GC) ----
 * The GC probes need -O0-style stack behaviour: at -O3 the compiler keeps
 * JsValueRef copies in registers across ClearStack/Collect and the
 * conservative stack scan sees them as roots even after locals are nulled.
 * Keep the probe logic optnone while the provider TU stays at the profile
 * default. */
__attribute__((noinline, optnone)) void P2_3() {
  napi_handle_scope s = nullptr;
  CheckStatus(napi_open_handle_scope(g_env, &s), napi_ok, "P2-3",
              "open inner scope");
  napi_value v = CreateObj(); /* rooted in inner scope */
  JsValueRef jv = reinterpret_cast<JsValueRef>(v);
  JsWeakRef weak = JS_INVALID_REFERENCE;
  JsCreateWeakReference(jv, &weak);
  JsAddRef(weak, nullptr); /* R6: weak handle root for the window */
  Check(Cnt() == Base() + 1, "P2-3", "one root inside scope");
  CheckStatus(napi_close_handle_scope(g_env, s), napi_ok, "P2-3",
              "close inner scope");
  Check(Cnt() == Base(), "P2-3", "root released at close");
  v = nullptr;
  jv = JS_INVALID_REFERENCE;
  ClearStack();
  ForceGC();
  Check(!WeakAlive(weak), "P2-3", "value collected after close + GC");
  JsRelease(weak, nullptr);
  Check(Cnt() == Base(), "P2-3", "counter at baseline after probe");
}

/* ---- P2-2: escaped value survives escapable close; sibling dies ---- */
__attribute__((noinline, optnone)) void P2_2() {
  napi_handle_scope outer = nullptr;
  napi_escapable_handle_scope esc = nullptr;
  napi_open_handle_scope(g_env, &outer);
  napi_open_escapable_handle_scope(g_env, &esc);
  napi_value kept = CreateObj(); /* in escapable scope */
  napi_value doomed = CreateObj(); /* in escapable scope */
  JsWeakRef wk = JS_INVALID_REFERENCE;
  JsWeakRef wd = JS_INVALID_REFERENCE;
  JsCreateWeakReference(reinterpret_cast<JsValueRef>(kept), &wk);
  JsCreateWeakReference(reinterpret_cast<JsValueRef>(doomed), &wd);
  JsAddRef(wk, nullptr);
  JsAddRef(wd, nullptr);
  Check(Cnt() == Base() + 2, "P2-2", "two roots in escapable scope");
  napi_value rescued = nullptr;
  CheckStatus(napi_escape_handle(g_env, esc, kept, &rescued), napi_ok, "P2-2",
              "escape one value to outer");
  Check(rescued == kept, "P2-2", "escape result is the escapee");
  Check(Cnt() == Base() + 2, "P2-2", "escape moves, does not add/release");
  CheckStatus(napi_close_escapable_handle_scope(g_env, esc), napi_ok, "P2-2",
              "close escapable scope");
  Check(Cnt() == Base() + 1, "P2-2", "only the doomed root released");
  kept = nullptr;
  doomed = nullptr;
  ClearStack();
  ForceGC();
  Check(WeakAlive(wk), "P2-2", "escaped value alive after GC");
  Check(!WeakAlive(wd), "P2-2", "non-escaped sibling collected after GC");
  CheckStatus(napi_close_handle_scope(g_env, outer), napi_ok, "P2-2",
              "close outer scope");
  JsRelease(wk, nullptr);
  JsRelease(wd, nullptr);
  ClearStack();
  ForceGC();
  Check(Cnt() == Base(), "P2-2", "counter at baseline after probe");
}

/* ---- P2-4: escape twice => napi_escape_called_twice; first stands ---- */
void P2_4() {
  napi_handle_scope outer = nullptr;
  napi_escapable_handle_scope esc = nullptr;
  napi_open_handle_scope(g_env, &outer);
  napi_open_escapable_handle_scope(g_env, &esc);
  napi_value v = CreateObj();
  napi_value out = nullptr;
  CheckStatus(napi_escape_handle(g_env, esc, v, &out), napi_ok, "P2-4",
              "first escape");
  CheckStatus(napi_escape_handle(g_env, esc, v, &out),
              napi_escape_called_twice, "P2-4", "second escape rejected");
  Check(Cnt() == Base() + 1, "P2-4", "no double-move (still one root)");
  CheckStatus(napi_close_escapable_handle_scope(g_env, esc), napi_ok, "P2-4",
              "close escapable");
  Check(Cnt() == Base() + 1, "P2-4", "first escape stands (in outer)");
  CheckStatus(napi_close_handle_scope(g_env, outer), napi_ok, "P2-4",
              "close outer");
  Check(Cnt() == Base(), "P2-4", "counter at baseline after probe");
}

/* ---- P2-4b: escape on a plain scope => invalid_arg (AMB-SC-1) ---- */
void P2_4b() {
  napi_handle_scope plain = nullptr;
  napi_open_handle_scope(g_env, &plain);
  napi_value v = CreateObj();
  napi_value out = nullptr;
  CheckStatus(
      napi_escape_handle(g_env,
                         reinterpret_cast<napi_escapable_handle_scope>(plain),
                         v, &out),
      napi_invalid_arg, "P2-4b", "escape on plain scope");
  Check(Cnt() == Base() + 1, "P2-4b", "no root moved or released");
  CheckStatus(napi_close_handle_scope(g_env, plain), napi_ok, "P2-4b",
              "close plain scope");
  Check(Cnt() == Base(), "P2-4b", "counter at baseline after probe");
}

/* ---- P2-5: out-of-order close => mismatch + named scope released ---- */
void P2_5() {
  napi_handle_scope b = nullptr;
  napi_handle_scope c = nullptr;
  napi_open_handle_scope(g_env, &b);
  napi_value vb = CreateObj(); /* rooted in B */
  napi_open_handle_scope(g_env, &c);
  napi_value vc = CreateObj(); /* rooted in C */
  Check(Cnt() == Base() + 2, "P2-5", "one root in B, one in C");
  CheckStatus(napi_close_handle_scope(g_env, b), napi_handle_scope_mismatch,
              "P2-5", "close B while C open");
  Check(Cnt() == Base() + 1, "P2-5", "B's roots released, C's intact");
  Check(NodeApiScopes_IsOpen(g_env, c) == 1, "P2-5", "C still open");
  Check(NodeApiScopes_IsOpen(g_env, b) == 0, "P2-5", "B gone after mismatch");
  CheckStatus(napi_close_handle_scope(g_env, c), napi_ok, "P2-5",
              "correct-order close of C afterwards");
  Check(Cnt() == Base(), "P2-5", "counter at baseline after probe");
  (void)vb;
  (void)vc;
}

/* ---- P2-5b: close-escapable on plain scope => mismatch+release (AMB-SC-2) ---- */
void P2_5b() {
  napi_handle_scope p = nullptr;
  napi_open_handle_scope(g_env, &p);
  CreateObj();
  Check(Cnt() == Base() + 1, "P2-5b", "one root in plain scope");
  CheckStatus(
      napi_close_escapable_handle_scope(
          g_env, reinterpret_cast<napi_escapable_handle_scope>(p)),
      napi_handle_scope_mismatch, "P2-5b",
      "close-escapable on plain scope");
  Check(Cnt() == Base(), "P2-5b", "roots released on mismatch");
}

size_t Base0Depth() {
  /* Depth right after env creation is 1 (default scope). */
  return 1;
}

/* ---- P2-6: 10x nested open/close in order ---- */
void P2_6() {
  napi_handle_scope handles[10];
  bool ok = true;
  for (int i = 0; i < 10; i++) {
    handles[i] = nullptr;
    napi_status s = napi_open_handle_scope(g_env, &handles[i]);
    ok = ok && (s == napi_ok);
    CreateObj();
  }
  Check(ok && Depth() == Base0Depth() + 10, "P2-6", "10 scopes nested");
  ok = true;
  for (int i = 9; i >= 0; i--) {
    napi_status s = napi_close_handle_scope(g_env, handles[i]);
    ok = ok && (s == napi_ok);
  }
  Check(ok, "P2-6", "all closes in order");
  Check(Cnt() == Base() && Depth() == Base0Depth(), "P2-6",
        "counter and depth at baseline");
}

/* ---- P2-7: close executes with a pending exception (R-SC-8) ---- */
void P2_7() {
  napi_handle_scope s = nullptr;
  napi_open_handle_scope(g_env, &s);
  CreateObj();
  JsValueRef exc = JS_INVALID_REFERENCE;
  JsCreateObject(&exc);
  JsValueRef use = exc;
  JsSetException(exc);
  bool has = false;
  JsHasException(&has);
  Check(has, "P2-7", "exception pending before close");
  CheckStatus(napi_close_handle_scope(g_env, s), napi_ok, "P2-7",
              "close runs despite pending exception");
  bool still = false;
  JsHasException(&still);
  Check(still, "P2-7", "exception still pending after close");
  JsValueRef cleared = JS_INVALID_REFERENCE;
  JsGetAndClearException(&cleared);
  Check(cleared == exc, "P2-7", "pending exception is the one we set");
  (void)use;
  Check(Cnt() == Base(), "P2-7", "counter at baseline after probe");
}

/* ---- P2-8: balance verification, both arms (R-SC-10 / P8-5 shape) ---- */
void P2_8() {
  size_t entry = Depth(); /* includes default */
  napi_handle_scope impl = nullptr;
  CheckStatus(NodeApiScopes_OpenImplicit(g_env, &impl), napi_ok, "P2-8",
              "thunk opens implicit scope");
  /* Arm 1: leaky callback leaves an extra scope open. */
  napi_handle_scope extra = nullptr;
  napi_open_handle_scope(g_env, &extra);
  CreateObj(); /* rooted in the leaked scope */
  Check(Cnt() == Base() + 1, "P2-8", "extra scope holds one root");
  CheckStatus(NodeApiScopes_EnforceImplicitBalance(g_env, entry),
              napi_handle_scope_mismatch, "P2-8",
              "leaked scope detected");
  Check(Depth() == entry + 1, "P2-8", "extras unwound to entry+1");
  Check(Cnt() == Base(), "P2-8", "leaked roots released by unwinding");
  Check(NodeApiScopes_IsOpen(g_env, impl) == 1, "P2-8", "implicit still open");
  CheckStatus(napi_close_handle_scope(g_env, impl), napi_ok, "P2-8",
              "thunk closes implicit normally");
  /* Arm 2: callback closes the implicit scope itself (underflow). */
  napi_handle_scope impl2 = nullptr;
  NodeApiScopes_OpenImplicit(g_env, &impl2);
  napi_close_handle_scope(g_env, impl2);
  CheckStatus(NodeApiScopes_EnforceImplicitBalance(g_env, entry),
              napi_handle_scope_mismatch, "P2-8",
              "underflow detected");
  Check(Depth() == entry, "P2-8", "underflow leaves stack as-is");
  Check(Cnt() == Base(), "P2-8", "counter at baseline after probe");
}

/* ---- P2-9: bad handles / NULL args => invalid_arg ---- */
void P2_9() {
  CheckStatus(napi_close_handle_scope(g_env, nullptr), napi_invalid_arg,
              "P2-9", "close NULL scope");
  CheckStatus(napi_close_handle_scope(nullptr,
                                      reinterpret_cast<napi_handle_scope>(1)),
              napi_invalid_arg, "P2-9", "close with NULL env");
  napi_handle_scope s = nullptr;
  CheckStatus(napi_open_handle_scope(g_env, nullptr), napi_invalid_arg,
              "P2-9", "open with NULL result");
  CheckStatus(napi_open_handle_scope(nullptr, &s), napi_invalid_arg, "P2-9",
              "open with NULL env");
  napi_value out = nullptr;
  napi_value v = nullptr; /* deliberate NULL escapee */
  napi_escapable_handle_scope esc = nullptr;
  napi_open_escapable_handle_scope(g_env, &esc);
  CheckStatus(napi_escape_handle(nullptr, esc, nullptr, &out),
              napi_invalid_arg, "P2-9", "escape with NULL env");
  CheckStatus(napi_escape_handle(g_env, esc, v, &out), napi_invalid_arg,
              "P2-9", "escape with NULL escapee");
  CheckStatus(napi_escape_handle(g_env, esc, reinterpret_cast<napi_value>(1),
                                 nullptr),
              napi_invalid_arg, "P2-9", "escape with NULL result");
  CheckStatus(napi_close_escapable_handle_scope(g_env, esc), napi_ok, "P2-9",
              "cleanup close");
  Check(Cnt() == Base(), "P2-9", "counter at baseline after probe");
}

/* ---- P2-10: RootedValue RAII (lifetime, move, scope-close no-op) ---- */
void P2_10() {
  napi_handle_scope s = nullptr;
  napi_open_handle_scope(g_env, &s);
  napi_value v = CreateObj(); /* +1 (value lane) */
  {
    NodeApiScopes::RootedValue rv(g_env, v); /* +1 (explicit slot) */
    Check(rv.active() && Cnt() == Base() + 2, "P2-10",
          "wrapper roots a second slot");
    NodeApiScopes::RootedValue moved(std::move(rv));
    Check(!rv.active() && moved.active() && Cnt() == Base() + 2, "P2-10",
          "move transfers the slot");
  } /* moved destroyed */
  Check(Cnt() == Base() + 1, "P2-10", "wrapper dtor unroots one slot");
  { /* Wrapper destroyed AFTER its scope closed must be a safe no-op. */
    NodeApiScopes::RootedValue rv2(g_env, v);
    CheckStatus(napi_close_handle_scope(g_env, s), napi_ok, "P2-10",
                "close scope under live wrapper");
    s = nullptr;
  } /* rv2 dtor here: slot already released by close */
  Check(Cnt() == Base(), "P2-10",
        "post-close wrapper dtor releases nothing (no double)");
  /* Heap not corrupted: another create + GC still works. */
  napi_handle_scope s2 = nullptr;
  napi_open_handle_scope(g_env, &s2);
  CreateObj();
  napi_close_handle_scope(g_env, s2);
  ForceGC();
  Check(Cnt() == Base(), "P2-10", "counter at baseline after probe");
}

/* ---- P2-1: default scope (create with no explicit scope) ---- */
__attribute__((noinline)) void P2_1() {
  /* g_env is fresh here (only the default scope exists). */
  Check(Depth() == 1, "P2-1", "only default scope open");
  napi_value obj = CreateObj(); /* roots into default */
  napi_value num = nullptr;
  CheckStatus(napi_create_double(g_env, 3.5, &num), napi_ok, "P2-1",
              "create double in default scope");
  Check(Cnt() == Base() + 2, "P2-1", "both values rooted in default");
  /* Usable after the "native call" returns: type + value reads work. */
  JsValueType t = JsUndefined;
  JsGetValueType(reinterpret_cast<JsValueRef>(obj), &t);
  Check(t == JsObject, "P2-1", "default-scoped object still usable");
  JsValueRef numAsNumber = JS_INVALID_REFERENCE;
  JsConvertValueToNumber(reinterpret_cast<JsValueRef>(num), &numAsNumber);
  JsValueType nt = JsUndefined;
  JsGetValueType(numAsNumber, &nt);
  Check(nt == JsNumber, "P2-1", "default-scoped number readable");
  JsWeakRef wo = JS_INVALID_REFERENCE;
  JsCreateWeakReference(reinterpret_cast<JsValueRef>(obj), &wo);
  JsAddRef(wo, nullptr);
  obj = nullptr;
  num = nullptr;
  ClearStack();
  ForceGC();
  Check(WeakAlive(wo), "P2-1", "default-scoped value survives GC");
  JsRelease(wo, nullptr);
  /* Baseline restoration happens in destroy (step 6 closes the default
   * scope). The caller destroys and re-creates the env, asserting the new
   * baseline -- the old env is freed, so no query is legal on it. */
  std::printf("[INFO] P2-1: default scope held %zu roots until destroy\n",
              Cnt());
}

} /* namespace */

int main() {
  if (JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &g_runtime) != JsNoError ||
      JsCreateContext(g_runtime, &g_ctx) != JsNoError ||
      JsSetCurrentContext(g_ctx) != JsNoError) {
    std::printf("[FAIL] runtime/context setup\n");
    return 1;
  }
  g_env = ScopesProbe_CreateEnv(g_runtime, g_ctx);
  if (g_env == nullptr) {
    std::printf("[FAIL] env creation\n");
    return 1;
  }
  std::printf("baseline=%zu depth=%zu\n", Base(), Depth());
  Check(Base() == 0 && Depth() == 1, "env", "fresh env: baseline 0, depth 1 (default)");

  P2_3();
  P2_2();
  P2_4();
  P2_4b();
  P2_5();
  P2_5b();
  P2_6();
  P2_7();
  P2_8();
  P2_9();
  P2_10();

  /* P2-1 on a fresh env (defaults released by destroy). */
  ScopesProbe_DestroyEnv(g_env);
  g_env = ScopesProbe_CreateEnv(g_runtime, g_ctx);
  P2_1();
  ScopesProbe_DestroyEnv(g_env);
  g_env = ScopesProbe_CreateEnv(g_runtime, g_ctx);
  Check(Cnt() == Base() && Base() == 0, "P2-1",
        "baseline restored on env after destroy (fresh env baseline 0)");

  /* P2-11: destroy with scopes still open (step 6 closes everything). */
  {
    napi_handle_scope s = nullptr;
    napi_open_handle_scope(g_env, &s);
    CreateObj();
    CreateObj();
    Check(Cnt() == Base() + 2, "P2-11", "roots live at destroy time");
    ScopesProbe_DestroyEnv(g_env);
    g_env = nullptr;
    std::printf("[PASS] P2-11: destroy closed open scopes without crash\n");
  }

  JsSetCurrentContext(JS_INVALID_REFERENCE);
  JsDisposeRuntime(g_runtime);
  std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
