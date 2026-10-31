// Agentless valid-broadcast leak probe (host-broadcast-type repair).
//
// WScript.Broadcast over a real SharedArrayBuffer with NO $262.agent
// children: threadData is null (it is created only at the crossthread
// inject, WScriptJsrt.cpp), so the parent-side handshake — including the
// content-handle release — never runs. The repair's validation extracts
// a content handle, which AddRefs on success (JsrtCore.cpp:
// (*content)->AddRef()); pre-repair each agentless valid broadcast
// therefore leaked one SharedContents reference, pinning that
// iteration's fresh 64 KiB buffer until runtime disposal.
//
// Mode "control" runs the identical allocation loop with no broadcast.
// Compare peak RSS between the two modes.
//
// Usage: ch -ESSharedArrayBuffer -args <broadcast|control> [iters] -endargs leakprobe.js

var mode = WScript.Arguments[0] || "broadcast";
var iters = parseInt(WScript.Arguments[1]) || 6000;
for (var i = 0; i < iters; i++) {
    var sab = new SharedArrayBuffer(64 * 1024);
    if (mode !== "control") {
        WScript.Broadcast(sab);
    }
}
print(mode + " " + iters + " done");
