//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft Corporation and contributors. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Pins AsmJsLink::CheckParams argument routing (A7-1): CheckFFI must validate
// the foreign (FFI) argument, not stdlib. Case 1 (non-object foreign with
// imports) must fail at link; case 2 (Proxy stdlib with valid foreign and no
// stdlib builtins used) must link; case 3 (Proxy foreign) must fail at link.
// Run with -testtrace:asmjs; the link trace lines are the pin.
// NOTE: foreign imports only accept extern (constant) call arguments, hence
// the constant call shapes below.

var stdlib = {Math:Math,Int8Array:Int8Array,Int16Array:Int16Array,Int32Array:Int32Array,Uint8Array:Uint8Array,Uint16Array:Uint16Array,Uint32Array:Uint32Array,Float32Array:Float32Array,Float64Array:Float64Array,Infinity:Infinity,NaN:NaN};
var env = {"f":function(x){print("ffi:" + x);}};
var buffer = new ArrayBuffer(1<<20);

function AsmModuleBadFFI(stdlib, foreign, buffer) {
    "use asm";
    var f = foreign.f;
    function g() {
        f(1);
    }
    return { g: g };
}
var m1 = AsmModuleBadFFI(stdlib, 5, buffer);
print("case1 linked");

function AsmModuleProxyStdlib(stdlib, foreign, buffer) {
    "use asm";
    var f = foreign.f;
    function g() {
        f(7);
    }
    return { g: g };
}
var m2 = AsmModuleProxyStdlib(new Proxy(stdlib, {}), env, buffer);
m2.g();

function AsmModuleProxyFFI(stdlib, foreign, buffer) {
    "use asm";
    var f = foreign.f;
    function g() {
        f(1);
    }
    return { g: g };
}
var m3 = AsmModuleProxyFFI(stdlib, new Proxy(env, {}), buffer);
print("case3 linked");
