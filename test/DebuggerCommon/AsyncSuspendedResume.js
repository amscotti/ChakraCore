//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// A plain async function suspended at an await must resume and complete
// correctly after a dynamic debugger attach reparses the function for
// debugging, and the same function must still run under the debug bytecode
// for a new invocation while the first one is suspended (the async
// locals/stepping baseline). See doc/dbg-async-attach.md.

var resolveIt;
var p = new Promise(function (r) { resolveIt = r; });

async function f() {
    WScript.Echo("before");
    var v = await p;
    WScript.Echo("after " + v);
}

f();

WScript.Attach(function () {
    // While the first f() is suspended at `await p`, call f() again: this
    // invocation must execute on the freshly generated debug bytecode.
    f();
    resolveIt(41);
});

WScript.Echo("done");
