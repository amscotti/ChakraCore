//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Exact double-to-decimal conversion for every requested digit count (the
// ES2018 0..100 range), including subnormals and the legacy formatter's
// rounding boundaries, plus exact integer radix conversion. Expectations
// below were produced by an independent correctly-rounded implementation
// (Node/V8) and by exact Python integer conversion; see
// doc/b4-swarm-handoff.md D2/D3.

function exactRadixString(value, radix) {
    var digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    var negative = value < 0;
    var n = BigInt(negative ? -value : value);
    var r = BigInt(radix);
    var out = "";
    do {
        out = digits[Number(n % r)] + out;
        n = n / r;
    } while (n > 0n);
    return (negative ? "-" : "") + out;
}

var tests = [
    {
        name: "toFixed uses the exact decimal expansion",
        body() {
            assert.strictEqual("0.10000000000000000555", (0.1).toFixed(20));
            assert.strictEqual("0.1000000000000000055511151231257827021181583404541015625000000000000000000000000000000000000000000000", (0.1).toFixed(100));
            assert.strictEqual("100000000000000000000.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", (1e20).toFixed(100));
            assert.strictEqual("34376060434654027776", (34376060434654027776).toFixed(0));
            assert.strictEqual("34376060434654027776.00", (34376060434654027776).toFixed(2));
        }
    },
    {
        name: "toFixed rounds the magnitude half up, including values that round to zero",
        body() {
            assert.strictEqual("3", (2.5).toFixed(0));
            assert.strictEqual("-3", (-2.5).toFixed(0));
            assert.strictEqual("-1", (-0.5).toFixed(0));
            assert.strictEqual("1.00", (1.005).toFixed(2));
            assert.strictEqual("-0.00000000000000000000", (-1e-25).toFixed(20));
        }
    },
    {
        name: "toExponential is exact to 100 digits",
        body() {
            assert.strictEqual("1.00000000000000005551e-1", (0.1).toExponential(20));
            assert.strictEqual("1.0000000000000000555111512312578270211815834045410156250000000000000000000000000000000000000000000000e-1", (0.1).toExponential(100));
            assert.strictEqual("4.9e-324", (5e-324).toExponential(1));
            assert.strictEqual("4.9406564584124654417656879286822137236505980261432476442558568250067550727020875186529983636163599238e-324", (5e-324).toExponential(100));
            assert.strictEqual("3e+0", (2.5).toExponential(0));
            assert.strictEqual("-3e+0", (-2.5).toExponential(0));
            assert.strictEqual("1.0e+1", (9.99).toExponential(1));
            assert.strictEqual("2.36546973056460e+179", (2.365469730564605e179).toExponential(14));
            assert.strictEqual("9.999999999999999e-1", (0.9999999999999999).toExponential(15));
        }
    },
    {
        name: "toPrecision is exact to 100 significant digits",
        body() {
            assert.strictEqual("0.10000000000000000555", (0.1).toPrecision(20));
            assert.strictEqual("0.1000000000000000055511151231257827021181583404541015625000000000000000000000000000000000000000000000", (0.1).toPrecision(100));
            assert.strictEqual("1.797693134862315708145274237317043567980705675258449965989174768031572607800285387605895586327668782e+308", (1.7976931348623157e308).toPrecision(100));
            assert.strictEqual("4.9e-324", (5e-324).toPrecision(2));
            assert.strictEqual("10", (9.99).toPrecision(2));
            assert.strictEqual("1.0e+2", (99.9).toPrecision(2));
            assert.strictEqual("0.0000000000000000000", (0).toPrecision(20));
        }
    },
    {
        name: "toFixed/toPrecision/toExponential accept the ES2018 0..100 range",
        body() {
            assert.strictEqual("1." + "0".repeat(100), (1).toFixed(100));
            assert.strictEqual("1." + "0".repeat(99), (1).toPrecision(100));
            assert.strictEqual("1." + "0".repeat(100) + "e+0", (1).toExponential(100));
            assert.throws(function () { (1).toFixed(101); }, RangeError);
            assert.throws(function () { (1).toPrecision(101); }, RangeError);
            assert.throws(function () { (1).toExponential(101); }, RangeError);
            assert.throws(function () { (1).toFixed(-1); }, RangeError);
        }
    },
    {
        name: "Number.prototype.toString(radix) converts the integer part exactly",
        body() {
            var values = [1e20, 1e100, 1e308, Math.pow(2, 64) + 4096, 34376060434654027776];
            for (var i = 0; i < values.length; i++) {
                for (var radix = 2; radix <= 36; radix++) {
                    if (radix === 10) {
                        // Radix 10 uses the shortest round-trip path, not the
                        // exact integer conversion.
                        assert.strictEqual(String(values[i]), values[i].toString(10));
                        continue;
                    }
                    assert.strictEqual(exactRadixString(values[i], radix),
                        values[i].toString(radix),
                        "value " + values[i] + " radix " + radix);
                }
            }
            assert.strictEqual("0.1", (0.5).toString(2));
            assert.strictEqual("-ff", (-255).toString(16));
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
