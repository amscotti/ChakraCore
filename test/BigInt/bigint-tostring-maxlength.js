//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// RV-03 regression: BigInt.prototype.toString must throw RangeError when the
// resulting char16 buffer or scratch buffer would reach Recycler's 2 GiB
// LargeAlloc threshold (resultLength + 1 >= 2**30) instead of aborting.

if (this.WScript && this.WScript.LoadScriptFile) {
    this.WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");
}

var has64BitCap = false;
try {
    var probe = 1n << 536870912n;
    probe = null;
    has64BitCap = true;
} catch (e) {
    assert.isTrue(e instanceof RangeError);
}

var tests = [
    {
        name: "BigInt.prototype.toString throws RangeError at 2 GiB char16 boundary",
        body: function () {
            if (!has64BitCap) {
                return;
            }

            // 1n << 1073741822n needs 1073741823 digits + NUL = 2**30 char16s (2 GiB).
            var tooLongPos = 1n << 1073741822n;
            assert.throws(function () { tooLongPos.toString(2); }, RangeError);
            tooLongPos = null;

            // -(1n << 1073741821n) adds '-' -> 1073741823 chars + NUL = 2**30 char16s (2 GiB).
            var tooLongNeg = -(1n << 1073741821n);
            assert.throws(function () { tooLongNeg.toString(2); }, RangeError);
            tooLongNeg = null;

            // Max 64-bit BigInt (2**30 bits) must throw RangeError cleanly across
            // both pow2 and generic non-pow2 scratch paths instead of aborting.
            var maxVal = 1n << 1073741823n;
            assert.throws(function () { maxVal.toString(2); }, RangeError);
            assert.throws(function () { maxVal.toString(); }, RangeError);
            assert.throws(function () { maxVal.toString(10); }, RangeError);
            assert.throws(function () { maxVal.toString(3); }, RangeError);

            // Base 16 of 1n << 1073741823n has 2**28 chars (512 MiB), well under 2 GiB.
            var hex = maxVal.toString(16);
            assert.strictEqual(268435456, hex.length);
            assert.strictEqual("8000", hex.slice(0, 4));
            assert.strictEqual("0000", hex.slice(-4));
            hex = null;
            maxVal = null;
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
