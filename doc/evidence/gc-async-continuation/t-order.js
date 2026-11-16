// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
// Deterministic (timing-free) microtask-vs-timer ordering probe:
// after timer1's callback returns, the promise continuation (microtask)
// must run before timer2 (another macrotask). Output must be:
// f-start / f-resumed / timer2 / END
var out = [];
async function f() {
    out.push("f-start");
    var v = await Promise.resolve(1);
    out.push("f-resumed:" + v);
}
WScript.SetTimeout(function () { f(); }, 0);
WScript.SetTimeout(function () { out.push("timer2"); }, 0);
WScript.SetTimeout(function () { out.forEach(function (s) { WScript.Echo(s); }); WScript.Echo("END"); }, 0);
WScript.Echo("scriptend");
