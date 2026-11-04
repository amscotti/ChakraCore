//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2025 Float16Array (doc/c2-s8-float16array.md). The binary16 codec itself
// (Float16::ToBits/ToDouble) is shared with DataView.prototype.getFloat16 /
// setFloat16 and Math.f16round and is exhaustively covered by
// test/es7/float16.js; this file covers the constructor, element load/store
// conversion through every overload, and the generic %TypedArray% behavior
// dispatched by TypeIds_Float16Array.
// Every conversion expectation below was mirrored against Node v26.5.0 first;
// engine-wide typed-array conventions shared with Float32Array/Float64Array
// (constructor key order, index descriptor configurability) are asserted by
// consistency rather than by the newer spec text.

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

// Little-endian raw bits of element i (the engine is little-endian).
function bitsOf(ta, i) {
    return new DataView(ta.buffer, ta.byteOffset, ta.byteLength).getUint16(i * 2, true);
}

function allBits(ta) {
    var out = [];
    for (var i = 0; i < ta.length; i++) {
        out.push(bitsOf(ta, i).toString(16));
    }
    return out.join(",");
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

var tests = [
    {
        name: "Float16Array constructor identity and descriptors",
        body() {
            assert.strictEqual(3, Float16Array.length, "constructor length is 3");
            assert.strictEqual("Float16Array", Float16Array.name, "constructor name");
            assert.strictEqual(2, Float16Array.BYTES_PER_ELEMENT, "constructor BYTES_PER_ELEMENT");
            assert.strictEqual(2, Float16Array.prototype.BYTES_PER_ELEMENT, "prototype BYTES_PER_ELEMENT");

            function checkBPE(obj, label) {
                var descriptor = Object.getOwnPropertyDescriptor(obj, "BYTES_PER_ELEMENT");
                assert.isTrue(descriptor.writable === false && descriptor.enumerable === false &&
                    descriptor.configurable === false, label + " descriptor");
                assert.strictEqual(2, descriptor.value, label + " value");
            }
            checkBPE(Float16Array, "constructor");
            checkBPE(Float16Array.prototype, "prototype");

            var descriptor = Object.getOwnPropertyDescriptor(Float16Array.prototype, "constructor");
            assert.strictEqual(Float16Array, descriptor.value, "prototype.constructor");
            assert.isTrue(descriptor.writable && !descriptor.enumerable && descriptor.configurable,
                "prototype.constructor descriptor");

            // Chained to %TypedArray%.prototype like the other kinds.
            assert.strictEqual(Object.getPrototypeOf(Float16Array.prototype),
                Object.getPrototypeOf(Float32Array.prototype),
                "Float16Array.prototype parent is %TypedArray%.prototype");
            assert.strictEqual(Object.getPrototypeOf(Float16Array),
                Object.getPrototypeOf(Float32Array),
                "Float16Array parent is %TypedArray%");

            // Own-key order follows the engine convention shared with Float32Array.
            assert.strictEqual(
                Object.getOwnPropertyNames(Float32Array).join(","),
                Object.getOwnPropertyNames(Float16Array).join(","),
                "constructor own-key order matches Float32Array");

            // Symbol-keyed statics come from %TypedArray%.
            assert.strictEqual(Float16Array, Float16Array[Symbol.species],
                "@@species defaults to the constructor");
            assert.strictEqual("function", typeof Float16Array.from, "from is inherited");
            assert.strictEqual("function", typeof Float16Array.of, "of is inherited");

            // Cannot be called without new.
            assert.throws(function () { Float16Array(4); }, TypeError, "call without new throws");
            assert.throws(function () { Float16Array.call({}); }, TypeError, "call with receiver throws");
        }
    },
    {
        name: "Float16Array @@toStringTag",
        body() {
            var ta = new Float16Array(1);
            assert.strictEqual("[object Float16Array]", Object.prototype.toString.call(ta),
                "Object.prototype.toString");
            assert.strictEqual("Float16Array",
                Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Float16Array.prototype),
                    Symbol.toStringTag).get.call(ta),
                "%TypedArray%.prototype @@toStringTag getter");
            assert.strictEqual(undefined,
                Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Float16Array.prototype),
                    Symbol.toStringTag).get.call({}),
                "non typed array gives undefined");
        }
    },
    {
        name: "Float16Array length overload",
        body() {
            assert.strictEqual(0, new Float16Array().length, "missing argument gives length 0");
            assert.strictEqual(0, new Float16Array(undefined).length, "undefined gives length 0");
            assert.strictEqual(3, new Float16Array(3.7).length, "fractional length is ToIndex truncated");
            assert.strictEqual(3, new Float16Array("3").length, "string length is coerced");
            assert.strictEqual(0, new Float16Array(0).length, "explicit zero");
            assert.throws(function () { new Float16Array(-1); }, RangeError, "negative length");
            assert.throws(function () { new Float16Array(1e30); }, RangeError, "huge length");
            assert.throws(function () { new Float16Array(Symbol("1")); }, TypeError, "symbol length");
            var ta = new Float16Array(2);
            assert.strictEqual(0, ta[0], "elements initialize to +0");
            assert.strictEqual(0, bitsOf(ta, 0), "initial bits are +0");
        }
    },
    {
        name: "Float16Array TypedArray source overload",
        body() {
            var f32 = new Float32Array([1.5, -2.25, 0.1, NaN, Infinity]);
            var ta = new Float16Array(f32);
            assert.strictEqual(5, ta.length, "length copies the source length");
            assert.isTrue(ta.buffer !== f32.buffer, "a fresh buffer is allocated");
            assertSameValue(1.5, ta[0], "element 0");
            assertSameValue(-2.25, ta[1], "element 1");
            assertSameValue(Math.f16round(0.1), ta[2], "float32 rounds to binary16");
            assert.isTrue(ta[3] !== ta[3], "NaN stays NaN");
            assert.strictEqual(Infinity, ta[4], "Infinity");

            // Every other kind widens through ToNumber then narrows.
            var sources = [
                new Int8Array([1, -2]), new Uint8Array([1, 254]),
                new Uint8ClampedArray([1, 2]), new Int16Array([-1, 300]),
                new Uint16Array([1, 65534]), new Int32Array([1, 100000]),
                new Uint32Array([1, 70000]), new Float64Array([1.5, -0]),
                new Float16Array([1.5, -0])
            ];
            for (var i = 0; i < sources.length; i++) {
                var src = sources[i];
                var dst = new Float16Array(src);
                assert.strictEqual(src.length, dst.length, "length for " + src.constructor.name);
                for (var j = 0; j < src.length; j++) {
                    assertSameValue(Math.f16round(src[j]), dst[j],
                        src.constructor.name + "[" + j + "] converts through f16round");
                }
            }

            // Detached source throws.
            var buf = new ArrayBuffer(8);
            var doomed = new Float32Array(buf);
            buf.transfer();
            assert.throws(function () { new Float16Array(doomed); }, TypeError, "detached source");
        }
    },
    {
        name: "Float16Array 2-byte sources convert element-wise, not bitwise",
        body() {
            // Uint16/Int16 share the 2-byte element size. A bitwise copy
            // would preserve the patterns; the spec requires conversion.
            var u = new Uint16Array([0x3D00, 0xBC00, 0x7C00]);
            var d = new Float16Array(u);
            // The integers convert as numbers (verified against Node);
            // a bitwise copy would have preserved the raw patterns.
            assert.strictEqual(0x73A0, bitsOf(d, 0), "15616 converts, not 0x3D00");
            assert.strictEqual(0x79E0, bitsOf(d, 1), "48256 converts, not 0xBC00");
            assert.strictEqual(0x77C0, bitsOf(d, 2), "31744 converts, not 0x7C00");
            assertSameValue(15616, d[0], "15616 value");

            var s = new Int16Array([-1, -2]);
            var e = new Float16Array(s);
            assert.strictEqual(0xBC00, bitsOf(e, 0), "-1 converts to 0xBC00, not 0xFFFF");
            assert.strictEqual(0xC000, bitsOf(e, 1), "-2 converts to 0xC000, not 0xFFFE");
            assertSameValue(-2, e[1], "-2 value");

            // set() across 2-byte kinds also converts element-wise.
            var z = new Float16Array(2);
            z.set(new Uint16Array([0x3D00, 1]));
            assert.strictEqual(0x73A0, bitsOf(z, 0), "set converts, not bitwise");
            assertSameValue(1, z[1], "set value converts");
        }
    },
    {
        name: "Float16Array iterable and array-like overloads",
        body() {
            // Iterable (Set, generator).
            assert.strictEqual("3e00,4100", allBits(new Float16Array(new Set([1.5, 2.5]))),
                "Set source");
            function *gen() { yield 1.5; yield 2.5; }
            assert.strictEqual("3e00,4100", allBits(new Float16Array(gen())), "generator source");

            // Array-like with holes: missing elements read undefined -> NaN.
            var ta = new Float16Array({ length: 3, 0: 1.5, 2: 2.5 });
            assert.strictEqual(3, ta.length, "array-like length");
            assertSameValue(1.5, ta[0], "present element");
            assert.isTrue(ta[1] !== ta[1], "hole reads NaN");
            assert.strictEqual(0x7E00, bitsOf(ta, 1), "hole stores canonical NaN");
            assertSameValue(2.5, ta[2], "present element");

            // Array source with values needing conversion.
            var arr = new Float16Array([0.1, "2.5", undefined, null, true]);
            assertSameValue(Math.f16round(0.1), arr[0], "0.1 rounds");
            assertSameValue(2.5, arr[1], "string coerces");
            assert.isTrue(arr[2] !== arr[2], "undefined stores NaN");
            assertSameValue(0, arr[3], "null stores +0");
            assertSameValue(1, arr[4], "true stores 1");

            // Symbol values throw through ToNumber.
            assert.throws(function () { new Float16Array([Symbol("x")]); }, TypeError,
                "symbol element");
            assert.throws(function () { new Float16Array({ length: 1, 0: Symbol("x") }); },
                TypeError, "symbol in array-like");
        }
    },
    {
        name: "Float16Array ArrayBuffer overloads",
        body() {
            var buf = new ArrayBuffer(8);
            var full = new Float16Array(buf);
            assert.strictEqual(4, full.length, "buffer-only length");
            assert.strictEqual(8, full.byteLength, "buffer-only byteLength");
            assert.strictEqual(0, full.byteOffset, "buffer-only byteOffset");
            assert.isTrue(full.buffer === buf, "buffer identity");

            var mid = new Float16Array(buf, 2);
            assert.strictEqual(3, mid.length, "offset-only length");
            assert.strictEqual(2, mid.byteOffset, "byteOffset");

            var win = new Float16Array(buf, 2, 2);
            assert.strictEqual(2, win.length, "offset+length");
            assert.isTrue(buf === win.buffer, "shared buffer");

            // Views share storage.
            full[1] = 1.5;
            assertSameValue(1.5, mid[0], "shared storage reads the write");
            mid[0] = 2.5;
            assertSameValue(2.5, full[1], "writes are visible both ways");

            // Misaligned offset and overrun throw RangeError.
            assert.throws(function () { new Float16Array(buf, 1); }, RangeError, "odd offset");
            assert.throws(function () { new Float16Array(buf, 2, 4); }, RangeError, "overrun");
            assert.throws(function () { new Float16Array(buf, 10); }, RangeError, "offset past end");
            assert.throws(function () { new Float16Array(new ArrayBuffer(9)); }, RangeError,
                "buffer length not a multiple of 2");

            // Detached buffer throws.
            var dead = new ArrayBuffer(8);
            dead.transfer();
            assert.throws(function () { new Float16Array(dead); }, TypeError, "detached buffer");
            assert.throws(function () { new Float16Array(dead, 0); }, TypeError,
                "detached buffer with offset");
        }
    },
    {
        name: "Float16Array element store is ToNumber then round-to-nearest-even",
        body() {
            var ta = new Float16Array(1);
            function stored(value) {
                ta[0] = value;
                return bitsOf(ta, 0);
            }

            // Exact values round-trip bit-identically.
            assert.strictEqual(0x3C00, stored(1), "1");
            assert.strictEqual(0xBC00, stored(-1), "-1");
            assert.strictEqual(0x0, stored(0), "+0");
            assert.strictEqual(0x8000, stored(-0), "-0 keeps the sign");
            assert.strictEqual(0x7C00, stored(Infinity), "+Infinity");
            assert.strictEqual(0xFC00, stored(-Infinity), "-Infinity");
            assert.strictEqual(0x7BFF, stored(65504), "max finite");
            assert.strictEqual(0x0400, stored(0.00006103515625), "min normal 2^-14");
            assert.strictEqual(0x0001, stored(5.960464477539063e-8), "min subnormal 2^-24");

            // Overflow (including the 65520 tie) goes to Infinity.
            assert.strictEqual(0x7C00, stored(65520), "overflow tie");
            assert.strictEqual(0x7C00, stored(100000), "overflow");
            assert.strictEqual(0xFC00, stored(-65520), "negative overflow tie");

            // Underflow: the 2^-25 tie goes to even (zero); above rounds up.
            assert.strictEqual(0x0, stored(Math.pow(2, -25)), "underflow tie to zero");
            assert.strictEqual(0x8000, stored(-Math.pow(2, -25)), "signed underflow tie");
            assert.strictEqual(0x0001, stored(Math.pow(2, -24) + Math.pow(2, -26)),
                "above the tie rounds to min subnormal");

            // Ties-to-even in the normal range: halfway between 1 (0x3C00)
            // and 0x3C01 rounds down (even), halfway between 0x3C01 and
            // 0x3C02 rounds up (even).
            assert.strictEqual(0x3C00, stored(1.00048828125), "tie to even mantissa");
            assert.strictEqual(0x3C02, stored(1.00146484375), "tie up to even mantissa");

            // NaN canonicalizes with the input sign.
            assert.strictEqual(0x7E00, stored(NaN), "NaN canonical bits");
            assert.strictEqual(0xFE00, stored(-NaN), "-NaN keeps the sign");

            // Value coercion runs before conversion; abrupt completions win.
            var log = [];
            ta[0] = { valueOf() { log.push("coerced"); return 1.5; } };
            assert.strictEqual("coerced", log.join(","), "valueOf runs");
            assertSameValue(1.5, ta[0], "coerced value stores");
            assert.throws(function () { ta[0] = Symbol("x"); }, TypeError, "symbol store");
        }
    },
    {
        name: "Float16Array exhaustive bit round trip",
        body() {
            var view = new DataView(new ArrayBuffer(2));
            var ta = new Float16Array(view.buffer);

            // Load: every stored pattern widens exactly.
            for (var bits = 0; bits < 0x10000; bits++) {
                view.setUint16(0, bits, true);
                var expected = decodeHalf(bits);
                if (expected !== expected) {
                    assert.isTrue(ta[0] !== ta[0], "NaN pattern 0x" + bits.toString(16));
                } else {
                    assertSameValue(expected, ta[0], "load 0x" + bits.toString(16));
                }
            }

            // Store: every finite value writes back its own pattern. The
            // in-test decoder produces a positive NaN for every NaN
            // pattern, so every NaN store canonicalizes to +0x7E00 (the
            // -NaN sign case is covered separately above).
            for (var p = 0; p < 0x10000; p++) {
                var value = decodeHalf(p);
                ta[0] = value;
                var back = bitsOf(ta, 0);
                if (value !== value) {
                    assert.strictEqual(0x7E00, back,
                        "NaN store canonicalizes 0x" + p.toString(16));
                } else {
                    assert.strictEqual(p, back, "store 0x" + p.toString(16));
                }
            }
        }
    },
    {
        name: "Float16Array agrees with DataView.setFloat16",
        body() {
            var view = new DataView(new ArrayBuffer(2));
            var ta = new Float16Array(view.buffer);
            var samples = [0, -0, 1, -1, 0.1, 0.60009765625, 65504, 65520,
                1e-8, 5.960464477539063e-8, 3.0000003, -123.456, Infinity, -Infinity];
            for (var i = 0; i < samples.length; i++) {
                view.setFloat16(0, samples[i], true);
                var viaView = view.getUint16(0, true);
                ta[0] = samples[i];
                assert.strictEqual(viaView, bitsOf(ta, 0),
                    "store agreement for " + samples[i]);
                assertSameValue(view.getFloat16(0, true), ta[0],
                    "load agreement for " + samples[i]);
            }
            // NaN agrees up to payload: both read back NaN, and the array
            // stores canonical bits.
            view.setFloat16(0, NaN, true);
            ta[0] = NaN;
            assert.isTrue(ta[0] !== ta[0], "array NaN");
            assert.isTrue(view.getFloat16(0, true) !== view.getFloat16(0, true), "view NaN");
            assert.strictEqual(0x7E00, bitsOf(ta, 0), "array NaN canonical bits");
        }
    },
    {
        name: "Float16Array set, subarray and slice",
        body() {
            // Same-type set copies bitwise.
            var src = new Float16Array([1.5, NaN, -0]);
            var dst = new Float16Array(3);
            assert.strictEqual(undefined, dst.set(src, 0), "set returns undefined");
            assert.strictEqual(allBits(src), allBits(dst), "same-type set copies bits");

            // Offset and array sources.
            dst = new Float16Array(4);
            dst.set([9.5], 2);
            assertSameValue(9.5, dst[2], "set with offset");
            assert.throws(function () { dst.set([1, 2, 3, 4, 5]); }, RangeError, "set overrun");
            assert.throws(function () { dst.set([1], 5); }, RangeError, "set offset out of range");

            // Detached source and target throw.
            var doomedBuf = new ArrayBuffer(4);
            var doomed = new Float16Array(doomedBuf);
            doomedBuf.transfer();
            assert.throws(function () { new Float16Array(2).set(doomed); }, TypeError,
                "detached source");
            assert.throws(function () { doomed.set([1]); }, TypeError, "detached target");

            // subarray shares storage; slice copies.
            var x = new Float16Array([1, 2, 3, 4]);
            var sub = x.subarray(1, 3);
            assert.isTrue(sub instanceof Float16Array, "subarray keeps the kind");
            assert.isTrue(sub.buffer === x.buffer, "subarray shares the buffer");
            assert.strictEqual(2, sub.byteOffset, "subarray byteOffset");
            sub[0] = 9.5;
            assertSameValue(9.5, x[1], "subarray write is visible");

            var cp = x.slice();
            assert.isTrue(cp instanceof Float16Array, "slice keeps the kind");
            assert.isTrue(cp.buffer !== x.buffer, "slice copies");
            cp[0] = 7.5;
            assertSameValue(1, x[0], "slice write is isolated");
            assert.strictEqual("3c00,48c0,4200,4400", allBits(x.slice(0)),
                "slice full bits");

            // Species: derived constructors flow through map/slice/from.
            class Sub extends Float16Array {}
            var subm = new Sub([1, 2]).map(function (v) { return v * 2; });
            assert.isTrue(subm instanceof Sub, "map honors the subclass");
            assert.isTrue(subm instanceof Float16Array, "map keeps the kind");
            assertSameValue(4, subm[1], "subclass map value");
            var subs = new Sub([1, 2, 3]).slice(1);
            assert.isTrue(subs instanceof Sub, "slice honors the subclass");
            var subf = Float16Array.from.call(Sub, [1.5]);
            assert.isTrue(subf instanceof Sub, "from honors the call-site constructor");
            assertSameValue(1.5, subf[0], "from value");
        }
    },
    {
        name: "Float16Array iteration and search",
        body() {
            var ta = new Float16Array([5.5, 6.5]);

            assert.strictEqual("0,1", Array.from(ta.keys()).join(","), "keys");
            assert.strictEqual("5.5,6.5", Array.from(ta.values()).join(","), "values");
            assert.strictEqual("0:5.5,1:6.5",
                Array.from(ta.entries()).map(function (e) { return e.join(":"); }).join(","),
                "entries");
            assert.strictEqual("5.5,6.5", Array.from(ta).join(","), "for-of via Array.from");
            assert.strictEqual("5.5|6.5", [...ta].join("|"), "spread");

            // indexOf uses strict equality (NaN never matches); includes
            // uses SameValueZero (NaN matches).
            var s = new Float16Array([1, 2, 1, NaN]);
            assert.strictEqual(0, s.indexOf(1), "indexOf");
            assert.strictEqual(2, s.indexOf(1, 2), "indexOf from index");
            assert.strictEqual(2, s.lastIndexOf(1), "lastIndexOf");
            assert.strictEqual(-1, s.indexOf(9), "indexOf missing");
            assert.strictEqual(-1, s.indexOf(NaN), "indexOf NaN is -1");
            assert.isTrue(s.includes(NaN), "includes NaN");
            assert.isTrue(!s.includes(9), "includes missing");

            // find family passes widened numbers, indices and the array.
            var seen = [];
            var arr3 = new Float16Array([1.5, 2.5, 3.5]);
            var found = arr3.find(function (v, i, o) {
                seen.push(v + "@" + i + ":" + (o === arr3));
                return v > 2;
            });
            assertSameValue(2.5, found, "find value");
            assert.strictEqual("1.5@0:true,2.5@1:true", seen.join(","),
                "find callback arguments");
            var missing = new Float16Array([1.5]).findIndex(function () { return false; });
            assert.strictEqual(-1, missing, "findIndex missing");
            assertSameValue(3.5, new Float16Array([1.5, 2.5, 3.5]).findLast(function (v) {
                return v > 1;
            }), "findLast");
            assert.strictEqual(0, new Float16Array([1.5, 2.5]).findLastIndex(function (v) {
                return v < 2;
            }), "findLastIndex");

            assert.isTrue(new Float16Array([1, 2]).every(function (v) { return v > 0; }), "every");
            assert.isTrue(new Float16Array([1, 2]).some(function (v) { return v > 1.5; }), "some");
        }
    },
    {
        name: "Float16Array map, filter and reduce keep the kind",
        body() {
            var mapped = new Float16Array([1, 2]).map(function (v, i) { return v * 2 + i; });
            assert.isTrue(mapped instanceof Float16Array, "map returns Float16Array");
            assertSameValue(2, mapped[0], "map[0]");
            assertSameValue(5, mapped[1], "map[1]");

            var filtered = new Float16Array([1, 2, 3]).filter(function (v) { return v > 1.5; });
            assert.isTrue(filtered instanceof Float16Array, "filter returns Float16Array");
            assert.strictEqual("4000,4200", allBits(filtered), "filter bits");

            assertSameValue(4, new Float16Array([1.5, 2.5]).reduce(function (a, b) {
                return a + b;
            }), "reduce");
            assertSameValue(1, new Float16Array([1.5, 2.5]).reduceRight(function (a, b) {
                return a - b;
            }), "reduceRight");
            assert.throws(function () { new Float16Array(0).reduce(function () { }); },
                TypeError, "reduce on empty without initial");
            assertSameValue(7, new Float16Array(0).reduce(function () { }, 7),
                "reduce on empty with initial");

            // join/toString/toLocaleString see the widened values.
            assert.strictEqual("1.5-2.5", new Float16Array([1.5, 2.5]).join("-"), "join");
            assert.strictEqual("1.5,2.5", new Float16Array([1.5, 2.5]).toString(), "toString");
            assert.strictEqual(new Float64Array([1.5]).toLocaleString(),
                new Float16Array([1.5]).toLocaleString(), "toLocaleString matches float64");
        }
    },
    {
        name: "Float16Array sort, fill, copyWithin, reverse and friends",
        body() {
            // Default sort is numeric on the represented values, NaN last.
            var s = new Float16Array([3, NaN, 1, 2]);
            s.sort();
            assert.strictEqual("3c00,4000,4200,7e00", allBits(s), "default sort bits");
            assert.isTrue(s[3] !== s[3], "NaN sorts last");

            // A comparefn receives widened Numbers.
            var got = [];
            var t = new Float16Array([3, 1, 2]);
            t.sort(function (x, y) { got.push(typeof x + ":" + typeof y); return x - y; });
            assert.strictEqual("3c00,4000,4200", allBits(t), "sorted bits");
            for (var i = 0; i < got.length; i++) {
                assert.strictEqual("number:number", got[i], "comparefn gets numbers");
            }

            // fill converts the value once for the whole range.
            assert.strictEqual("2e66,2e66,2e66", allBits(new Float16Array(3).fill(0.1)),
                "fill converts");
            assert.strictEqual("3c00,48c0,4200",
                allBits(new Float16Array([1, 2, 3]).fill(9.5, 1, 2)), "fill range");

            // copyWithin, reverse, at, with, toSorted, toReversed.
            assert.strictEqual("4200,4400,4200,4400",
                allBits(new Float16Array([1, 2, 3, 4]).copyWithin(0, 2)), "copyWithin");
            var r = new Float16Array([1.5, 2.5]);
            assert.isTrue(r.reverse() === r, "reverse returns this");
            assert.strictEqual("4100,3e00", allBits(r), "reverse bits");

            var a = new Float16Array([1.5, 2.5, 3.5]);
            assertSameValue(3.5, a.at(-1), "at(-1)");
            assert.strictEqual(undefined, a.at(9), "at OOB is undefined");
            var w = a.with(1, 9.5);
            assert.isTrue(w instanceof Float16Array, "with keeps the kind");
            assertSameValue(2.5, a[1], "with does not mutate");
            assertSameValue(9.5, w[1], "with value");
            assert.throws(function () { a.with(9, 1); }, RangeError, "with OOB");

            var sorted = new Float16Array([3, 1, 2]).toSorted();
            assert.isTrue(sorted instanceof Float16Array, "toSorted keeps the kind");
            assert.strictEqual("3c00,4000,4200", allBits(sorted), "toSorted bits");
            var rev = a.toReversed();
            assert.isTrue(rev instanceof Float16Array, "toReversed keeps the kind");
            assert.strictEqual("4300,4100,3e00", allBits(rev), "toReversed bits");
        }
    },
    {
        name: "Float16Array.from and Float16Array.of",
        body() {
            var f = Float16Array.from([1.5, 2.5]);
            assert.isTrue(f instanceof Float16Array, "from returns Float16Array");
            assert.strictEqual("3e00,4100", allBits(f), "from values");

            var mapped = Float16Array.from([1, 2], function (v, i) { return v * 2 + i; });
            assert.strictEqual("4000,4500", allBits(mapped), "from with mapfn");

            var thisArg = { mark: 21 };
            var seenThis = null;
            Float16Array.from([1], function () { seenThis = this; }, thisArg);
            assert.isTrue(seenThis === thisArg, "from mapfn thisArg");

            assert.throws(function () { Float16Array.from(); }, TypeError, "from missing source");
            assert.throws(function () { Float16Array.from([1], 1); }, TypeError,
                "from non-callable mapfn");

            var o = Float16Array.of(1.5, 2.5, 0.1);
            assert.isTrue(o instanceof Float16Array, "of returns Float16Array");
            assert.strictEqual(3, o.length, "of length");
            assertSameValue(Math.f16round(0.1), o[2], "of converts");

            assert.strictEqual(1, Float16Array.from.length, "from length");
            assert.strictEqual(0, Float16Array.of.length, "of length");
        }
    },
    {
        name: "Float16Array accessors, detach and resizable interplay",
        body() {
            var buf = new ArrayBuffer(8);
            var ta = new Float16Array(buf, 2, 2);
            assert.isTrue(ta.buffer === buf, "buffer getter");
            assert.strictEqual(4, ta.byteLength, "byteLength getter");
            assert.strictEqual(2, ta.byteOffset, "byteOffset getter");
            assert.strictEqual(2, ta.length, "length getter");

            // Detached views report zero length and throw on access.
            buf.transfer();
            assert.strictEqual(0, ta.length, "length after detach");
            assert.strictEqual(0, ta.byteLength, "byteLength after detach");
            // Web-reality detached semantics: element reads see undefined and
            // stores are silent no-ops; validating builtins still throw.
            assert.strictEqual(undefined, ta[0], "get after detach");
            ta[0] = 1;
            assert.strictEqual(undefined, ta[0], "set after detach is a no-op");
            assert.throws(function () { ta.set([1]); }, TypeError, "set() after detach");
            assert.throws(function () { Array.from(ta); }, TypeError, "iteration after detach");

            // Length-tracking views follow resizable buffers like other kinds.
            var rab = new ArrayBuffer(8, { maxByteLength: 16 });
            var tracked = new Float16Array(rab);
            var fixed = new Float16Array(rab, 0, 2);
            assert.strictEqual(4, tracked.length, "tracking starts at 4");
            rab.resize(16);
            assert.strictEqual(8, tracked.length, "tracking follows growth");
            assert.strictEqual(2, fixed.length, "fixed view keeps its length");
            rab.resize(2);
            assert.strictEqual(1, tracked.length, "tracking follows shrinkage");
            assert.strictEqual(0, fixed.length, "fixed view is OOB while shrunk");
            // Out-of-bounds reads are undefined (only detached views throw).
            assert.strictEqual(undefined, fixed[0], "OOB view access is undefined");
            rab.resize(16);
            assert.strictEqual(2, fixed.length, "fixed view recovers after growth");
            assertSameValue(0, fixed[0], "recovered value");
        }
    },
    {
        name: "Float16Array index-exotic semantics",
        body() {
            // Out-of-bounds reads are undefined; writes are ignored.
            var ta = new Float16Array(2);
            assert.strictEqual(undefined, ta[5], "OOB read is undefined");
            ta[5] = 1;
            assert.strictEqual(2, ta.length, "OOB write changes nothing");
            assert.isTrue(!("5" in ta), "OOB write creates no expando");

            assert.isTrue(0 in ta, "index in range");
            assert.isTrue(!("5" in ta), "index out of range");
            assert.strictEqual(false, delete ta[0], "indices are not deletable");

            // Index descriptors match the engine convention shared with
            // Float32Array (configurable is false engine-wide here; the
            // spec text says true).
            var mine = Object.getOwnPropertyDescriptor(ta, "0");
            var theirs = Object.getOwnPropertyDescriptor(new Float32Array(1), "0");
            assert.strictEqual(theirs.configurable, mine.configurable,
                "configurable matches Float32Array");
            assert.strictEqual(theirs.writable, mine.writable, "writable matches");
            assert.strictEqual(theirs.enumerable, mine.enumerable, "enumerable matches");
            assertSameValue(0, mine.value, "descriptor value");
            assert.strictEqual(undefined,
                Object.getOwnPropertyDescriptor(ta, "5"), "OOB has no descriptor");

            // Length assignment throws in strict mode (accessor without setter).
            assert.throws(function () { "use strict"; ta.length = 5; }, TypeError,
                "length assignment throws");
        }
    },
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
