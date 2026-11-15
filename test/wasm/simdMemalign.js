//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("../UnitTestFramework/UnitTestFramework.js");

// A7-2: a wasm memarg alignment is a log2 exponent, and the v128 natural
// alignment is 16 bytes = exponent 4, so v128.load with align 5..16 must be
// rejected at validation. EmitSimdMemAccess used to gate at 16 (bytes),
// silently accepting exponents 5..16. Requires -wasmsimd (Simd gate).

// (module (memory 1) (func (drop (v128.load align=N offset=0 (i32.const 0)))))
function simdLoadModule(align) {
    return new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x0b, 0x01, 0x09, 0x00, 0x41, 0x00, 0xfd, 0x00, align, 0x00, 0x1a, 0x0b
    ]);
}

var tests = [
    {
        name: "v128.load with natural-exponent align=4 validates and instantiates",
        body() {
            var bytes = simdLoadModule(4);
            assert.isTrue(WebAssembly.validate(bytes));
            var module = new WebAssembly.Module(bytes);
            var instance = new WebAssembly.Instance(module, {});
            assert.isTrue(instance !== null && typeof instance === "object");
        }
    },
    {
        name: "v128.load with align=5 is rejected (exponent, not bytes)",
        body() {
            var bytes = simdLoadModule(5);
            assert.isFalse(WebAssembly.validate(bytes));
            assert.throws(function () { new WebAssembly.Module(bytes); },
                WebAssembly.CompileError);
        }
    },
    {
        name: "v128.load with align=16 is rejected (old byte-compare boundary)",
        body() {
            var bytes = simdLoadModule(16);
            assert.isFalse(WebAssembly.validate(bytes));
            assert.throws(function () { new WebAssembly.Module(bytes); },
                WebAssembly.CompileError);
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
