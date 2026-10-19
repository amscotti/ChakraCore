//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function print(x) { WScript.Echo(x+''); }

const f = 'const f';
let g = 'let g';

// The block function names do not collide with the outer const/let, so no
// Annex B var extension is requested in the eval's global variable environment.
eval("if (false) { function fe() { } }");
eval("if (true) { function ge() { } }");


print(f);
print(g);

function h(global) { }
eval("function h(ineval) { }");

print(h);

{
    function ib(globalblock) { }
    eval("function i(ineval) { }");
    print(i);
}
print(i);

function j(global) { }

{
    function jb(globalblock) { }
    eval("function j(ineval) { }");
    print(j);
}
print(j);

