//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2025 DataView.prototype.getFloat16/setFloat16 and Math.f16round
// (doc/c2-s8-float16.md). Conversion is implemented with integer bit
// manipulation (round-to-nearest, ties-to-even, subnormals, overflow to
// Infinity, underflow to zero/subnormal). The Float16Array constructor is
// deliberately out of scope.
// Every value assertion below was mirrored against Node v26.5.0 first.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function isNegativeZero(value) {
    return value === 0 && 1 / value === -Infinity;
}

function assertSameValue(expected, actual, message) {
    if (expected !== expected) {
        assert.isTrue(actual !== actual, message || "expected NaN, got " + actual);
    } else if (expected === 0) {
        assert.isTrue(actual === 0 && isNegativeZero(actual) === isNegativeZero(expected),
            (message || "zero") + ": expected " + (isNegativeZero(expected) ? "-0" : "+0"));
    } else {
        assert.strictEqual(expected, actual, message);
    }
}

// Independent arithmetic decode of a binary16 pattern.
function decodeHalf(bits) {
    var sign = (bits & 0x8000) ? -1 : 1;
    var exponent = (bits >> 10) & 0x1F;
    var fraction = bits & 0x3FF;
    if (exponent === 0) {
        return sign * fraction * Math.pow(2, -24);
    }
    if (exponent === 0x1F) {
        return fraction === 0 ? sign * Infinity : NaN;
    }
    return sign * (1 + fraction / 1024) * Math.pow(2, exponent - 15);
}

function makeHalfReader(bits, littleEndian) {
    var view = new DataView(new ArrayBuffer(2));
    view.setUint16(0, bits, littleEndian);
    return view;
}

function expectHalfValue(bits, littleEndian, message) {
    var view = makeHalfReader(bits, littleEndian);
    assertSameValue(decodeHalf(bits), view.getFloat16(0, littleEndian),
        (message || "pattern 0x" + bits.toString(16)) + (littleEndian ? " LE" : " BE"));
}

function expectEncoded(value, expectedBits, message) {
    var view = new DataView(new ArrayBuffer(2));
    view.setFloat16(0, value, false);
    if (value !== value) {
        assert.isTrue(view.getFloat16(0) !== view.getFloat16(0), (message || "NaN") + ": NaN result");
        return;
    }
    assert.strictEqual(expectedBits, view.getUint16(0, false),
        (message || String(value)) + ": expected 0x" + expectedBits.toString(16));
}

