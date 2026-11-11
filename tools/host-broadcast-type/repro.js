// -------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
// -------------------------------------------------------------------------------------------------------
var sab = new SharedArrayBuffer(4);
$262.agent.start(`
    $262.agent.receiveBroadcast(function(s) { $262.agent.report("got " + (s ? s.byteLength : s)); $262.agent.leaving(); });
`);
$262.agent.sleep(200);
var i32 = new Int32Array(sab);
try { WScript.Broadcast(i32); print("broadcast returned"); } catch (e) { print("threw: " + e); }
print("survived");
