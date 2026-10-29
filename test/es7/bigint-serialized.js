//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S2 default-enablement: BigInt literal constants must survive bytecode
// serialization/deserialization (ch -Serialized, JsSerialize, parser-state
// cache). This entry runs the whole script through the serialized path under
// the DEFAULT configuration, so both the writer (AddConstantTable/
// PrependVarConstant) and the reader (ReadConstantsTable) are exercised for
// decimal/non-decimal/negative/multi-limb literals, array literals, literal
// property names, arithmetic on restored constants and deferred function
// bodies. See doc/c2-s2-bigint-default.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var decimal = 123n;
var negative = -456n;
var zero = 0n;
var hex = 0xffn;
var octal = 0o17n;
var binary = 0b101n;
var separated = 1_000_000n;
var multiLimb = 12345678901234567890123456789012345678901234567890n;
var bigHex = 0x8000000000000000n;
var int64Min = -9223372036854775808n;
var named = { 1n: "one", 0x10n: "sixteen" };

function deferred(a, b) {
    // Deferred function body: the literals below are restored from the
    // serialized constant table only when this function is first called.
    return [a + 0xffn, b * -2n, 2n ** 128n];
}

var tests = [
    {
        name: "Serialized literal constants round-trip exactly",
        body() {
            assert.strictEqual(123n, decimal, "decimal");
            assert.strictEqual(-456n, negative, "negative decimal");
            assert.strictEqual(0n, zero, "zero");
            assert.strictEqual(255n, hex, "hex");
            assert.strictEqual(15n, octal, "octal");
            assert.strictEqual(5n, binary, "binary");
            assert.strictEqual(1000000n, separated, "numeric separators");
            assert.strictEqual(12345678901234567890123456789012345678901234567890n, multiLimb, "multi-limb");
            assert.strictEqual(9223372036854775808n, bigHex, "2^63 hex literal is positive");
            assert.strictEqual(-9223372036854775808n, int64Min, "int64 min decimal");
            assert.strictEqual("one", named["1"], "decimal literal property name");
            assert.strictEqual("sixteen", named["16"], "hex literal property name uses decimal key");
        }
    },
    {
        name: "Restored constants behave as values",
        body() {
            assert.strictEqual("bigint", typeof decimal, "typeof restored constant");
            assert.strictEqual("123", decimal.toString(), "toString");
            assert.strictEqual(378n, decimal * 3n + 9n, "arithmetic on restored constants");
            assert.isTrue(decimal === 123n, "value identity across the round trip");
            assert.isTrue(BigInt("123") === decimal, "equal to a runtime-constructed value");
            assert.strictEqual(-9223372036854775808n, int64Min | 0n, "bitwise on restored constant");
            assert.strictEqual(9223372036854775808n, bigHex | 0n, "bitwise keeps the positive hex value");
        }
    },
    {
        name: "Deferred function literals restore on first call",
        body() {
            var results = deferred(1n, 21n);
            assert.strictEqual(256n, results[0], "hex literal inside deferred body");
            assert.strictEqual(-42n, results[1], "negative literal inside deferred body");
            assert.strictEqual(340282366920938463463374607431768211456n, results[2], "exponentiation inside deferred body");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
