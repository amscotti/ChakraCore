//---------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//---------------------------------------------------------------------------------------------------------
// host-map-sync repro (swarm-triage R1): the host's process-global module/
// script maps (moduleRecordMap, moduleDirMap, moduleErrMap, scriptDirMap)
// and the source-context counter in bin/ch/WScriptJsrt.cpp are hit from
// crossthread agent threads concurrently with the main thread.
//
// Each agent, on its own thread:
//   - reports "started";
//   - LoadScript(..., "module") a string module (LoadModuleFromString ->
//     moduleRecordMap/moduleErrMap insert + parse);
//   - LoadScript(..., "self") a string script: mints a source context,
//     registers scriptDirMap, and the loaded script itself dynamically
//     imports a per-agent module file (fetch callbacks -> module maps);
//   - reports "ok" when its dynamic import resolves ("wrongval" on a bad
//     namespace, "err:<reason>" on rejection), then marks itself leaving.
// The main thread concurrently fires its own dynamic import (dep-main.js,
// resolved through the main script's scriptDirMap entry) while spawning.
//
// Pre-fix behavior (two bugs, one visible each):
//   - unsynchronized map mutation: concurrent std::map writes corrupt the
//     heap (crash / debug assert) under agent churn;
//   - PushMessage routed module fetch messages to the MAIN queue
//     unconditionally, so an agent's dynamic import never settled on its
//     own thread (silent promise loss: the agent reports "started" and
//     then nothing).
//
// Run:  sh tools/host-map-sync/drive.sh <ch-binary> [runs] [agents]
//       (drive.sh builds the per-agent dep files in a scratch workdir and
//       runs ch with that as CWD; agents resolve relative specifiers
//       against CWD because agent sources carry no script directory).
// Want: exit code 0, stdout ending "agents=<N> started=<N> importOk=<N>
//       importErr=0", no ASSERTION lines.

var AGENTS = parseInt(WScript.Arguments[0]) || 40;

// Main-thread concurrent fetch: content against the shared maps while the
// agents spawn. Resolves after the main script ends (ch drains the main
// queue then), so only its side effects matter, not a reported value.
import("./dep-main.js").then(null, function () {});

for (var i = 0; i < AGENTS; i++) {
    $262.agent.start(
        "$262.agent.report('started');" +
        // String module load on the agent thread: map insert + parse.
        "WScript.LoadScript('export const q = " + i + ";', 'module', 'agentStrMod" + i + ".js');" +
        // Self script load: source-context mint + scriptDirMap register;
        // the loaded script's dynamic import resolves through those maps.
        // The import settles from the agent's own queue drain (the fetch
        // and evaluation messages are per-thread since R1); the reporting
        // happens in the continuations, after the initial script returns.
        "WScript.LoadScript(\"import('./dep-" + i + ".js').then(" +
        "function (m) { $262.agent.report(m.v === " + i + " ? 'ok' : 'wrongval'); $262.agent.leaving(); }, " +
        "function (e) { $262.agent.report('err:' + String(e)); $262.agent.leaving(); });\", 'self');");
}

var started = 0, importOk = 0, importErr = 0;
var deadline = Date.now() + 20000;
while (Date.now() < deadline &&
       (started < AGENTS || importOk + importErr < AGENTS)) {
    var r = $262.agent.getReport();
    if (r === null) {
        $262.agent.sleep(10);
        continue;
    }
    if (r === "ok") { importOk++; }
    else if (r.indexOf("err:") === 0) { importErr++; }
    else if (r === "started") { started++; }
}

print("agents=" + AGENTS + " started=" + started +
      " importOk=" + importOk + " importErr=" + importErr);
if (started !== AGENTS || importOk !== AGENTS || importErr !== 0) {
    print("HOST-MAP-SYNC-LOSS: expected started=" + AGENTS +
          " importOk=" + AGENTS + " importErr=0");
}
// Main ends normally either way: ch's Uninitialize joins every agent (the
// host-exit-join repair), and drive.sh classifies wrong summaries, nonzero
// exits and ASSERTION lines as failures.
