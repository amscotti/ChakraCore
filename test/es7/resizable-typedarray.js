//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2024/ES2025 TypedArray views over resizable ArrayBuffers
// (doc/c2-s8-typedarray-tracking.md). Auto-length views created without an
// explicit length track the current buffer byte length; explicit-length
// views keep their captured length and become length-zero while OOB.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var constructors = [
    Uint8Array,
    Int8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array
];

function elements(view) {
    var result = [];
    for (var i = 0; i < view.length; i++) {
        result.push(view[i]);
    }
    return result;
}

function assertSameElements(expected, actual, message) {
    assert.strictEqual(expected.length, actual.length, (message || "elements") + ": length");
    for (var i = 0; i < expected.length; i++) {
        if (expected[i] !== expected[i]) {
            assert.isTrue(actual[i] !== actual[i], (message || "elements") + "[" + i + "]: NaN");
        } else {
            assert.strictEqual(expected[i], actual[i], (message || "elements") + "[" + i + "]");
        }
    }
}

function assertThrowsTypeError(testFunction, message) {
    assert.throws(testFunction, TypeError, message);
}

var tests = [
    {
        name: "Auto-length views track grow, shrink, partial elements and recovery",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(16, { maxByteLength: 40 });
                var offset = 8;
                var view = new Ctor(rab, offset);

                assert.areEqual((16 - offset) / bpe, view.length, Ctor.name + " initial length");
                assert.areEqual(16 - offset, view.byteLength, Ctor.name + " initial byteLength");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " initial byteOffset");

                rab.resize(40);
                assert.areEqual((40 - offset) / bpe, view.length, Ctor.name + " grown length");
                assert.areEqual(40 - offset, view.byteLength, Ctor.name + " grown byteLength");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " grown byteOffset");

                rab.resize(20);
                assert.areEqual(Math.floor((20 - offset) / bpe), view.length, Ctor.name + " shrunk length");
                assert.areEqual(Math.floor((20 - offset) / bpe) * bpe, view.byteLength,
                    Ctor.name + " shrunk byteLength drops the partial element");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " shrunk byteOffset");

                // The offset itself is past the end: the view is out of bounds
                // and byteOffset reports zero until the offset fits again.
                rab.resize(7);
                assert.areEqual(0, view.length, Ctor.name + " offset-OOB length");
                assert.areEqual(0, view.byteLength, Ctor.name + " offset-OOB byteLength");
                assert.areEqual(0, view.byteOffset, Ctor.name + " offset-OOB byteOffset");

                rab.resize(offset);
                assert.areEqual(0, view.length, Ctor.name + " boundary length");
                assert.areEqual(0, view.byteLength, Ctor.name + " boundary byteLength");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " boundary byteOffset");

                rab.resize(offset + 1);
                assert.areEqual(bpe === 1 ? 1 : 0, view.length, Ctor.name + " partial first element");
                assert.areEqual(bpe === 1 ? 1 : 0, view.byteLength, Ctor.name + " partial first byteLength");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " partial first byteOffset");

                rab.resize(0);
                assert.areEqual(0, view.length, Ctor.name + " zero length");
                assert.areEqual(0, view.byteLength, Ctor.name + " zero byteLength");
                assert.areEqual(0, view.byteOffset, Ctor.name + " zero byteOffset");

                rab.resize(40);
                assert.areEqual((40 - offset) / bpe, view.length, Ctor.name + " recovered length");
                assert.areEqual(40 - offset, view.byteLength, Ctor.name + " recovered byteLength");
                assert.areEqual(offset, view.byteOffset, Ctor.name + " recovered byteOffset");
            });
        }
    },
    {
        name: "Explicit-length views keep their length, go OOB on shrink and recover on grow",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(8 * bpe, { maxByteLength: 16 * bpe });
                var view = new Ctor(rab, 2 * bpe, 3);

                assert.areEqual(3, view.length, Ctor.name + " fixed initial length");
                assert.areEqual(3 * bpe, view.byteLength, Ctor.name + " fixed initial byteLength");

                rab.resize(16 * bpe);
                assert.areEqual(3, view.length, Ctor.name + " fixed grow keeps length");
                assert.areEqual(3 * bpe, view.byteLength, Ctor.name + " fixed grow keeps byteLength");
                assert.areEqual(2 * bpe, view.byteOffset, Ctor.name + " fixed grow keeps byteOffset");

                rab.resize(5 * bpe);
                assert.areEqual(3, view.length, Ctor.name + " fixed in-bounds shrink keeps length");

                // Only two of the three elements still fit.
                rab.resize(4 * bpe);
                assert.areEqual(0, view.length, Ctor.name + " fixed OOB length is zero");
                assert.areEqual(0, view.byteLength, Ctor.name + " fixed OOB byteLength is zero");
                assert.areEqual(0, view.byteOffset, Ctor.name + " fixed OOB byteOffset is zero");

                rab.resize(5 * bpe);
                assert.areEqual(3, view.length, Ctor.name + " fixed recovers after grow");
                assert.areEqual(3 * bpe, view.byteLength, Ctor.name + " fixed byteLength recovers");
                assert.areEqual(2 * bpe, view.byteOffset, Ctor.name + " fixed byteOffset recovers");
            });
        }
    },
    {
        name: "Element get/set and property queries follow the current length",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(4 * bpe, { maxByteLength: 8 * bpe });
                var view = new Ctor(rab);

                for (var i = 0; i < 4; i++) {
                    view[i] = i + 1;
                }
                assertSameElements([1, 2, 3, 4], elements(view), Ctor.name + " initial values");

                rab.resize(2 * bpe);
                assertSameElements([1, 2], elements(view), Ctor.name + " shrunk values");
                assert.strictEqual(undefined, view[2], Ctor.name + " OOB get is undefined");
                assert.strictEqual(undefined, view[3], Ctor.name + " OOB get is undefined");
                assert.isFalse(2 in view, Ctor.name + " OOB index not present");
                view[2] = 42;
                view[3] = 43;
                assertSameElements([1, 2], elements(view), Ctor.name + " OOB set is a no-op");

                rab.resize(4 * bpe);
                assertSameElements([1, 2, 0, 0], elements(view), Ctor.name + " regrown bytes are zeroed");
                assert.isTrue(2 in view, Ctor.name + " recovered index present");

                var fixed = new Ctor(rab, 0, 4);
                rab.resize(2 * bpe);
                assert.strictEqual(0, fixed.length, Ctor.name + " fixed view OOB length");
                assert.strictEqual(undefined, fixed[0], Ctor.name + " fixed view OOB get");
                assert.isFalse(0 in fixed, Ctor.name + " fixed view OOB has");
                fixed[0] = 99;
                rab.resize(4 * bpe);
                assertSameElements([1, 2, 0, 0], elements(fixed), Ctor.name + " fixed view recovers original bytes");

                var descriptor = Object.getOwnPropertyDescriptor(view, "1");
                assert.isTrue(descriptor !== undefined, Ctor.name + " index descriptor exists");
                assert.strictEqual(2, descriptor.value, Ctor.name + " index descriptor value");
            });
        }
    },
    {
        name: "Explicit undefined length tracks; explicit zero stays fixed",
        body() {
            var partialRab = new ArrayBuffer(3, { maxByteLength: 3 });
            var partial = new Uint16Array(partialRab);
            assert.areEqual(1, partial.length, "auto view floors a partial element");
            assert.areEqual(2, partial.byteLength, "auto view byteLength floors a partial element");

            var rab = new ArrayBuffer(8, { maxByteLength: 16 });
            var undefinedLength = new Uint8Array(rab, 2, undefined);
            var zeroLength = new Uint8Array(rab, 2, 0);

            assert.areEqual(6, undefinedLength.length, "undefined length is auto");
            assert.areEqual(0, zeroLength.length, "explicit zero is fixed");

            rab.resize(16);
            assert.areEqual(14, undefinedLength.length, "undefined length tracks grow");
            assert.areEqual(0, zeroLength.length, "explicit zero stays fixed");

            rab.resize(4);
            assert.areEqual(2, undefinedLength.length, "undefined length tracks shrink");
            assert.areEqual(0, zeroLength.length, "explicit zero stays in bounds");

            rab.resize(2);
            assert.areEqual(0, undefinedLength.length, "undefined length at offset boundary");
            assert.areEqual(0, zeroLength.length, "explicit zero at offset boundary");
            assert.areEqual(2, undefinedLength.byteOffset, "auto boundary byteOffset");
            assert.areEqual(2, zeroLength.byteOffset, "zero-length offset stays visible");

            rab.resize(1);
            assert.areEqual(0, undefinedLength.byteOffset, "auto offset-OOB byteOffset");
            assert.areEqual(0, zeroLength.byteOffset, "zero-length offset-OOB byteOffset");
        }
    },
    {
        name: "Constructor re-reads the buffer length after offset coercion",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 12 });
            var offset = {
                valueOf() {
                    rab.resize(12);
                    return 8;
                }
            };
            var view = new Uint8Array(rab, offset);

            assert.areEqual(4, view.length, "view uses the grown byteLength");
            assert.areEqual(8, view.byteOffset, "view keeps the coerced offset");
        }
    },
    {
        name: "with validates the index against the length after ToNumber",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 4 });
            var view = new Uint8Array(rab);
            var value = {
                valueOf() {
                    rab.resize(1);
                    return 1;
                }
            };

            assert.throws(function () { view.with(-1, value); }, RangeError,
                "with index became out of bounds");
            assert.areEqual(1, view.length, "buffer stays shrunk");

            rab.resize(4);
            assert.areEqual(7, view.with(0, 7)[0], "with still copies in bounds");
        }
    },
    {
        name: "Prototype methods reject out-of-bounds fixed views with TypeError",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(4 * bpe, { maxByteLength: 8 * bpe });
                var view = new Ctor(rab, 0, 4);
                for (var i = 0; i < 4; i++) {
                    view[i] = i;
                }

                rab.resize(3 * bpe);
                assert.areEqual(0, view.length, Ctor.name + " fixed view OOB");
                assertThrowsTypeError(function () { view.at(0); }, Ctor.name + " at");
                assertThrowsTypeError(function () { view.fill(1); }, Ctor.name + " fill");
                assertThrowsTypeError(function () { view.copyWithin(0, 1); }, Ctor.name + " copyWithin");
                assertThrowsTypeError(function () { view.sort(); }, Ctor.name + " sort");
                assertThrowsTypeError(function () { view.slice(); }, Ctor.name + " slice");
                assertThrowsTypeError(function () { view.every(function () { return true; }); }, Ctor.name + " every");
                assertThrowsTypeError(function () { view.map(function (v) { return v; }); }, Ctor.name + " map");
                assertThrowsTypeError(function () { view.reduce(function (a, b) { return a + b; }); }, Ctor.name + " reduce");
                assertThrowsTypeError(function () { view.join(); }, Ctor.name + " join");
                assertThrowsTypeError(function () { view.includes(0); }, Ctor.name + " includes");
                assertThrowsTypeError(function () { view.indexOf(0); }, Ctor.name + " indexOf");
                assertThrowsTypeError(function () { view.lastIndexOf(0); }, Ctor.name + " lastIndexOf");
                assertThrowsTypeError(function () { view.entries().next(); }, Ctor.name + " entries");
                assertThrowsTypeError(function () { view.keys().next(); }, Ctor.name + " keys");
                assertThrowsTypeError(function () { view.values().next(); }, Ctor.name + " values");
                assertThrowsTypeError(function () { view.set([]); }, Ctor.name + " set array-like");
                assertThrowsTypeError(function () { view.set(new Ctor(0)); }, Ctor.name + " set typed array");
            });
        }
    },
    {
        name: "set, subarray and slice use the current element range",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(4 * bpe, { maxByteLength: 8 * bpe });
                var auto = new Ctor(rab);
                var source = new Ctor([7, 8, 9, 10]);

                auto.set(source);
                assertSameElements([7, 8, 9, 10], elements(auto), Ctor.name + " initial set");

                rab.resize(2 * bpe);
                auto.set([11, 12]);
                assertSameElements([11, 12], elements(auto), Ctor.name + " set after shrink");
                assert.throws(function () { auto.set([1, 2, 3]); }, RangeError, Ctor.name + " set too long");

                rab.resize(4 * bpe);
                assertSameElements([11, 12, 0, 0], elements(auto), Ctor.name + " set after grow");

                var trackingSub = auto.subarray(1);
                assert.areEqual(3, trackingSub.length, Ctor.name + " auto subarray length");
                assert.areEqual(1 * bpe, trackingSub.byteOffset, Ctor.name + " auto subarray offset");
                rab.resize(6 * bpe);
                assert.areEqual(5, trackingSub.length, Ctor.name + " subarray of auto tracks grow");
                rab.resize(3 * bpe);
                assert.areEqual(2, trackingSub.length, Ctor.name + " subarray of auto tracks shrink");

                var fixedSub = auto.subarray(1, 3);
                assert.areEqual(2, fixedSub.length, Ctor.name + " fixed subarray length");
                rab.resize(6 * bpe);
                assert.areEqual(2, fixedSub.length, Ctor.name + " fixed subarray stays fixed");
                var deepOffset = new Ctor(rab, 2 * bpe, 1);

                rab.resize(1 * bpe);
                assert.areEqual(0, fixedSub.length, Ctor.name + " fixed subarray goes OOB");
                assert.areEqual(1, new Ctor(rab).subarray(0).length, Ctor.name + " one-element auto subarray");

                assert.throws(function () { deepOffset.subarray(0); }, RangeError,
                    Ctor.name + " OOB offset subarray");

                rab.resize(4 * bpe);
                var sliced = auto.slice();
                assert.isFalse(sliced.buffer.resizable, Ctor.name + " slice buffer is fixed");
                assert.areEqual(4, sliced.length, Ctor.name + " slice length");

                rab.resize(2 * bpe);
                assert.areEqual(2, auto.slice().length, Ctor.name + " slice of in-bounds auto after shrink");
                var offsetAuto = new Ctor(rab, 2 * bpe);
                rab.resize(1 * bpe);
                assertThrowsTypeError(function () { offsetAuto.slice(); }, Ctor.name + " slice on OOB auto");
            });
        }
    },
    {
        name: "TypedArray species constructors receive buffer/offset for auto subarrays",
        body() {
            class SpeciesUint8Array extends Uint8Array {}

            var rab = new ArrayBuffer(8, { maxByteLength: 16 });
            var view = new SpeciesUint8Array(rab, 2);
            var sub = view.subarray(1);

            assert.isTrue(sub instanceof SpeciesUint8Array, "auto subarray preserves species");
            assert.areEqual(3, sub.byteOffset, "auto subarray offset");
            assert.areEqual(5, sub.length, "auto subarray tracks source range");

            rab.resize(12);
            assert.areEqual(9, sub.length, "species auto subarray tracks grow");

            var fixedSub = view.subarray(1, 4);
            assert.isTrue(fixedSub instanceof SpeciesUint8Array, "fixed subarray preserves species");
            assert.areEqual(3, fixedSub.length, "fixed species subarray length");
            rab.resize(16);
            assert.areEqual(3, fixedSub.length, "fixed species subarray stays fixed");

            var sliced = view.slice(1, 3);
            assert.isTrue(sliced instanceof SpeciesUint8Array, "slice preserves species");
            assert.areEqual(2, sliced.length, "slice species length");
            assert.areEqual(2, sliced.buffer.byteLength, "slice creates a fixed buffer of the copied length");
        }
    },
    {
        name: "fill and copyWithin operate on the current length",
        body() {
            constructors.forEach(function (Ctor) {
                var bpe = Ctor.BYTES_PER_ELEMENT;
                var rab = new ArrayBuffer(4 * bpe, { maxByteLength: 8 * bpe });
                var view = new Ctor(rab);
                for (var i = 0; i < 4; i++) {
                    view[i] = i;
                }

                rab.resize(2 * bpe);
                view.fill(5);
                assertSameElements([5, 5], elements(view), Ctor.name + " fill after shrink");

                rab.resize(4 * bpe);
                assertSameElements([5, 5, 0, 0], elements(view), Ctor.name + " fill after grow");
                view.copyWithin(0, 2);
                assertSameElements([0, 0, 0, 0], elements(view), Ctor.name + " copyWithin after grow");

                view[0] = 1;
                view[1] = 2;
                view[2] = 3;
                view[3] = 4;
                rab.resize(2 * bpe);
                view.copyWithin(0, 1);
                assertSameElements([2, 2], elements(view), Ctor.name + " copyWithin after shrink");
            });
        }
    },
    {
        name: "Iterators follow grow and shrink and throw for fixed OOB views",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            var view = new Uint8Array(rab);
            for (var i = 0; i < 4; i++) {
                view[i] = i;
            }

            var iterator = view.values();
            assert.strictEqual(0, iterator.next().value);
            rab.resize(6);
            assert.strictEqual(1, iterator.next().value);
            assert.strictEqual(2, iterator.next().value);
            assert.strictEqual(3, iterator.next().value);
            assert.strictEqual(0, iterator.next().value);
            assert.strictEqual(0, iterator.next().value);
            assert.isTrue(iterator.next().done, "auto iterator ends at grown length");

            rab.resize(2);
            var shrinkIterator = view.entries();
            assert.strictEqual(0, shrinkIterator.next().value[0]);
            assert.strictEqual(1, shrinkIterator.next().value[0]);
            assert.isTrue(shrinkIterator.next().done, "auto iterator ends at shrunk length");

            var fixed = new Uint8Array(rab, 0, 2);
            var fixedIterator = fixed.values();
            assert.strictEqual(0, fixedIterator.next().value);
            rab.resize(1);
            assertThrowsTypeError(function () { fixedIterator.next(); }, "fixed iterator after shrink");
        }
    },
    {
        name: "OOB views still support subarray when the requested offset fits",
        body() {
            var rab = new ArrayBuffer(10, { maxByteLength: 10 });
            var auto = new Int8Array(rab, 4);
            var fixed = new Int8Array(rab, 4, 2);

            rab.resize(0);
            assert.strictEqual(0, auto.length, "auto offset view OOB");
            assert.strictEqual(0, fixed.length, "fixed offset view OOB");

            var start = {
                valueOf() {
                    rab.resize(10);
                    return 1;
                }
            };

            var autoResult = auto.subarray(start);
            assert.areEqual(4, autoResult.byteOffset, "auto subarray keeps source offset");
            assert.areEqual(6, autoResult.length, "auto subarray tracks restored range");

            rab.resize(0);
            var fixedResult = fixed.subarray(start);
            assert.areEqual(4, fixedResult.byteOffset, "fixed subarray keeps source offset");
            assert.areEqual(0, fixedResult.length, "fixed subarray is zero length");
        }
    },
    {
        name: "Detached resizable-buffer views preserve detached behavior",
        body() {
            var rab = new ArrayBuffer(8, { maxByteLength: 16 });
            var auto = new Uint8Array(rab, 2);
            var fixed = new Uint8Array(rab, 2, 3);

            rab.transfer();
            assert.isTrue(rab.detached, "source buffer detached");
            assert.areEqual(0, auto.length, "detached auto length");
            assert.areEqual(0, auto.byteLength, "detached auto byteLength");
            assert.areEqual(0, auto.byteOffset, "detached auto byteOffset");
            assert.areEqual(0, fixed.length, "detached fixed length");
            assertThrowsTypeError(function () { return auto[0]; }, "detached get");
            assertThrowsTypeError(function () { auto[0] = 1; }, "detached set");
            assertThrowsTypeError(function () { auto.at(0); }, "detached at");
            assertThrowsTypeError(function () { auto.fill(1); }, "detached fill");
            assertThrowsTypeError(function () { auto.set([]); }, "detached set method");
        }
    },
    {
        name: "copyWithin re-reads the live range after index coercion",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            var view = new Uint8Array(rab);

            function reset() {
                rab.resize(4);
                view.set([0, 1, 2, 3]);
            }

            reset();
            view.copyWithin({ valueOf() { rab.resize(2); return 0; } }, 1);
            assertSameElements([1, 1], elements(view), "target coercion shrink");

            reset();
            view.copyWithin(0, { valueOf() { rab.resize(2); return 1; } });
            assertSameElements([1, 1], elements(view), "start coercion shrink");

            reset();
            view.copyWithin(0, 1, { valueOf() { rab.resize(2); return 4; } });
            assertSameElements([1, 1], elements(view), "end coercion shrink");

            reset();
            view.copyWithin(0, { valueOf() { rab.resize(3); return 2; } });
            assertSameElements([2, 1, 2], elements(view), "OOB source leaves target unchanged");

            reset();
            view.copyWithin(1, { valueOf() { rab.resize(3); return 2; } });
            assertSameElements([0, 2, 2], elements(view), "overlapping OOB source leaves target unchanged");

            var floatRab = new ArrayBuffer(16, { maxByteLength: 32 });
            var floats = new Float32Array(floatRab);
            floats.set([0, 1, 2, 3]);
            floats.copyWithin(0, { valueOf() { floatRab.resize(12); return 2; } });
            assertSameElements([2, 1, 2], elements(floats), "float OOB source stays finite");

            reset();
            view.copyWithin({ valueOf() { rab.resize(6); return 0; } }, 2);
            assertSameElements([2, 3, 2, 3, 0, 0], elements(view), "grow after target coercion");

            var fixedRab = new ArrayBuffer(4, { maxByteLength: 4 });
            var fixed = new Uint8Array(fixedRab, 0, 4);
            fixed.set([0, 1, 2, 3]);
            assert.throws(function () {
                fixed.copyWithin({ valueOf() { fixedRab.resize(1); return 0; } }, 1);
            }, TypeError, "fixed OOB copyWithin after coercion throws");
            assert.areEqual(0, fixed.length, "fixed view is OOB");

            var zeroRab = new ArrayBuffer(2, { maxByteLength: 2 });
            var zero = new Uint8Array(zeroRab);
            zero.copyWithin({ valueOf() { zeroRab.resize(0); return 0; } }, 0);
            assert.areEqual(0, zero.length, "zero-length copyWithin is a no-op");
        }
    },
    {
        name: "toLocaleString distinguishes Array and TypedArray receivers",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 4 });
            var view = new Uint8Array(rab, 0, 4);
            view.set([1, 2, 3, 4]);
            rab.resize(0);

            assert.areEqual("", Array.prototype.toLocaleString.call(view),
                "generic Array entry on an OOB TypedArray");
            assert.throws(function () { view.toLocaleString(); }, TypeError,
                "TypedArray entry rejects an OOB receiver");
            assert.isFalse(Array.prototype.toLocaleString === Uint8Array.prototype.toLocaleString,
                "TypedArray has a distinct toLocaleString function");
        }
    },
    {
        name: "map calls the callback over the captured length after species resize",
        body() {
            function mapValues(source, rab, resizeTo) {
                var values = [];
                source.constructor = {
                    [Symbol.species]: function (length) {
                        var result = new Uint8Array(length);
                        rab.resize(resizeTo);
                        return result;
                    }
                };
                var result = source.map(function (value) {
                    values.push(value);
                    return value;
                });
                assert.areEqual(4, result.length, "result keeps captured length");
                assert.isTrue(result instanceof Uint8Array, "result preserves species");
                return values;
            }

            var fixedRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var fixed = new Uint8Array(fixedRab, 0, 4);
            fixed.set([0, 1, 2, 3]);
            assertSameElements([undefined, undefined, undefined, undefined],
                mapValues(fixed, fixedRab, 2), "fixed source shrink");

            var autoRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var auto = new Uint8Array(autoRab);
            auto.set([0, 1, 2, 3]);
            assertSameElements([0, 1, undefined, undefined],
                mapValues(auto, autoRab, 2), "auto source shrink");

            var detachRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var detachSource = new Uint8Array(detachRab, 0, 4);
            detachSource.constructor = {
                [Symbol.species]: function (length) {
                    var result = new Uint8Array(length);
                    detachRab.transfer();
                    return result;
                }
            };
            var detachedValues = [];
            var detachedResult = detachSource.map(function (value) {
                detachedValues.push(value);
                return value;
            });
            assertSameElements([undefined, undefined, undefined, undefined], detachedValues,
                "detached source keeps callback range");
            assert.areEqual(4, detachedResult.length, "detached source result length");
        }
    },
    {
        name: "slice revalidates the source after species construction",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 4 });
            var fixed = new Uint8Array(rab, 0, 4);
            fixed.set([1, 2, 3, 4]);
            fixed.constructor = {
                [Symbol.species]: function (length) {
                    var result = new Uint8Array(length);
                    rab.resize(2);
                    return result;
                }
            };
            assert.throws(function () { fixed.slice(); }, TypeError,
                "fixed source became OOB in the species constructor");

            var autoRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var auto = new Uint8Array(autoRab);
            auto.set([1, 2, 3, 4]);
            auto.constructor = {
                [Symbol.species]: function (length) {
                    var result = new Uint8Array(length);
                    autoRab.resize(2);
                    return result;
                }
            };
            assertSameElements([1, 2, 0, 0], elements(auto.slice()),
                "auto source copies its live range after species shrink");
        }
    },
    {
        name: "with validates the index against the live length after growth",
        body() {
            var rab = new ArrayBuffer(2, { maxByteLength: 5 });
            var view = new Int8Array(rab);
            view[0] = 11;
            view[1] = 22;
            var result = view.with(4, { valueOf() { rab.resize(5); return 123; } });

            assert.areEqual(2, result.length, "result keeps captured length");
            assertSameElements([11, 22], elements(result), "result copies captured range");
            assert.areEqual(5, view.length, "source grew");
            assert.areEqual(0, view[4], "grown element stays zero");

            var zeroRab = new ArrayBuffer(0, { maxByteLength: 1 });
            var zero = new Uint8Array(zeroRab);
            var zeroResult = zero.with(0, { valueOf() { zeroRab.resize(1); return 1; } });
            assert.areEqual(0, zeroResult.length, "zero-length source stays empty");

            var negRab = new ArrayBuffer(0, { maxByteLength: 1 });
            var neg = new Uint8Array(negRab);
            assert.throws(function () {
                neg.with(-1, { valueOf() { negRab.resize(1); return 1; } });
            }, RangeError, "negative index remains invalid after growth");
        }
    },
    {
        name: "sort tolerates a shrinking comparison callback",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            var fixed = new Uint8Array(rab, 0, 4);
            fixed.set([3, 2, 1, 0]);
            var calls = 0;
            fixed.sort(function (a, b) {
                if (calls++ === 0) {
                    rab.resize(2);
                }
                return a - b;
            });
            assert.areEqual(0, fixed.length, "fixed view is OOB");
            rab.resize(4);
            assertSameElements([3, 2, 0, 0], elements(fixed), "fixed bytes unchanged");

            var autoRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var auto = new Uint8Array(autoRab);
            auto.set([3, 2, 1, 0]);
            var autoCalls = 0;
            auto.sort(function (a, b) {
                if (autoCalls++ === 0) {
                    autoRab.resize(2);
                }
                return a - b;
            });
            assert.areEqual(2, auto.length, "auto view shrinks to live length");

            var detachRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var detached = new Uint8Array(detachRab);
            detached.set([3, 2, 1, 0]);
            assert.throws(function () {
                detached.sort(function () {
                    detachRab.transfer();
                    return 0;
                });
            }, TypeError, "detached sort still throws");
        }
    },
    {
        name: "join stringifies OOB and detached positions as empty",
        body() {
            var rab = new ArrayBuffer(8, { maxByteLength: 8 });
            var fixed = new Int8Array(rab);
            fixed.set([0, 1, 2, 3, 4, 5, 6, 7]);
            var separator = { toString() { rab.resize(3); return "-"; } };
            assert.areEqual("0-1-2-----", fixed.join(separator), "fixed OOB empty elements");

            var growRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var grow = new Int8Array(growRab);
            grow.set([0, 1, 2, 3]);
            var growSeparator = { toString() { growRab.resize(6); return "."; } };
            assert.areEqual("0.1.2.3", grow.join(growSeparator), "growth keeps captured length");

            var detachRab = new ArrayBuffer(4, { maxByteLength: 8 });
            var detached = new Int8Array(detachRab);
            detached.set([0, 1, 2, 3]);
            var detachSeparator = { toString() { detachRab.transfer(); return ","; } };
            assert.areEqual(",,,", detached.join(detachSeparator), "detached positions are empty");
        }
    },
    {
        name: "slice skips OOB source elements instead of copying undefined",
        body() {
            var rab = new ArrayBuffer(16, { maxByteLength: 32 });
            var floats = new Float32Array(rab);
            floats.set([1, 2, 3, 4]);
            floats.constructor = {
                [Symbol.species]: function (length) {
                    var result = new Float32Array(length);
                    rab.resize(8);
                    return result;
                }
            };
            var sliced = floats.slice();
            assertSameElements([1, 2, 0, 0], elements(sliced), "float OOB elements stay zero");

            var zeroRab = new ArrayBuffer(16, { maxByteLength: 16 });
            var zeroFloats = new Float32Array(zeroRab);
            zeroFloats.set([1, 2, 3, 4]);
            var start = { valueOf() { zeroRab.resize(0); return 1; } };
            assertSameElements([0, 0, 0], elements(zeroFloats.slice(start)),
                "resize-to-zero source leaves zeros");
        }
    },
    {
        name: "DataView auto-length tracking remains intact",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 12 });
            var dataView = new DataView(rab, 1);
            assert.areEqual(3, dataView.byteLength, "DataView initial auto length");
            rab.resize(8);
            assert.areEqual(7, dataView.byteLength, "DataView tracks grow");
            assert.areEqual(0, dataView.getUint8(0), "DataView reads grown bytes");
            rab.resize(1);
            assert.areEqual(0, dataView.byteLength, "DataView tracks zero remaining bytes");
            assert.areEqual(1, dataView.byteOffset, "DataView keeps an in-bounds offset");
            assert.throws(function () { dataView.getUint8(0); }, RangeError, "DataView zero-length get");
            rab.resize(0);
            assertThrowsTypeError(function () { return dataView.byteOffset; }, "DataView OOB byteOffset");
            assertThrowsTypeError(function () { dataView.getUint8(0); }, "DataView OOB get");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
