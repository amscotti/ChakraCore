// Repro C2-JIT-2: Object.groupBy / Map.groupBy abort under a JIT-compiled
// caller (fulljit / dynapogo flags). Recorded by doc/c2-jit-test262.md; do
// not fix here — fixes get their own lane.
//
//   out/jit/bin/ch -nonative                                           groupby-fulljit-reentrancy.js   -> prints 1, exit 0
//   out/jit/bin/ch -maxInterpretCount:1 -maxSimpleJitRunCount:1 -bgjit- groupby-fulljit-reentrancy.js   -> prints 1, exit 0
//   out/jit/bin/ch -forceNative -off:simpleJit -bgJitDelay:0          groupby-fulljit-reentrancy.js   -> SIGABRT (exit 134), no output
//
// Same failure site family as C2-JIT-1 (crash report backtrace
// Js::JavascriptObject::EntryGroupBy -> MapGroupByCallCallback ->
// ThreadContext::CheckAndResetReentrancySafeOrHandled ->
// Js::Throw::FatalInternalError -> abort): MapGroupByCallCallback
// (lib/Runtime/Library/JavascriptMap.cpp, shared by Object.groupBy and
// Map.groupBy) calls the user callback through CALL_FUNCTION inside
// JS_REENTRANCY_LOCK/JS_REENTRANT without BEGIN_SAFE_REENTRANT_CALL, which
// only survives when the builtin is entered from the interpreter's
// OP_Call safe-reentrancy region.

var cb = function (x) { return x % 2; };
print(Object.groupBy([1, 2], cb)[1].length); // 1 under the interpreter; SIGABRT under fulljit
print("unreachable under fulljit");
