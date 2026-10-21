//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S6 flag-on surface: ES2024 growable SharedArrayBuffer and ES2020 non-shared
// Atomics. Runs with -ESSharedArrayBuffer (doc/c2-s6-growable-nonshared.md).
// Atomics.waitAsync, Atomics.pause and BigInt typed arrays are out of scope.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertBytes(buffer, expected, message) {
    var actual = Array.prototype.slice.call(new Uint8Array(buffer));
    assert.areEqual(expected.length, actual.length, (message || "bytes") + ": length");
    for (var i = 0; i < expected.length; i++) {
        assert.areEqual(expected[i], actual[i], (message || "bytes") + "[" + i + "]");
    }
}

function fillBytes(buffer, values) {
    var view = new Uint8Array(buffer);
    for (var i = 0; i < values.length; i++) {
        view[i] = values[i];
    }
}

var tests = [
    {
        name: "flag-on precondition and default SharedArrayBuffer shape",
        body() {
            // Default (no -ESSharedArrayBuffer) runs fail this file at load time:
            // neither global exists. This asserts the flag-on side of the pair.
            assert.areEqual("function", typeof SharedArrayBuffer, "SharedArrayBuffer is installed");
            assert.areEqual("object", typeof Atomics, "Atomics is installed");

            var fixed = new SharedArrayBuffer(8);
            assert.areEqual(8, fixed.byteLength);
            assert.areEqual(8, fixed.maxByteLength);
            assert.isFalse(fixed.growable);
            assert.areEqual(SharedArrayBuffer.prototype.slice, fixed.slice);
            assert.areEqual("[object SharedArrayBuffer]", String(fixed));
            assert.throws(function () { SharedArrayBuffer(1); }, TypeError);
            assert.throws(function () { new SharedArrayBuffer(-1); }, RangeError);
            assert.areEqual(1, new SharedArrayBuffer(1.5).byteLength, "fractional length truncates");
            assert.throws(function () { new SharedArrayBuffer(Math.pow(2, 53)); }, RangeError);
        }
    },
    {
        name: "growable SharedArrayBuffer construction and options",
        body() {
            var growable = new SharedArrayBuffer(4, { maxByteLength: 8 });
            assert.areEqual(4, growable.byteLength);
            assert.areEqual(8, growable.maxByteLength);
            assert.isTrue(growable.growable);
            assertBytes(growable, [0, 0, 0, 0], "initial contents");
            assert.throws(function () { growable.grow(-1); }, RangeError);

            var zero = new SharedArrayBuffer(0, { maxByteLength: 0 });
            assert.areEqual(0, zero.byteLength);
            assert.areEqual(0, zero.maxByteLength);
            assert.isTrue(zero.growable);

            var equal = new SharedArrayBuffer(4, { maxByteLength: 4 });
            assert.isTrue(equal.growable);
            assert.areEqual(4, equal.maxByteLength);

            [null, true, Symbol("s"), "8", 9, undefined, {}].forEach(function (options) {
                var buffer = new SharedArrayBuffer(0, options);
                assert.isFalse(buffer.growable, String(options) + " options");
                assert.areEqual(0, buffer.maxByteLength);
            });
            assert.isFalse(new SharedArrayBuffer(0, { maxByteLength: undefined }).growable);
            assert.isFalse(new SharedArrayBuffer(0, []).growable);

            assert.throws(function () { new SharedArrayBuffer(1, { maxByteLength: 0 }); }, RangeError);
            assert.throws(function () { new SharedArrayBuffer(0, { maxByteLength: -1 }); }, RangeError);
            assert.throws(function () { new SharedArrayBuffer(0, { maxByteLength: Math.pow(2, 53) }); }, RangeError);
            assert.throws(function () { new SharedArrayBuffer(0, { maxByteLength: 7 * Math.pow(2, 50) }); }, RangeError);

            // Fractional maxByteLength truncates, NaN becomes zero (and rejects
            // a nonzero length).
            var fractional = new SharedArrayBuffer(0, { maxByteLength: 4.9 });
            assert.areEqual(4, fractional.maxByteLength);
            assert.areEqual(0, new SharedArrayBuffer(0, { maxByteLength: NaN }).maxByteLength);
            assert.throws(function () { new SharedArrayBuffer(4, { maxByteLength: NaN }); }, RangeError);
        }
    },
    {
        name: "growable SharedArrayBuffer construction coercion order",
        body() {
            var log = [];
            var length = { valueOf: function () { log.push("length"); return 4; } };
            var options = { get maxByteLength() { log.push("get max"); return "8"; } };
            var buffer = new SharedArrayBuffer(length, options);
            assert.areEqual(4, buffer.byteLength);
            assert.areEqual(8, buffer.maxByteLength);
            assert.isTrue(buffer.growable);
            assert.areEqual("length,get max", log.join(","));

            log = [];
            var badMax = {
                valueOf: function () { log.push("valueOf"); return {}; },
                toString: function () { log.push("toString"); return {}; }
            };
            assert.throws(function () { new SharedArrayBuffer(0, { maxByteLength: badMax }); }, TypeError);
            assert.areEqual("valueOf,toString", log.join(","));

            var poison = new Error("poisoned");
            assert.throws(function () {
                new SharedArrayBuffer(0, { get maxByteLength() { throw poison; } });
            }, Error, "maxByteLength getter abrupt completion");
            assert.throws(function () {
                new SharedArrayBuffer({ valueOf: function () { throw poison; } });
            }, Error, "length coercion abrupt completion");
        }
    },
    {
        name: "grow, byteLength tracking and zero-fill",
        body() {
            var buffer = new SharedArrayBuffer(4, { maxByteLength: 16 });
            fillBytes(buffer, [1, 2, 3, 4]);
            assert.areEqual(undefined, buffer.grow(8), "grow returns undefined");
            assert.areEqual(8, buffer.byteLength);
            assertBytes(buffer, [1, 2, 3, 4, 0, 0, 0, 0], "grow zero-fills new bytes");

            buffer.grow(8);
            assert.areEqual(8, buffer.byteLength, "grow to the same length is a no-op");
            buffer.grow(16);
            assert.areEqual(16, buffer.byteLength);
            assertBytes(buffer, [1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], "second grow zero-fills");

            assert.throws(function () { buffer.grow(0); }, RangeError, "grow cannot shrink");
            assert.throws(function () { buffer.grow(17); }, RangeError, "grow past maxByteLength");
            assert.throws(function () { buffer.grow(-1); }, RangeError);
            assert.throws(function () { buffer.grow(Math.pow(2, 53)); }, RangeError);

            // ToIndex semantics: fractional truncates, NaN/undefined become 0.
            var fractional = new SharedArrayBuffer(0, { maxByteLength: 8 });
            fractional.grow(4.9);
            assert.areEqual(4, fractional.byteLength);
            var nan = new SharedArrayBuffer(4, { maxByteLength: 8 });
            assert.throws(function () { nan.grow(NaN); }, RangeError, "NaN becomes zero, below current");
            var empty = new SharedArrayBuffer(0, { maxByteLength: 8 });
            empty.grow(NaN);
            empty.grow(undefined);
            assert.areEqual(0, empty.byteLength);
            assert.throws(function () { empty.grow(Symbol("s")); }, TypeError);

            assert.throws(function () { new SharedArrayBuffer(8).grow(8); }, TypeError, "fixed buffer, same size");
            assert.throws(function () { new SharedArrayBuffer(8).grow(0); }, TypeError, "fixed buffer, smaller");
            assert.throws(function () { new SharedArrayBuffer(8).grow(9); }, TypeError, "fixed buffer, larger");
            assert.throws(function () { SharedArrayBuffer.prototype.grow.call({}, 0); }, TypeError);
            assert.throws(function () { SharedArrayBuffer.prototype.grow.call(new ArrayBuffer(8), 0); }, TypeError);
        }
    },
    {
        name: "maxByteLength and growable accessor properties",
        body() {
            var growable = new SharedArrayBuffer(4, { maxByteLength: 16 });
            var fixed = new SharedArrayBuffer(4);
            assert.areEqual(16, growable.maxByteLength);
            assert.areEqual(4, fixed.maxByteLength);
            assert.isTrue(growable.growable);
            assert.isFalse(fixed.growable);

            ["maxByteLength", "growable"].forEach(function (name) {
                var descriptor = Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, name);
                assert.areEqual("function", typeof descriptor.get, name + " getter");
                assert.areEqual(undefined, descriptor.set, name + " setter");
                assert.isFalse(descriptor.enumerable, name + " enumerable");
                assert.isTrue(descriptor.configurable, name + " configurable");
                assert.throws(function () {
                    Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, name).get.call({});
                }, TypeError, name + " brand check");
                assert.throws(function () {
                    Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, name).get.call(new ArrayBuffer(4));
                }, TypeError, name + " rejects ArrayBuffer");
                assert.throws(function () {
                    Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, name).get.call(1);
                }, TypeError, name + " rejects primitive");
            });

            var growDescriptor = Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "grow");
            assert.areEqual("function", typeof growDescriptor.value);
            assert.areEqual("grow", growDescriptor.value.name);
            assert.areEqual(1, growDescriptor.value.length);
            assert.isFalse(growDescriptor.enumerable);
            assert.isTrue(growDescriptor.configurable);
            assert.isTrue(growDescriptor.writable);
            assert.throws(function () { new growDescriptor.value(); }, TypeError, "grow is not a constructor");
        }
    },
    {
        name: "TypedArray and DataView views track growable SharedArrayBuffer length",
        body() {
            var buffer = new SharedArrayBuffer(4, { maxByteLength: 12 });
            var auto32 = new Int32Array(buffer);
            var auto8 = new Uint8Array(buffer, 2);
            var fixed = new Int32Array(buffer, 0, 1);
            var dataView = new DataView(buffer);
            var fixedView = new DataView(buffer, 0, 4);

            assert.areEqual(1, auto32.length);
            assert.areEqual(2, auto8.length);
            assert.areEqual(1, fixed.length);
            assert.areEqual(4, dataView.byteLength);
            assert.areEqual(4, fixedView.byteLength);

            buffer.grow(12);
            assert.areEqual(3, auto32.length, "auto-length TypedArray grows");
            assert.areEqual(12, auto32.byteLength);
            assert.areEqual(10, auto8.length, "offset auto-length TypedArray grows");
            assert.areEqual(1, fixed.length, "explicit-length TypedArray stays fixed");
            assert.areEqual(12, dataView.byteLength, "auto-length DataView grows");
            assert.areEqual(4, fixedView.byteLength, "explicit-length DataView stays fixed");

            // Views remain usable over the grown range and zero-filled bytes.
            assert.areEqual(0, auto8[8]);
            assert.areEqual(0, auto8[9]);
            auto32[2] = 0x41424344;
            assert.areEqual(0x41424344, auto32[2]);

            // subarray of an auto-length view stays auto-length.
            var sub = auto32.subarray(1);
            assert.areEqual(2, sub.length);
            buffer.grow(12);
            assert.areEqual(2, sub.length);
            var tail = auto32.subarray(3);
            assert.areEqual(0, tail.length);

            // A fixed view into a growable buffer can be OOB only in impossible
            // shrink scenarios; construction still validates against current length.
            assert.throws(function () { new Int32Array(buffer, 0, 4); }, RangeError);
            assert.throws(function () { new Int32Array(buffer, 13); }, RangeError);
            assert.throws(function () { new DataView(buffer, 0, 13); }, RangeError);
        }
    },
    {
        name: "slice and species on growable SharedArrayBuffer",
        body() {
            var buffer = new SharedArrayBuffer(4, { maxByteLength: 16 });
            fillBytes(buffer, [1, 2, 3, 4]);
            buffer.grow(8);
            fillBytes(buffer, [1, 2, 3, 4, 5, 6, 7, 8]);

            var copy = buffer.slice();
            assert.areEqual(8, copy.byteLength);
            assert.isFalse(copy.growable, "slice result is fixed-length");
            assert.areEqual(8, copy.maxByteLength);
            assertBytes(copy, [1, 2, 3, 4, 5, 6, 7, 8]);

            var middle = buffer.slice(2, 6);
            assert.areEqual(4, middle.byteLength);
            assertBytes(middle, [3, 4, 5, 6]);
            assert.throws(function () { middle.grow(8); }, TypeError);

            var speciesSeen = -1;
            buffer.constructor = {
                [Symbol.species]: function (length) {
                    speciesSeen = length;
                    return new SharedArrayBuffer(length, { maxByteLength: 32 });
                }
            };
            var grown = buffer.slice(1, 5);
            assert.areEqual(4, speciesSeen);
            assert.areEqual(4, grown.byteLength);
            assert.isTrue(grown.growable, "species result may be growable");
            assert.areEqual(32, grown.maxByteLength);
            assertBytes(grown.subarray ? grown.subarray(0) : new Uint8Array(grown), [2, 3, 4, 5]);

            var original = buffer.constructor;
            buffer.constructor = { [Symbol.species]: function () { return new SharedArrayBuffer(1); } };
            assert.throws(function () { buffer.slice(); }, TypeError, "species result smaller than requested");
            buffer.constructor = { [Symbol.species]: function () { return buffer; } };
            assert.throws(function () { buffer.slice(); }, TypeError, "species cannot return the same buffer");
            buffer.constructor = original;
        }
    },
    {
        name: "Atomics read-modify-write on non-shared integer views",
        body() {
            var ctors = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];
            ctors.forEach(function (TA) {
                var view = new TA(new ArrayBuffer(TA.BYTES_PER_ELEMENT * 4));
                var value = TA === Int8Array ? -8 : (TA === Int16Array ? -800 : (TA === Int32Array ? -80000 : 100));
                assert.areEqual(0, Atomics.add(view, 0, value), TA.name + " add returns old value");
                assert.areEqual(value, Atomics.load(view, 0), TA.name + " add stored value");
                assert.areEqual(0, Atomics.exchange(view, 1, 7), TA.name + " exchange returns old");
                assert.areEqual(7, Atomics.load(view, 1));
                assert.areEqual(7, Atomics.compareExchange(view, 1, 7, 9), TA.name + " compareExchange match returns old");
                assert.areEqual(9, Atomics.load(view, 1));
                assert.areEqual(9, Atomics.compareExchange(view, 1, 7, 11), TA.name + " compareExchange mismatch returns old");
                assert.areEqual(9, Atomics.load(view, 1));
                assert.areEqual(value, Atomics.sub(view, 0, value), TA.name + " sub returns old");
                assert.areEqual(0, Atomics.load(view, 0));
                assert.areEqual(0, Atomics.and(view, 0, 0), TA.name + " and returns old");
                assert.areEqual(0, Atomics.or(view, 0, 6), TA.name + " or returns old");
                assert.areEqual(6, Atomics.load(view, 0));
                assert.areEqual(6, Atomics.xor(view, 0, 3), TA.name + " xor returns old");
                assert.areEqual(5, Atomics.load(view, 0));
                assert.areEqual(12, Atomics.store(view, 2, 12), TA.name + " store returns value");
                assert.areEqual(12, Atomics.load(view, 2));
                assert.areEqual(12, new TA(view.buffer)[2], TA.name + " non-atomic read sees atomic store");
            });

            // Atomics operations need not be shared and do not detach/remap.
            var ab = new ArrayBuffer(8);
            var view = new Int32Array(ab);
            assert.areEqual(0, Atomics.add(view, 0, 1));
            assert.areEqual(1, new Int32Array(ab)[0]);
            assert.areEqual(2, Atomics.store(view, 1, 2));
            assert.areEqual(2, new Int32Array(ab)[1]);
            assert.isTrue(Atomics.isLockFree(1));
            assert.isTrue(Atomics.isLockFree(4));
            assert.isFalse(Atomics.isLockFree(8));
        }
    },
    {
        name: "non-shared Atomics validation and error order",
        body() {
            var ab = new ArrayBuffer(16);
            var i32 = new Int32Array(ab);
            var poisoned = { valueOf: function () { throw new Error("poisoned"); } };

            // Type validation precedes index and value coercion.
            assert.areEqual(0, Atomics.add(new Int8Array(ab), 0, 1), "Int8Array is accepted");
            assert.areEqual(1, new Int8Array(ab)[0]);
            assert.throws(function () { Atomics.add(new Float32Array(ab), poisoned, poisoned); }, TypeError, "float views rejected");
            assert.throws(function () { Atomics.add(new Uint8ClampedArray(ab), poisoned, poisoned); }, TypeError, "clamped rejected");
            assert.throws(function () { Atomics.add({}, poisoned, poisoned); }, TypeError, "non-view rejected");
            assert.throws(function () { Atomics.add(i32, 4, poisoned); }, RangeError, "index range precedes value coercion");
            assert.throws(function () { Atomics.add(i32, poisoned, 1); }, Error, "index coercion is observable");
            assert.throws(function () { Atomics.load(i32, -1); }, RangeError);
            assert.throws(function () { Atomics.load(i32, Math.pow(2, 53)); }, RangeError);
            assert.throws(function () { new Atomics.load(i32, 0); }, TypeError, "not a constructor");
            assert.areEqual(0, Atomics.add(i32, 1.5, 1), "fractional index truncates to 1");
            assert.areEqual(1, Atomics.load(i32, 1));
        }
    },
    {
        name: "Atomics.wait and notify non-shared semantics",
        body() {
            var ab = new ArrayBuffer(16);
            var i32 = new Int32Array(ab);
            var poisoned = { valueOf: function () { throw new Error("poisoned"); } };

            // wait remains shared-only and validates before coercing arguments.
            assert.throws(function () { Atomics.wait(i32, 0, 0, 0); }, TypeError);
            assert.throws(function () { Atomics.wait(i32, poisoned, poisoned, poisoned); }, TypeError);
            assert.throws(function () { Atomics.wait(new Int16Array(ab), 0, 0, 0); }, TypeError);

            // notify accepts a non-shared Int32Array, evaluates count, then 0.
            assert.areEqual(0, Atomics.notify(i32, 0));
            assert.areEqual(0, Atomics.notify(i32, 0, 0));
            assert.areEqual(0, Atomics.notify(i32, 0, 5));
            assert.throws(function () { Atomics.notify(i32, 0, poisoned); }, Error, "count is coerced before returning 0");
            assert.throws(function () { Atomics.notify(i32, poisoned, poisoned); }, Error, "index is coerced first");
            assert.throws(function () { Atomics.notify(new Int16Array(ab), poisoned, poisoned); }, TypeError, "non-Int32 rejected before coercion");

            // Detached views and out-of-bounds resizable views are refused.
            var detachedBuffer = new ArrayBuffer(8);
            var detachedView = new Int32Array(detachedBuffer);
            detachedBuffer.transfer();
            assert.throws(function () { Atomics.load(detachedView, 0); }, TypeError);
            assert.throws(function () { Atomics.notify(detachedView, 0); }, TypeError);

            var resizable = new ArrayBuffer(8, { maxByteLength: 8 });
            var tracking = new Int32Array(resizable);
            resizable.resize(0);
            assert.throws(function () { Atomics.load(tracking, 0); }, RangeError, "zero-length view: index out of range");

            var resizable2 = new ArrayBuffer(8, { maxByteLength: 8 });
            var fixed = new Int32Array(resizable2, 0, 2);
            resizable2.resize(4);
            assert.throws(function () { Atomics.load(fixed, 0); }, TypeError, "OOB fixed view is rejected");

            // Index coercion may shrink the buffer; the operation must not touch
            // the retained capacity outside the current range.
            var resizable3 = new ArrayBuffer(8, { maxByteLength: 8 });
            var shrinkView = new Int32Array(resizable3);
            assert.throws(function () {
                Atomics.add(shrinkView, { valueOf: function () { resizable3.resize(4); return 1; } }, 1);
            }, TypeError, "index getter shrinks and OOBs the view");
            resizable3.resize(0);
            assert.throws(function () {
                Atomics.add(shrinkView, { valueOf: function () { return 0; } }, 1);
            }, RangeError, "length captured before index coercion");

            // Value coercion can also detach or shrink the buffer before the
            // atomic instruction; the captured raw pointer must not be used.
            var detachDuringValue = new ArrayBuffer(8);
            var detachedByValue = new Int32Array(detachDuringValue);
            assert.throws(function () {
                Atomics.add(detachedByValue, 0, { valueOf: function () { detachDuringValue.transfer(); return 1; } });
            }, TypeError, "value getter detaches the buffer");
            var shrinkDuringValue = new ArrayBuffer(8, { maxByteLength: 8 });
            var shrunkByValue = new Int32Array(shrinkDuringValue);
            assert.throws(function () {
                Atomics.add(shrunkByValue, 1, { valueOf: function () { shrinkDuringValue.resize(4); return 1; } });
            }, TypeError, "value getter shrinks the view out of bounds");
            assert.areEqual(0, Atomics.add(shrunkByValue, 0, { valueOf: function () { return 1; } }));

            // compareExchange coerces expectedValue before replacementValue.
            var ordered = new Int32Array(new ArrayBuffer(8));
            var log = [];
            assert.areEqual(0, Atomics.compareExchange(ordered, 0,
                { valueOf: function () { log.push("expected"); return 0; } },
                { valueOf: function () { log.push("replacement"); return 5; } }));
            assert.areEqual("expected,replacement", log.join(","));
            assert.areEqual(5, Atomics.load(ordered, 0));
        }
    },
    {
        name: "Atomics revalidation follows value conversion",
        body() {
            var ab = new ArrayBuffer(8, { maxByteLength: 8 });
            var view = new Int32Array(ab);

            // Index coercion shrinks below index 1; the value getter grows the
            // buffer back. Revalidation happens after value conversion, so the
            // operation succeeds and sees the restored range.
            assert.areEqual(7, Atomics.store(view,
                { valueOf: function () { ab.resize(4); return 1; } },
                { valueOf: function () { ab.resize(8); return 7; } }));
            assert.areEqual(7, Atomics.load(view, 1));

            var ab2 = new ArrayBuffer(8, { maxByteLength: 8 });
            var view2 = new Int32Array(ab2);
            assert.areEqual(0, Atomics.add(view2,
                { valueOf: function () { ab2.resize(4); return 1; } },
                { valueOf: function () { ab2.resize(8); return 5; } }));
            assert.areEqual(5, Atomics.load(view2, 1));

            // Order: index coercion runs before value coercion, and a value
            // getter error is not replaced by the early OOB TypeError.
            var ab3 = new ArrayBuffer(8, { maxByteLength: 8 });
            var view3 = new Int32Array(ab3);
            var indexError = new RangeError("index");
            var valueError = new Error("value");
            assert.throws(function () {
                Atomics.add(view3,
                    { valueOf: function () { ab3.resize(4); throw indexError; } },
                    { valueOf: function () { throw valueError; } });
            }, RangeError, "index coercion error precedes value coercion");
            ab3.resize(8);
            assert.throws(function () {
                Atomics.add(view3,
                    { valueOf: function () { ab3.resize(4); return 1; } },
                    { valueOf: function () { throw valueError; } });
            }, Error, "value coercion error precedes revalidation");
            var caughtValue;
            ab3.resize(8);
            try {
                Atomics.add(view3,
                    { valueOf: function () { ab3.resize(4); return 1; } },
                    { valueOf: function () { throw valueError; } });
            } catch (e) {
                caughtValue = e;
            }
            assert.strictEqual(valueError, caughtValue, "value error identity");

            // Without a regrow the post-conversion revalidation throws.
            var ab4 = new ArrayBuffer(8, { maxByteLength: 8 });
            var view4 = new Int32Array(ab4);
            assert.throws(function () {
                Atomics.add(view4,
                    { valueOf: function () { ab4.resize(4); return 1; } },
                    3);
            }, TypeError, "still out of bounds after conversion");

            // compareExchange converts both values before revalidating.
            var ab5 = new ArrayBuffer(8, { maxByteLength: 8 });
            var view5 = new Int32Array(ab5);
            assert.areEqual(0, Atomics.compareExchange(view5,
                { valueOf: function () { ab5.resize(4); return 1; } },
                { valueOf: function () { ab5.resize(8); return 0; } },
                { valueOf: function () { return 9; } }));
            assert.areEqual(9, Atomics.load(view5, 1));
        }
    },
    {
        name: "growable buffer virtual-length invariant and getter-only setters",
        body() {
            // 64 KiB is a valid asm.js virtual-buffer length. A growable SAB
            // must not be classified for that path: the explicit full-length
            // view stays fixed while the auto-length view grows.
            var big = new SharedArrayBuffer(65536, { maxByteLength: 131072 });
            var explicitFull = new Int8Array(big, 0, 65536);
            var autoBig = new Uint8Array(big);
            explicitFull[0] = 11;
            explicitFull[65535] = 22;
            assert.areEqual(65536, explicitFull.length);
            big.grow(131072);
            assert.areEqual(65536, explicitFull.length, "virtual-looking fixed view stays fixed");
            assert.areEqual(131072, autoBig.length, "auto view tracks the grown length");
            assert.areEqual(11, explicitFull[0]);
            assert.areEqual(22, explicitFull[65535]);
            assert.areEqual(0, autoBig[65536]);
            assert.areEqual(0, autoBig[131071]);
            explicitFull[65536 - 1] = 33;
            assert.areEqual(33, autoBig[65536 - 1]);

            // maxByteLength/growable are getter-only; assignment is ignored and
            // a later grow still uses the original limit.
            var setters = new SharedArrayBuffer(4, { maxByteLength: 8 });
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "maxByteLength").set);
            assert.areEqual(undefined, Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "growable").set);
            setters.maxByteLength = 4096;
            setters.growable = false;
            assert.areEqual(8, setters.maxByteLength, "maxByteLength assignment ignored");
            assert.isTrue(setters.growable, "growable assignment ignored");
            setters.grow(8);
            setters.grow(8);
            assert.areEqual(8, setters.byteLength, "regrow to the same length is a no-op");
            assert.throws(function () { setters.grow(9); }, RangeError, "original limit still applies");
        }
    },
    {
        name: "Atomics on growable SharedArrayBuffer views",
        body() {
            var buffer = new SharedArrayBuffer(4, { maxByteLength: 16 });
            var auto = new Int32Array(buffer);
            assert.areEqual(42, Atomics.store(auto, 0, 42));
            assert.areEqual(42, Atomics.load(auto, 0));

            buffer.grow(16);
            assert.areEqual(4, auto.length);
            assert.areEqual(7, Atomics.store(auto, 3, 7));
            assert.areEqual(7, Atomics.load(auto, 3));
            assert.areEqual(42, Atomics.load(auto, 0), "growth preserves existing bytes");
            assert.areEqual(0, Atomics.load(auto, 1), "growth zero-fills");

            var fixed = new Int32Array(buffer, 0, 1);
            assert.areEqual(42, Atomics.load(fixed, 0));
            assert.throws(function () { Atomics.load(fixed, 1); }, RangeError);

            // A growable SAB remains a valid Atomics operand after growing.
            assert.areEqual(7, Atomics.compareExchange(auto, 3, 7, 9));
            assert.areEqual(9, Atomics.load(auto, 3));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
