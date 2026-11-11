//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------


if (this.WScript && this.WScript.LoadScriptFile) { // Check for running in ch
    this.WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");
}

var tests = [
    {
        name: "Test BigInt global object properties",
        body: function () {
            assert.isTrue(BigInt.length == 1);
            assert.isTrue(BigInt.name == "BigInt");
            // ES2020: BigInt.prototype is an ordinary object with
            // @@toStringTag "BigInt"; it has no [[BigIntData]], so the
            // BigInt prototype valueOf/toString throw instead of falling
            // back to Object.prototype helpers (Node 26.5.0 agrees).
            assert.isTrue(Object.prototype.toString.call(BigInt.prototype) == "[object BigInt]");
            assert.throws(function () { BigInt.prototype == "[object Object]"; }, TypeError);
            assert.isTrue(BigInt.prototype.constructor === BigInt);
            assert.isTrue(BigInt.__proto__ === Function.prototype);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
