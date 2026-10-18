//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2024/ES2025 resizable ArrayBuffer core and transfer
// (doc/c2-s8-resizable.md). TypedArray length tracking, immutable buffers and
// Float16Array are deliberately out of scope.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertBytes(arrayBuffer, expected, message) {
    var actual = Array.prototype.slice.call(new Uint8Array(arrayBuffer));
    assert.areEqual(expected.length, actual.length, (message || "bytes") + ": length");
    for (var i = 0; i < expected.length; i++) {
        assert.areEqual(expected[i], actual[i], (message || "bytes") + "[" + i + "]");
    }
}

function fill(arrayBuffer, values) {
    var view = new Uint8Array(arrayBuffer);
    for (var i = 0; i < values.length; i++) {
        view[i] = values[i];
    }
}

var tests = [
    {
        name: "ArrayBuffer construction and maxByteLength options",
        body() {
            var empty = new ArrayBuffer();
            assert.areEqual(0, empty.byteLength);
            assert.areEqual(0, empty.maxByteLength);
            assert.isFalse(empty.resizable);

            var fixed = new ArrayBuffer(8);
            assert.areEqual(8, fixed.byteLength);
            assert.areEqual(8, fixed.maxByteLength);
            assert.isFalse(fixed.resizable);

            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            assert.areEqual(4, rab.byteLength);
            assert.areEqual(8, rab.maxByteLength);
            assert.isTrue(rab.resizable);
            assertBytes(rab, [0, 0, 0, 0], "resizable initial contents");

            var zero = new ArrayBuffer(0, { maxByteLength: 0 });
            assert.areEqual(0, zero.byteLength);
            assert.areEqual(0, zero.maxByteLength);
            assert.isTrue(zero.resizable);

            var equal = new ArrayBuffer(4, { maxByteLength: 4 });
            assert.isTrue(equal.resizable);
            assert.areEqual(4, equal.maxByteLength);

            // Non-object options and an undefined maxByteLength produce a fixed buffer.
            [null, true, Symbol("s"), "8", 9, undefined].forEach(function (options) {
                var buffer = new ArrayBuffer(0, options);
                assert.isFalse(buffer.resizable);
                assert.areEqual(0, buffer.maxByteLength);
            });
            var absent = new ArrayBuffer(0, {});
            assert.isFalse(absent.resizable);
            var undefinedMax = new ArrayBuffer(0, { maxByteLength: undefined });
            assert.isFalse(undefinedMax.resizable);

            assert.throws(function () { new ArrayBuffer(1, { maxByteLength: 0 }); }, RangeError);
            assert.throws(function () { new ArrayBuffer(0, { maxByteLength: -1 }); }, RangeError);
            assert.throws(function () { new ArrayBuffer(0, { maxByteLength: Math.pow(2, 53) }); }, RangeError);
            assert.throws(function () { new ArrayBuffer(0, { maxByteLength: Math.pow(2, 53) - 1 }); }, RangeError);
            assert.throws(function () { new ArrayBuffer(0, { maxByteLength: 7 * Math.pow(2, 50) }); }, RangeError);
        }
    },
    {
        name: "ArrayBuffer construction coercion and ordering",
        body() {
            var log = [];
            var length = { valueOf: function () { log.push("length"); return 4; } };
            var options = { get maxByteLength() { log.push("get max"); return "8"; } };
            var buffer = new ArrayBuffer(length, options);
            assert.areEqual(4, buffer.byteLength);
            assert.areEqual(8, buffer.maxByteLength);
            assert.isTrue(buffer.resizable);
            assert.areEqual("length,get max", log.join(","));

            log = [];
            var badMax = {
                valueOf: function () { log.push("valueOf"); return {}; },
                toString: function () { log.push("toString"); return {}; }
            };
            assert.throws(function () { new ArrayBuffer(0, { maxByteLength: badMax }); }, TypeError);
            assert.areEqual("valueOf,toString", log.join(","));

            var poison = new Error("poisoned");
            assert.throws(function () {
                new ArrayBuffer(0, { get maxByteLength() { throw poison; } });
            }, Error, "maxByteLength getter abrupt completion");

            // byteLength/maxByteLength validation is independent of newTarget
            // prototype handling (Chakra's Reflect.construct pre-creates the
            // result object and reads newTarget.prototype itself).
            var caught;
            try {
                new ArrayBuffer(10, { maxByteLength: 0 });
            } catch (e) {
                caught = e;
            }
            assert.isTrue(caught instanceof RangeError, "maxByteLength < length throws RangeError");

            caught = null;
            try {
                Reflect.construct(ArrayBuffer, [10, { maxByteLength: 0 }], function () { });
            } catch (e) {
                caught = e;
            }
            assert.isTrue(caught instanceof RangeError, "maxByteLength < length throws RangeError through Reflect.construct");
        }
    },
    {
        name: "maxByteLength and resizable accessors",
        body() {
            var maxDescriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "maxByteLength");
            assert.strictEqual("function", typeof maxDescriptor.get);
            assert.strictEqual(undefined, maxDescriptor.set);
            assert.isFalse(maxDescriptor.enumerable);
            assert.isTrue(maxDescriptor.configurable);
            assert.strictEqual("get maxByteLength", maxDescriptor.get.name);
            assert.strictEqual(0, maxDescriptor.get.length);

            var resizableDescriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "resizable");
            assert.strictEqual("function", typeof resizableDescriptor.get);
            assert.strictEqual(undefined, resizableDescriptor.set);
            assert.isFalse(resizableDescriptor.enumerable);
            assert.isTrue(resizableDescriptor.configurable);
            assert.strictEqual("get resizable", resizableDescriptor.get.name);
            assert.strictEqual(0, resizableDescriptor.get.length);

            assert.throws(function () { ArrayBuffer.prototype.maxByteLength; }, TypeError);
            assert.throws(function () { maxDescriptor.get(); }, TypeError);
            assert.throws(function () { ArrayBuffer.prototype.resizable; }, TypeError);
            assert.throws(function () { resizableDescriptor.get(); }, TypeError);
            assert.throws(function () { maxDescriptor.get.call({}); }, TypeError);
            assert.throws(function () { resizableDescriptor.get.call({}); }, TypeError);
            assert.throws(function () { maxDescriptor.get.call("ArrayBuffer"); }, TypeError);

            if (typeof SharedArrayBuffer !== "undefined") {
                var sab = new SharedArrayBuffer(4);
                assert.throws(function () { maxDescriptor.get.call(sab); }, TypeError);
                assert.throws(function () { resizableDescriptor.get.call(sab); }, TypeError);
            }

            var detachedDescriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "detached");
            assert.strictEqual("function", typeof detachedDescriptor.get);
            assert.strictEqual(undefined, detachedDescriptor.set);
            assert.strictEqual("get detached", detachedDescriptor.get.name);
            assert.isFalse(detachedDescriptor.enumerable);
            assert.isTrue(detachedDescriptor.configurable);

            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            rab.resize(2);
            assert.areEqual(8, rab.maxByteLength);
            assert.isTrue(rab.resizable);
            assert.isFalse(rab.detached);

            var fixed = new ArrayBuffer(4);
            assert.throws(function () { fixed.resize(2); }, TypeError);
            assert.areEqual(4, fixed.byteLength);

            var detached = new ArrayBuffer(1, { maxByteLength: 1 });
            detached.transfer();
            assert.isTrue(detached.detached);
            assert.isTrue(detached.resizable);
            assert.areEqual(0, detached.maxByteLength);
            assert.areEqual(0, detached.byteLength);
        }
    },
    {
        name: "ArrayBuffer.prototype.resize grow, shrink, zero-fill and detach",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            var view = new Uint8Array(rab);
            view[0] = 1;
            view[1] = 2;
            view[2] = 3;
            view[3] = 4;

            assert.isUndefined(rab.resize(6));
            assert.areEqual(6, rab.byteLength);
            assert.areEqual(8, rab.maxByteLength);
            assertBytes(rab, [1, 2, 3, 4, 0, 0], "grow zero-fill");

            rab.resize(2);
            assert.areEqual(2, rab.byteLength);
            assertBytes(rab, [1, 2], "shrink");

            rab.resize(8);
            assertBytes(rab, [1, 2, 0, 0, 0, 0, 0, 0], "shrink then grow zero-fill");

            rab.resize(8);
            assert.areEqual(8, rab.byteLength);
            rab.resize(0);
            assert.areEqual(0, rab.byteLength);
            rab.resize();
            assert.areEqual(0, rab.byteLength);

            rab.resize(2.9);
            assert.areEqual(2, rab.byteLength);
            rab.resize(NaN);
            assert.areEqual(0, rab.byteLength);
            rab.resize(null);
            assert.areEqual(0, rab.byteLength);

            assert.throws(function () { rab.resize(-1); }, RangeError);
            assert.throws(function () { rab.resize(9); }, RangeError);
            assert.throws(function () { rab.resize(Infinity); }, RangeError);
            assert.throws(function () { rab.resize(Symbol("length")); }, TypeError);
            assert.areEqual(0, rab.byteLength);

            // A resize is not a detach; slice and views keep working within bounds.
            rab.resize(4);
            fill(rab, [5, 6, 7, 8]);
            var copy = rab.slice(1, 3);
            assertBytes(copy, [6, 7], "slice after resize");
            assert.isFalse(copy.resizable);
            assert.areEqual(2, copy.byteLength);

            // Detach during length coercion is observed before the detached TypeError.
            var log = [];
            var detachedDuringCoercion = new ArrayBuffer(4, { maxByteLength: 8 });
            assert.throws(function () {
                detachedDuringCoercion.resize({ valueOf: function () {
                    log.push("valueOf");
                    ArrayBuffer.prototype.transfer.call(detachedDuringCoercion);
                    return 2;
                } });
            }, TypeError);
            assert.areEqual("valueOf", log.join(","));
            assert.areEqual(0, detachedDuringCoercion.byteLength);
            assert.isTrue(detachedDuringCoercion.resizable);

            // Already-detached buffers still coerce, then throw TypeError.
            log = [];
            assert.throws(function () {
                detachedDuringCoercion.resize({ valueOf: function () { log.push("valueOf"); return 1; } });
            }, TypeError);
            assert.areEqual("valueOf", log.join(","));
        }
    },
    {
        name: "ArrayBuffer.prototype.transfer copies, detaches and preserves resizability",
        body() {
            var fixed = new ArrayBuffer(4);
            fill(fixed, [1, 2, 3, 4]);
            var dest = fixed.transfer(5);
            assert.areEqual(0, fixed.byteLength);
            assert.isTrue(fixed.resizable === false);
            assert.throws(function () { fixed.slice(); }, TypeError);
            assert.isFalse(dest.resizable);
            assert.areEqual(5, dest.byteLength);
            assert.areEqual(5, dest.maxByteLength);
            assertBytes(dest, [1, 2, 3, 4, 0], "fixed transfer grow");

            var same = new ArrayBuffer(3);
            fill(same, [7, 8, 9]);
            var sameDest = same.transfer();
            assert.areEqual(3, sameDest.byteLength);
            assertBytes(sameDest, [7, 8, 9], "transfer default length");

            var smaller = new ArrayBuffer(4);
            fill(smaller, [1, 2, 3, 4]);
            assertBytes(smaller.transfer(2), [1, 2], "transfer shrink");
            var zero = new ArrayBuffer(4);
            fill(zero, [1, 2, 3, 4]);
            assertBytes(zero.transfer(0), [], "transfer zero");

            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(rab, [1, 2, 3, 4]);
            var rabDest = rab.transfer(6);
            assert.areEqual(0, rab.byteLength);
            assert.throws(function () { rab.slice(); }, TypeError);
            assert.isTrue(rab.resizable);
            assert.isTrue(rabDest.resizable);
            assert.areEqual(6, rabDest.byteLength);
            assert.areEqual(8, rabDest.maxByteLength);
            assertBytes(rabDest, [1, 2, 3, 4, 0, 0], "resizable transfer grow");
            rabDest.resize(8);
            assertBytes(rabDest, [1, 2, 3, 4, 0, 0, 0, 0], "transferred buffer stays resizable");

            var rabSame = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(rabSame, [1, 2, 3, 4]);
            var rabSameDest = rabSame.transfer();
            assert.areEqual(4, rabSameDest.byteLength);
            assert.areEqual(8, rabSameDest.maxByteLength);
            assertBytes(rabSameDest, [1, 2, 3, 4], "resizable transfer default");

            var overflow = new ArrayBuffer(4, { maxByteLength: 8 });
            assert.throws(function () { overflow.transfer(9); }, RangeError);
            assert.areEqual(4, overflow.byteLength, "failed transfer leaves the source attached");

            var log = [];
            var badLength = {
                valueOf: function () { log.push("valueOf"); return {}; },
                toString: function () { log.push("toString"); return {}; }
            };
            assert.throws(function () { new ArrayBuffer(0).transfer(badLength); }, TypeError);
            assert.areEqual("valueOf,toString", log.join(","));

            var detached = new ArrayBuffer(1);
            ArrayBuffer.prototype.transfer.call(detached);
            assert.throws(function () { detached.transfer(); }, TypeError);
            assert.throws(function () { ArrayBuffer.prototype.transfer.call({}); }, TypeError);
            assert.throws(function () { ArrayBuffer.prototype.transfer.call("ArrayBuffer"); }, TypeError);
        }
    },
    {
        name: "ArrayBuffer.prototype.transferToFixedLength always returns a fixed buffer",
        body() {
            var rab = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(rab, [1, 2, 3, 4]);
            var dest = rab.transferToFixedLength(6);
            assert.areEqual(0, rab.byteLength);
            assert.throws(function () { rab.slice(); }, TypeError);
            assert.isFalse(dest.resizable);
            assert.areEqual(6, dest.byteLength);
            assert.areEqual(6, dest.maxByteLength);
            assertBytes(dest, [1, 2, 3, 4, 0, 0], "transferToFixedLength grow");

            var same = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(same, [1, 2, 3, 4]);
            var sameDest = same.transferToFixedLength();
            assert.isFalse(sameDest.resizable);
            assert.areEqual(4, sameDest.byteLength);
            assert.areEqual(4, sameDest.maxByteLength);
            assertBytes(sameDest, [1, 2, 3, 4], "transferToFixedLength default");

            var smaller = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(smaller, [1, 2, 3, 4]);
            var smallerDest = smaller.transferToFixedLength(2);
            assert.isFalse(smallerDest.resizable);
            assertBytes(smallerDest, [1, 2], "transferToFixedLength shrink");

            var largerThanMax = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(largerThanMax, [1, 2, 3, 4]);
            var largerDest = largerThanMax.transferToFixedLength(10);
            assert.areEqual(10, largerDest.byteLength);
            assertBytes(largerDest, [1, 2, 3, 4, 0, 0, 0, 0, 0, 0], "transferToFixedLength beyond max");

            var overflow = new ArrayBuffer(4);
            assert.throws(function () { overflow.transferToFixedLength(-1); }, RangeError);
            assert.areEqual(4, overflow.byteLength);
            assert.throws(function () { ArrayBuffer.prototype.transferToFixedLength.call({}); }, TypeError);
            var detached = new ArrayBuffer(1);
            detached.transferToFixedLength();
            assert.throws(function () { detached.transferToFixedLength(); }, TypeError);
        }
    },
    {
        name: "Existing fixed-length ArrayBuffer, DataView and TypedArray behavior",
        body() {
            var buffer = new ArrayBuffer(8);
            var view = new Uint8Array(buffer);
            view[0] = 42;
            assert.areEqual(8, buffer.byteLength);
            assert.areEqual(8, buffer.maxByteLength);
            assert.isFalse(buffer.resizable);
            assert.areEqual(42, view[0]);

            var copy = buffer.slice(0, 4);
            assert.areEqual(4, copy.byteLength);
            assert.isFalse(copy.resizable);
            assert.areEqual(42, new Uint8Array(copy)[0]);

            var dv = new DataView(buffer, 1, 4);
            dv.setUint8(0, 7);
            assert.areEqual(7, dv.getUint8(0));
            assert.areEqual(1, dv.byteOffset);
            assert.areEqual(4, dv.byteLength);

            var rab = new ArrayBuffer(8, { maxByteLength: 16 });
            var dv2 = new DataView(rab);
            assert.areEqual(8, dv2.byteLength);
            rab.resize(16);
            assert.areEqual(0, dv2.getUint8(0), "DataView readable after grow");
            rab.resize(4);
            assert.throws(function () { dv2.getUint8(0); }, TypeError, "DataView is out of bounds after shrink");
            assert.throws(function () { dv2.setUint8(0, 1); }, TypeError, "DataView write is out of bounds after shrink");

            var rab2 = new ArrayBuffer(4, { maxByteLength: 8 });
            var fixedView = new Uint8Array(rab2, 0, 4);
            fixedView[0] = 9;
            rab2.resize(8);
            assert.areEqual(4, fixedView.length, "TypedArray length remains fixed (residual)");
            assert.areEqual(9, fixedView[0]);
        }
    },
    {
        name: "Resize, transfer and transferToFixedLength descriptors",
        body() {
            ["resize", "transfer", "transferToFixedLength"].forEach(function (name) {
                var descriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, name);
                assert.isTrue(descriptor.writable, name + " writable");
                assert.isFalse(descriptor.enumerable, name + " enumerable");
                assert.isTrue(descriptor.configurable, name + " configurable");
                assert.strictEqual(name, ArrayBuffer.prototype[name].name);
                assert.strictEqual(name === "resize" ? 1 : 0, ArrayBuffer.prototype[name].length);
                assert.isTrue(Object.isExtensible(ArrayBuffer.prototype[name]));
                assert.throws(function () { new ArrayBuffer.prototype[name](); }, TypeError);
            });
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
