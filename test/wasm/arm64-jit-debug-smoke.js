//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for jit-debug 0-bytecode WASM FunctionBody entry validity
// (AsmJsDefaultEntryThunk alongside AsmJsExternalEntryPoint) and AsmJsJitCall
// callee-saved register preservation across WASM -> JS import callbacks.

if (typeof WebAssembly === "object") {
    // Minimal module:
    // (module
    //   (import "m" "cb" (func $cb (param i32) (result i32)))
    //   (func $add1 (export "add1") (param i32) (result i32)
    //     (i32.add (local.get 0) (i32.const 1)))
    //   (func $callCb (export "callCb") (param i32) (result i32)
    //     (i32.add (call $add1 (call $cb (local.get 0))) (i32.const 10))))
    var bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, // magic
        0x01, 0x00, 0x00, 0x00, // version
        // Type section (1): 1 type: (i32) -> i32
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        // Import section (2): m.cb : func type 0
        0x02, 0x08, 0x01, 0x01, 0x6d, 0x02, 0x63, 0x62, 0x00, 0x00,
        // Function section (3): 2 funcs of type 0 (func 1 = add1, func 2 = callCb)
        0x03, 0x03, 0x02, 0x00, 0x00,
        // Export section (7): "add1" -> 1, "callCb" -> 2
        0x07, 0x11, 0x02,
        0x04, 0x61, 0x64, 0x64, 0x31, 0x00, 0x01,
        0x06, 0x63, 0x61, 0x6c, 0x6c, 0x43, 0x62, 0x00, 0x02,
        // Code section (10)
        0x0a, 0x15, 0x02,
        // func 1 ($add1): local.get 0, i32.const 1, i32.add, end
        0x07, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x0b,
        // func 2 ($callCb): local.get 0, call 0 ($cb), call 1 ($add1), i32.const 10, i32.add, end
        0x0b, 0x00, 0x20, 0x00, 0x10, 0x00, 0x10, 0x01, 0x41, 0x0a, 0x6a, 0x0b
    ]);

    var mod = new WebAssembly.Module(bytes);
    var cb = function (x) {
        return (x * 3) | 0;
    };
    var inst = new WebAssembly.Instance(mod, { m: { cb: cb } });

    function drive(n) {
        var a = n + 1;
        var b = n + 2;
        var c = n + 3;
        var d = n + 4;
        var e = n + 5;
        var f = n + 6;
        var res = inst.exports.callCb(n);
        if (res !== ((n * 3) + 11)) {
            throw new Error("unexpected wasm result: " + res);
        }
        return a + b + c + d + e + f + res;
    }

    for (var i = 0; i < 200; i++) {
        var got = drive(i);
        var exp = (6 * i + 21) + ((i * 3) + 11);
        if (got !== exp) {
            throw new Error("callee-saved / wasm mismatch at i=" + i + ": " + got + " !== " + exp);
        }
    }
    if (inst.exports.add1(41) !== 42) {
        throw new Error("add1 mismatch");
    }
}

print("PASS");
