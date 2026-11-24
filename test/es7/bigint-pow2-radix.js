//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// PE-4 regression: O(N) power-of-2 BigInt <-> string conversions for radix
// 2, 4, 8, 16, 32, covering 0n, negatives, leading zeros, single-digit values,
// 32/64-bit limb bit boundaries, invalid digits, and large 65k/262k strings.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var ALPHABET = "0123456789abcdefghijklmnopqrstuv";

function referenceToPowerOf2String(value, radix) {
    if (value === 0n) {
        return "0";
    }
    var neg = value < 0n;
    var mag = neg ? -value : value;
    var bits = 0n;
    for (var r = BigInt(radix); r > 1n; r >>= 1n) {
        bits += 1n;
    }
    var mask = BigInt(radix - 1);
    var chars = [];
    while (mag > 0n) {
        chars.push(ALPHABET[Number(mag & mask)]);
        mag >>= bits;
    }
    chars.reverse();
    return (neg ? "-" : "") + chars.join("");
}

var tests = [
    {
        name: "Zero, negative, and single-digit values across all power-of-2 radixes",
        body() {
            var radixes = [2, 4, 8, 16, 32];
            for (var ri = 0; ri < radixes.length; ri++) {
                var r = radixes[ri];
                assert.strictEqual("0", (0n).toString(r));
                assert.strictEqual("0", (-0n).toString(r));
                for (var d = 1; d < r; d++) {
                    var expected = ALPHABET[d];
                    assert.strictEqual(expected, BigInt(d).toString(r), "pos d=" + d + " r=" + r);
                    assert.strictEqual("-" + expected, (-BigInt(d)).toString(r), "neg d=" + d + " r=" + r);
                }
                assert.strictEqual("10", BigInt(r).toString(r));
                assert.strictEqual("-10", (-BigInt(r)).toString(r));
            }
        }
    },
    {
        name: "32-bit and 64-bit limb boundaries across radixes 2, 4, 8, 16, 32",
        body() {
            var bitLengths = [
                1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17,
                30, 31, 32, 33, 34, 60, 61, 62, 63, 64, 65, 66,
                95, 96, 97, 127, 128, 129, 191, 192, 193,
                254, 255, 256, 257, 258, 319, 320, 321, 511, 512, 513
            ];
            var radixes = [2, 4, 8, 16, 32];

            for (var bi = 0; bi < bitLengths.length; bi++) {
                var b = BigInt(bitLengths[bi]);
                var candidates = [
                    1n << (b - 1n),
                    (1n << b) - 1n,
                    ((1n << b) - 1n) ^ (1n << (b >> 1n))
                ];
                for (var ci = 0; ci < candidates.length; ci++) {
                    var v = candidates[ci];
                    for (var ri = 0; ri < radixes.length; ri++) {
                        var r = radixes[ri];
                        var posExpected = referenceToPowerOf2String(v, r);
                        assert.strictEqual(posExpected, v.toString(r), "bits=" + bitLengths[bi] + " r=" + r);
                        if (v !== 0n) {
                            assert.strictEqual("-" + posExpected, (-v).toString(r), "neg bits=" + bitLengths[bi] + " r=" + r);
                        }
                    }
                    // Verify BigInt("0x...") / BigInt("0o...") / BigInt("0b...") round-trip.
                    assert.strictEqual(v, BigInt("0x" + v.toString(16)));
                    assert.strictEqual(v, BigInt("0o" + v.toString(8)));
                    assert.strictEqual(v, BigInt("0b" + v.toString(2)));
                }
            }
        }
    },
    {
        name: "CreateFromString leading zeros, case, whitespace, literals, and invalid digits",
        body() {
            assert.strictEqual(0n, BigInt("0x0"));
            assert.strictEqual(0n, BigInt("0x0000000000000000"));
            assert.strictEqual(0n, BigInt("0o0000000000000000"));
            assert.strictEqual(0n, BigInt("0b0000000000000000"));
            assert.strictEqual(1n, BigInt("0x00001"));
            assert.strictEqual(1n, BigInt("0o00001"));
            assert.strictEqual(1n, BigInt("0b00001"));
            assert.strictEqual(1n, BigInt("  0x" + "0".repeat(200) + "1  "));
            assert.strictEqual(0xabcdef1234567890abcdef1234567890n,
                BigInt("0X0000ABCDEF1234567890abcdef1234567890"));
            assert.strictEqual(0n, eval("0x0000_0000n"));
            assert.strictEqual(0x1234abcd5678ef01n, eval("0x000_1234_abcd_5678_ef01n"));

            var badInputs = [
                "0x", "0o", "0b", "-0x1", "+0x1", "-0o1", "+0b1",
                "0x0000g", "0xg0000", "0x" + "f".repeat(1000) + "z",
                "0b00002", "0b" + "1".repeat(500) + "2",
                "0o00008", "0o" + "7".repeat(500) + "9",
                "0x1_2"
            ];
            for (var i = 0; i < badInputs.length; i++) {
                var s = badInputs[i];
                assert.throws(function () { BigInt(s); }, SyntaxError, "bad " + s);
                assert.isFalse(0n == s, "eq false " + s);
            }
        }
    },
    {
        name: "Large 65k/262k power-of-2 round-trips are exact and fast",
        body() {
            var hex65k = "123456789abcdef0".repeat(4096); // 65,536 hex digits = 262,144 bits
            var big65k = BigInt("0x" + hex65k);
            assert.strictEqual(hex65k, big65k.toString(16));
            assert.strictEqual("-" + hex65k, (-big65k).toString(16));

            // Odd bit-boundary large value (not divisible by 3, 4, 5, 32, or 64)
            var largeOdd = (1n << 262141n) + (big65k >> 3n);
            var bin = largeOdd.toString(2);
            var oct = largeOdd.toString(8);
            var hex = largeOdd.toString(16);
            var b32 = largeOdd.toString(32);
            assert.strictEqual(262142, bin.length);
            assert.strictEqual(Math.ceil(262142 / 3), oct.length);
            assert.strictEqual(Math.ceil(262142 / 4), hex.length);
            assert.strictEqual(Math.ceil(262142 / 5), b32.length);
            assert.strictEqual(largeOdd, BigInt("0b" + bin));
            assert.strictEqual(largeOdd, BigInt("0o" + oct));
            assert.strictEqual(largeOdd, BigInt("0x" + hex));

            var hex262k = "f".repeat(262144);
            var big262k = BigInt("0x" + hex262k);
            assert.strictEqual((1n << 1048576n) - 1n, big262k);
            assert.strictEqual(hex262k, big262k.toString(16));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
