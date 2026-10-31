//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// host-spawn-check repro: $262.agent.start while the host cannot create
// the agent thread (doc/c2-s6-cross-agent-waitasync.md, "S6e host spawn
// check" section).
//
// The driver (drive.sh) injects a transient thread-exhaustion window via
// failthreads.c: ch's pthread budget covers startup (3 creates measured
// on the test profile, 2 on debug) plus the phase-1 agents, then exactly
// ONE pthread_create is refused with EAGAIN — the same errno a
// thread-exhausted kernel returns — and later creates succeed again
// (agents parking frees nothing here; the window is the interposer's,
// modelling churn). Because the startup count differs per profile, the
// refused spawn is "the first one past the budget, wherever it lands in
// the loop below" — the loop absorbs that variance (old fixed-iteration
// versions put the refusal outside the try on debug).
//
//   pre-fix:  the refused spawn hangs the main thread forever in
//             WaitForSingleObject(hevntInitialScriptCompleted) at
//             WScriptJsrt.cpp:879 — nothing can set the parent's event,
//             because the child that would set it never started. The
//             driver's timeout classifies this run as a hang.
//   post-fix: the refused spawn throws a catchable exception ("Failed to
//             create the agent thread."), the next spawn succeeds (the
//             host's spawn state is consistent again), and end-of-main
//             still joins every successfully started agent. exit 0.
//
// Run (via the driver; a bare run without the interposer just prints the
// ERROR line): <ch> -Test262 tools/host-spawn-check/spawnfail.js
// Not wired into the gates: thread-exhaustion is not deterministically
// reachable in-gate (see README.md).

var PHASE1 = 5;
var ok = 0;
for (var i = 0; i < PHASE1; i++) {
    $262.agent.start("var parked = true;");
    ok++;
}
print("phase1=" + ok);

// Spawns continue until one is refused (the driver denies exactly one
// create somewhere in this window). Pre-fix the first refused spawn
// never returns; post-fix each refusal is a catchable exception.
var caught = false;
var tries = 0;
while (!caught && tries < 6) {
    tries++;
    try {
        $262.agent.start("var parked = true;");
        print("spawn-ok=" + tries);
    } catch (e) {
        print("caught=" + e.message);
        caught = true;
    }
}
if (!caught) {
    print("ERROR: no spawn refused (no thread budget injected?)");
}

// Consistency probe: the transient window is over. If the failed spawn
// had left inconsistent state (a stale child in children, a wedged
// event), this spawn or the end-of-main join below would misbehave.
$262.agent.start("var parked = true;");
print("phase2=1");
// Main ends: Uninitialize signals every live agent's shutdown event and
// joins each thread handle individually (host-exit-join repair).