var tests = [
    {
        name: "DataView getFloat16 decodes every binary16 pattern in both endiannesses",
        body() {
            for (var bits = 0; bits < 0x10000; bits++) {
                expectHalfValue(bits, false);
                expectHalfValue(bits, true);
            }
        }
    },
    {
        name: "DataView setFloat16 round-trips every non-NaN binary16 pattern",
        body() {
            var view = new DataView(new ArrayBuffer(2));
            for (var bits = 0; bits < 0x10000; bits++) {
                var value = decodeHalf(bits);
                view.setFloat16(0, value, false);
                if (value !== value) {
                    assert.isTrue(view.getFloat16(0) !== view.getFloat16(0),
                        "NaN pattern 0x" + bits.toString(16));
                } else {
                    assert.strictEqual(bits, view.getUint16(0, false),
                        "round-trip 0x" + bits.toString(16) + " (value " + value + ")");
                }
            }
        }
    },
    {
        name: "DataView getFloat16 special values and endianness",
        body() {
            var view = new DataView(new ArrayBuffer(8));
            view.setUint8(0, 0x00); view.setUint8(1, 0x00); // +0
            view.setUint8(2, 0x80); view.setUint8(3, 0x00); // -0
            view.setUint8(4, 0x7C); view.setUint8(5, 0x00); // +Infinity
            view.setUint8(6, 0xFC); view.setUint8(7, 0x00); // -Infinity
            assertSameValue(0, view.getFloat16(0));
            assert.isTrue(isNegativeZero(view.getFloat16(2)), "-0");
            assert.strictEqual(Infinity, view.getFloat16(4));
            assert.strictEqual(-Infinity, view.getFloat16(6));

            view.setUint8(0, 0x7E); view.setUint8(1, 0x00); // NaN
            assert.isTrue(view.getFloat16(0) !== view.getFloat16(0), "NaN");

            // 0x4B4B = 14.5859375; little-endian 0x4B4C = 17.171875.
            view.setUint8(0, 0x4B); view.setUint8(1, 0x4C);
            assert.strictEqual(14.59375, view.getFloat16(0, false));
            assert.strictEqual(17.171875, view.getFloat16(0, true));

            // Custom offset.
            var custom = new DataView(view.buffer, 4);
            assert.strictEqual(Infinity, custom.getFloat16(0));
        }
    },
    {
        name: "DataView setFloat16 little-endian byte order and return value",
        body() {
            var view = new DataView(new ArrayBuffer(2));
            assert.isUndefined(view.setFloat16(0, 42, true));
            assert.strictEqual(42, view.getFloat16(0, true));
            assert.strictEqual(0x40, view.getUint8(0));
            assert.strictEqual(0x51, view.getUint8(1));
            assert.strictEqual(0x5140, view.getUint16(0, true));

            assert.isUndefined(view.setFloat16(0, 2.158203125, true));
            assert.strictEqual(42, view.getFloat16(0));

            assert.isUndefined(view.setFloat16(0, -0, false));
            assert.isTrue(isNegativeZero(view.getFloat16(0)), "set -0");
        }
    },
    {
        name: "DataView setFloat16 rounds ties to even and handles overflow/underflow",
        body() {
            // 2^11 = 2048 has a binary16 spacing of 2: ties select the even neighbour.
            expectEncoded(2049, 0x6800, "2049 ties to even (down)");
            expectEncoded(2050, 0x6801, "2050 exact");
            expectEncoded(2051, 0x6802, "2051 ties to even (up)");
            expectEncoded(2052, 0x6802, "2052 exact");

            // Maximum finite, the 65520 overflow tie, and just below it.
            expectEncoded(65504, 0x7BFF, "max finite");
            expectEncoded(65519.99999999999, 0x7BFF, "largest double below overflow tie");
            expectEncoded(65520, 0x7C00, "overflow tie goes to Infinity");
            expectEncoded(65536, 0x7C00, "overflow");
            expectEncoded(-65504, 0xFBFF, "negative max finite");
            expectEncoded(-65520, 0xFC00, "negative overflow");

            // Subnormal/normal boundaries and the underflow tie.
            expectEncoded(5.960464477539063e-8, 0x0001, "smallest subnormal");
            expectEncoded(2.9802322387695312e-8, 0x0000, "underflow tie rounds to +0");
            expectEncoded(2.980232238769532e-8, 0x0001, "just above underflow tie");
            expectEncoded(-2.9802322387695312e-8, 0x8000, "negative underflow tie");
            expectEncoded(0.00006097555160522461, 0x03FF, "largest subnormal");
            expectEncoded(0.00006103515625, 0x0400, "smallest normal");
            expectEncoded(0.0000610053539276123, 0x03FF, "below normal boundary");
            expectEncoded(0.000061005353927612305, 0x0400, "normal boundary tie rounds to even");
            expectEncoded(8.940696716308594e-8, 0x0002, "subnormal tie to even");
            expectEncoded(1.4901161193847656e-7, 0x0002, "subnormal below tie");
            expectEncoded(1.490116119384766e-7, 0x0003, "subnormal above tie");

            // NaN is stored as a quiet NaN.
            expectEncoded(NaN, 0x7E00);
        }
    },
    {
        name: "DataView getFloat16/setFloat16 argument coercion and error order",
        body() {
            var view = new DataView(new ArrayBuffer(8));

            // Brand and receiver checks come first.
            assert.throws(function () { DataView.prototype.getFloat16.call({}); }, TypeError);
            assert.throws(function () { DataView.prototype.getFloat16.call(undefined); }, TypeError);
            assert.throws(function () { DataView.prototype.setFloat16.call({}); }, TypeError);
            assert.throws(function () { DataView.prototype.setFloat16.call(1); }, TypeError);

            // ToIndex precedes the detached and range checks.
            assert.throws(function () { view.getFloat16(-1); }, RangeError);
            assert.throws(function () { view.getFloat16(Infinity); }, RangeError);
            assert.throws(function () { view.getFloat16(7); }, RangeError);
            assert.throws(function () { view.setFloat16(-1, 0); }, RangeError);
            assert.throws(function () { view.setFloat16(Infinity, 0); }, RangeError);

            // Index coercion precedes value conversion.
            var valueConverted = false;
            var poisoned = { valueOf: function () { valueConverted = true; throw new Error("poisoned"); } };
            assert.throws(function () { view.setFloat16(-1, poisoned); }, RangeError);
            assert.isFalse(valueConverted, "value must not be converted after a bad index");

            // Value conversion precedes the range check.
            var marker = new Error("marker");
            var throwingValue = { valueOf: function () { throw marker; } };
            try {
                view.setFloat16(100, throwingValue);
                assert.fail("expected the value conversion to throw");
            } catch (e) {
                assert.strictEqual(marker, e, "value conversion error wins over RangeError");
            }

            // Abrupt ToNumber on the value and on the offset.
            assert.throws(function () { view.setFloat16(0, Symbol("1")); }, TypeError);
            assert.throws(function () { view.setFloat16(Symbol("1"), 1); }, TypeError);
            assert.throws(function () { view.getFloat16(Symbol("1")); }, TypeError);

            // Missing offset means undefined -> 0; missing value means NaN.
            view.setFloat16(0, 7);
            assert.isUndefined(view.setFloat16());
            assert.isTrue(view.getFloat16(0) !== view.getFloat16(0), "missing value writes NaN");
            view.setFloat16(0, 7);
            assert.isUndefined(view.setFloat16(0));
            assert.isTrue(view.getFloat16(0) !== view.getFloat16(0), "undefined value casts to NaN");
            view.setUint8(0, 0x4B); view.setUint8(1, 0x4C);
            assert.strictEqual(14.59375, view.getFloat16());
        }
    },
    {
        name: "DataView Float16 detached-buffer ordering",
        body() {
            var buffer = new ArrayBuffer(6);
            var view = new DataView(buffer);
            ArrayBuffer.detach(buffer);

            assert.throws(function () { view.getFloat16(-1); }, RangeError, "ToIndex before detach");
            assert.throws(function () { view.getFloat16(Infinity); }, RangeError, "ToIndex before detach");
            assert.throws(function () { view.getFloat16(5); }, TypeError, "detach before range");
            assert.throws(function () { view.getFloat16(0); }, TypeError);
            assert.throws(function () { view.setFloat16(-1, 0); }, RangeError, "ToIndex before detach");
            assert.throws(function () { view.setFloat16(5, 0); }, TypeError, "detach before range");

            var marker = new Error("marker");
            var throwingValue = { valueOf: function () { throw marker; } };
            try {
                view.setFloat16(0, throwingValue);
                assert.fail("expected the value conversion to throw");
            } catch (e) {
                assert.strictEqual(marker, e, "ToNumber(value) precedes the detach check");
            }
        }
    },
    {
        name: "DataView getFloat16/setFloat16 descriptors, lengths, and names",
        body() {
            var getter = Object.getOwnPropertyDescriptor(DataView.prototype, "getFloat16");
            var setter = Object.getOwnPropertyDescriptor(DataView.prototype, "setFloat16");
            assert.isTrue(getter.writable && getter.configurable && !getter.enumerable);
            assert.isTrue(setter.writable && setter.configurable && !setter.enumerable);
            assert.strictEqual("getFloat16", DataView.prototype.getFloat16.name);
            assert.strictEqual("setFloat16", DataView.prototype.setFloat16.name);
            assert.strictEqual(1, DataView.prototype.getFloat16.length);
            assert.strictEqual(2, DataView.prototype.setFloat16.length);
            assert.throws(function () { new DataView.prototype.getFloat16(); }, TypeError);
            assert.throws(function () { new DataView.prototype.setFloat16(); }, TypeError);
        }
    },
    {
        name: "Math.f16round converts, preserves special values, and matches DataView",
        body() {
            assert.strictEqual(1.099609375, Math.f16round(1.1));
            assert.strictEqual(0.0999755859375, Math.f16round(0.1));
            assert.strictEqual(0.60009765625, Math.f16round(0.6));
            assert.strictEqual(65504, Math.f16round(65504));
            assert.strictEqual(Infinity, Math.f16round(65520));
            assert.strictEqual(-Infinity, Math.f16round(-65536));
            assert.strictEqual(5.960464477539063e-8, Math.f16round(5.960464477539063e-8));
            assert.isTrue(Math.f16round(NaN) !== Math.f16round(NaN), "NaN");
            assert.strictEqual(Infinity, Math.f16round(Infinity));
            assert.strictEqual(-Infinity, Math.f16round(-Infinity));
            assert.isTrue(isNegativeZero(Math.f16round(-0)), "-0");
            assert.isTrue(isNegativeZero(Math.f16round(-1e-30)), "underflow keeps the sign");
            assert.strictEqual(0, Math.f16round(1e-30));
            assert.isTrue(Math.f16round() !== Math.f16round(), "missing argument is NaN");
            assert.throws(function () { Math.f16round(Symbol("1")); }, TypeError);

            // The result is a Number (the represented value), not a 16-bit integer.
            assert.strictEqual("number", typeof Math.f16round(1.1));

            // DataView and Math.f16round agree for every non-NaN pattern.
            var view = new DataView(new ArrayBuffer(2));
            for (var bits = 0; bits < 0x10000; bits++) {
                var value = decodeHalf(bits);
                if (value === value) {
                    assertSameValue(Math.f16round(value), decodeHalf(bits), "f16round 0x" + bits.toString(16));
                }
            }
        }
    },
    {
        name: "Math.f16round descriptor and non-constructibility",
        body() {
            var descriptor = Object.getOwnPropertyDescriptor(Math, "f16round");
            assert.isTrue(descriptor.writable && descriptor.configurable && !descriptor.enumerable);
            assert.strictEqual("f16round", Math.f16round.name);
            assert.strictEqual(1, Math.f16round.length);
            assert.throws(function () { new Math.f16round(); }, TypeError);
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
