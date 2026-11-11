//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("../UnitTestFramework/UnitTestFramework.js");

// A valid module with one empty function, and the same module with an invalid
// opcode (0xff) in the function body. Deferred compilation used to skip
// validation at construction, so the invalid bytes produced a Module that only
// threw on the first call. See doc/b4-swarm-handoff.md D4.

var validModule = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
]);

var invalidModule = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x0a, 0x05, 0x01, 0x03, 0x00, 0xff, 0x0b
]);

// The same module with an exported function returning 42. Constructing the
// Module validates the deferred body; calling the export then runs the real
// deferred compile, which must still own the body's frame layout.
var exportedModule = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b
]);

// A module with an imported function and a deferred body that calls it. This
// is the import/instance shape that forced the earlier eager-validation
// revert; construction must validate the body without rejecting the imports,
// and the first call must still compile it.
var importedModule = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x02, 0x09, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x01, 0x66, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x67, 0x00, 0x01,
    0x0a, 0x08, 0x01, 0x06, 0x00, 0x20, 0x00, 0x10, 0x00, 0x0b
]);

var tests = [
    {
        name: "WebAssembly.Module rejects an invalid deferred function body at construction",
        body() {
            assert.isTrue(WebAssembly.validate(validModule));
            assert.isFalse(WebAssembly.validate(invalidModule));
            assert.doesNotThrow(function () { new WebAssembly.Module(validModule); });
            assert.throws(function () { new WebAssembly.Module(invalidModule); },
                WebAssembly.CompileError);
        }
    },
    {
        name: "a valid module is still instantiable after construction-time validation",
        body() {
            var module = new WebAssembly.Module(validModule);
            var instance = new WebAssembly.Instance(module);
            assert.isTrue(instance !== null && typeof instance === "object");
            assert.isTrue(instance.exports !== null && typeof instance.exports === "object");
        }
    },
    {
        name: "a validated deferred body still compiles and runs on first call",
        body() {
            var module = new WebAssembly.Module(exportedModule);
            var instance = new WebAssembly.Instance(module);
            assert.strictEqual(42, instance.exports.f());
            assert.strictEqual(42, instance.exports.f());
        }
    },
    {
        name: "a valid import/instance module still validates and calls through",
        body() {
            var module = new WebAssembly.Module(importedModule);
            var instance = new WebAssembly.Instance(module, {
                env: { f: function (x) { return x + 1; } }
            });
            assert.strictEqual(42, instance.exports.g(41));
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
