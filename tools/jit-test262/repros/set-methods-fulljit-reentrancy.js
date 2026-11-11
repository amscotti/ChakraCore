// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
// Repro C2-JIT-1: Set methods abort under a JIT-compiled caller
// (fulljit / dynapogo flags). Recorded by doc/c2-jit-test262.md; do not fix
// here — fixes get their own lane.
//
//   out/jit/bin/ch -nonative                                           set-methods-fulljit-reentrancy.js   -> prints 2, exit 0
//   out/jit/bin/ch -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- set-methods-fulljit-reentrancy.js   -> prints 2, exit 0
//   out/jit/bin/ch -forceNative -off:simpleJit -bgJitDelay:0          set-methods-fulljit-reentrancy.js   -> SIGABRT (exit 134), no output
//
// All seven Set.prototype methods (union, intersection, difference,
// symmetricDifference, isSubsetOf, isSupersetOf, isDisjointFrom) abort the
// same way when called from JIT-compiled code; `other` may be a real Set or
// a plain set-like object. macOS crash report backtrace:
//   Js::JavascriptSet::EntryIsSubsetOf
//   -> Js::SetMethodCallHas
//   -> ThreadContext::CheckAndResetReentrancySafeOrHandled
//   -> Js::Throw::FatalInternalError -> abort
//
// Suspected area: lib/Runtime/Library/JavascriptSet.cpp's Set-method helpers
// (SetMethodCallHas / GetSetLikeKeysIterator / SetLikeKeysStep) re-enter JS
// through CALL_FUNCTION inside JS_REENTRANCY_LOCK/JS_REENTRANT without
// BEGIN_SAFE_REENTRANT_CALL. The interpreter's OP_Call wraps every call in
// BEGIN_SAFE_REENTRANT_REGION (InterpreterStackFrame.cpp), so the
// interpreter path passes; a JIT-compiled caller enters the C++ builtin with
// neither reentrancySafeOrHandled nor isInReentrancySafeRegion set, and the
// first C++->JS CALL_FUNCTION fails fast. JavascriptMap::EntryForEach
// (lib/Runtime/Library/JavascriptMap.cpp:179) shows the arming pattern the
// Set helpers lack.

var a = new Set([1]);
var b = new Set([2]);
print(a.union(b).size); // 3 under the interpreter; SIGABRT under fulljit
print("unreachable under fulljit");
