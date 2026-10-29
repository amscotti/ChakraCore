//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S2 default-enablement: disable-path coverage. This file deliberately
// contains NO BigInt syntax at the top level; it is registered with
// -ESBigInt- and verifies that dynamically compiled BigInt source is a clean
// SyntaxError (a parse error, not a crash or a silent accept) and that the
// BigInt global surface is absent while the flag is disabled. Default
// availability is covered by bigint-default.js. See
// doc/c2-s2-bigint-default.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "BigInt global surface is absent with -ESBigInt-",
        body() {
            assert.strictEqual("undefined", typeof BigInt, "typeof BigInt");
            assert.strictEqual("undefined", typeof BigInt64Array, "typeof BigInt64Array");
            assert.strictEqual("undefined", typeof BigUint64Array, "typeof BigUint64Array");
            assert.strictEqual("undefined", typeof DataView.prototype.getBigInt64, "DataView getBigInt64");
            assert.strictEqual("undefined", typeof DataView.prototype.setBigUint64, "DataView setBigUint64");
        }
    },
    {
        name: "eval of BigInt literals throws SyntaxError",
        body() {
            assert.throws(() => eval("1n"), SyntaxError, "decimal literal");
            assert.throws(() => eval("0xffn"), SyntaxError, "hex literal");
            assert.throws(() => eval("1_000n"), SyntaxError, "separated literal");
            assert.throws(() => eval("({ 1n: 1 })"), SyntaxError, "literal property name");
        }
    },
    {
        name: "Function constructor with BigInt source throws SyntaxError",
        body() {
            assert.throws(() => Function("return 1n"), SyntaxError, "Function body");
            assert.throws(() => Function("a", "return a + 0x10n"), SyntaxError, "Function parameter list/body");
        }
    },
    {
        name: "Non-BigInt dynamic code is unaffected",
        body() {
            assert.strictEqual(2, eval("1 + 1"), "eval still works");
            assert.strictEqual(3, Function("a", "return a * 1.5")(2), "Function still works");
            assert.strictEqual("4", eval("2 + 2").toString(), "eval result");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
