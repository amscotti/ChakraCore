//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function print(x) { WScript.Echo(x+''); }

const f = 'const f';
let g = 'let g';

try { eval("if (false) { function f() { } }"); } catch (e) { print(e.name + ': ' + e.message); }
try { eval("if (true) { function g() { } }"); } catch (e) { print(e.name + ': ' + e.message); }


print(f);
print(g);

function h(global) { }
eval("function h(ineval) { }");

print(h);

{
    function i(globalblock) { }
    try { eval("function i(ineval) { }"); } catch (e) { print(e.name + ': ' + e.message); }
    print(i);
}
print(i);

function j(global) { }

{
    function j(globalblock) { }
    try { eval("function j(ineval) { }"); } catch (e) { print(e.name + ': ' + e.message); }
    print(j);
}
print(j);
