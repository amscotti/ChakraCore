//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// host-exit-join repro: more than 64 agents still in threadData->children at
// end-of-main (doc/c2-s6-cross-agent-waitasync.md, "Pre-existing exit assert
// under very-high agent churn" residual).
//
// Agents are NEVER unlinked from the parent's children list (leaving() only
// sets the child's own `leaving` flag; the only removal is the parent's
// Uninitialize clear), so every agent started here is joined at end-of-main.
// 100 > MAXIMUM_WAIT_OBJECTS (64), which made the pre-fix single
// WaitForMultipleObjects(100, ...) fail outright (the PAL rejects nCount > 64,
// pal/src/synchmgr/wait.cpp): debug profiles asserted at the waitRet check and
// release/test profiles deleted RuntimeThreadData objects without ever having
// observed their threads exit.
//
// Mixes the two agent exit classes: even agents report then mark themselves
// leaving (they still park in their wait loop until the end-of-main shutdown
// signal — leaving alone does not terminate an agent), odd agents just finish
// their initial script and park. Both classes stay in children.
//
// Run:  <ch> -Test262 tools/host-exit-join/agents100.js
// Want: exit code 0, stdout exactly "agents-started=100", no ASSERTION lines.
// Not wired into the gates: 100 agent-runtime creations are too slow for the
// debug regression gate; use drive.sh for A/B evidence runs.

var AGENTS = 100;
for (var i = 0; i < AGENTS; i++) {
    if (i % 2 === 0) {
        $262.agent.start(`
            $262.agent.report("done");
            $262.agent.leaving();
        `);
    } else {
        $262.agent.start("var parked = true;");
    }
}
print("agents-started=" + AGENTS);
// Main ends. ch's Uninitialize now signals every child's shutdown event and
// joins each child's thread handle individually (post-fix); pre-fix it made
// one WaitForMultipleObjects call over all 100 handles.
