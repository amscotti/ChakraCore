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

function expectThrowsValue(testFunction, expected, message) {
    var caught = {};
    try {
        testFunction();
    } catch (e) {
        caught = e;
    }
    assert.strictEqual(expected, caught, message);
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
        name: "transfer coercion order and failed-transfer data integrity",
        body() {
            function makeDetached() {
                var buffer = new ArrayBuffer(1);
                buffer.transfer();
                return buffer;
            }

            var thrown = new Error("coercion");
            var detached = makeDetached();
            expectThrowsValue(function () {
                detached.transfer({ valueOf: function () { throw thrown; } });
            }, thrown, "transfer propagates newLength coercion errors before the detached check");

            detached = makeDetached();
            expectThrowsValue(function () {
                detached.transferToFixedLength({ valueOf: function () { throw thrown; } });
            }, thrown, "transferToFixedLength propagates newLength coercion errors before the detached check");

            ["transfer", "transferToFixedLength"].forEach(function (name) {
                var log = [];
                var buffer = makeDetached();
                assert.throws(function () {
                    buffer[name]({ valueOf: function () { log.push(name + ":valueOf"); return 1; } });
                }, TypeError, name + " checks detachment after coercion");
                assert.areEqual(name + ":valueOf", log.join(","), name + " coercion ran while detached");

                assert.throws(function () { makeDetached()[name](); }, TypeError, name + " no-argument detached");
                assert.throws(function () { makeDetached()[name](-1); }, RangeError, name + " detached negative length is a ToIndex RangeError");
                assert.throws(function () { makeDetached()[name](Math.pow(2, 53)); }, RangeError, name + " detached 2^53 is a ToIndex RangeError");
                assert.throws(function () { makeDetached()[name](0x7FFFFFFF); }, TypeError, name + " detached engine-limit length remains detached TypeError");
                assert.throws(function () { makeDetached()[name](Math.pow(2, 32)); }, TypeError, name + " detached over-engine-limit length remains detached TypeError");

                var caught;
                try {
                    makeDetached()[name]({ valueOf: function () { return 1; } });
                } catch (e) {
                    caught = e;
                }
                assert.isTrue(caught instanceof TypeError, name + " detached TypeError type");
                assert.isTrue(String(caught.message).indexOf(name) >= 0, name + " detached error names the method");

                log = [];
                buffer = makeDetached();
                assert.throws(function () {
                    buffer[name]({ valueOf: function () { log.push(name + ":valueOf"); return -1; } });
                }, RangeError);
                assert.areEqual(name + ":valueOf", log.join(","), name + " negative coercion still runs");

                assert.throws(function () { var b = new ArrayBuffer(4); b[name](-1); }, RangeError, name + " attached negative");
                assert.throws(function () { var b = new ArrayBuffer(4); b[name](Math.pow(2, 53)); }, RangeError, name + " attached 2^53");
                assert.throws(function () { var b = new ArrayBuffer(4); b[name](Math.pow(2, 32)); }, RangeError, name + " attached engine-limit");

                var nanSource = new ArrayBuffer(4);
                fill(nanSource, [1, 2, 3, 4]);
                var nanDest = nanSource[name](NaN);
                assert.areEqual(0, nanDest.byteLength, name + " NaN coerces to zero");
                assert.areEqual(0, nanSource.byteLength, name + " NaN transfer detaches source");

                var fractionalSource = new ArrayBuffer(4);
                fill(fractionalSource, [1, 2, 3, 4]);
                var fractionalDest = fractionalSource[name](2.9);
                assert.areEqual(2, fractionalDest.byteLength, name + " fractional length truncates");
                assertBytes(fractionalDest, [1, 2], name + " fractional transfer data");
            });

            var source = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(source, [1, 2, 3, 4]);
            assert.throws(function () { source.transfer(9); }, RangeError);
            assert.areEqual(4, source.byteLength, "failed transfer length");
            assertBytes(source, [1, 2, 3, 4], "failed transfer preserves source data");
            assert.throws(function () { source.transferToFixedLength(-1); }, RangeError);
            assert.areEqual(4, source.byteLength, "failed transferToFixedLength length");
            assertBytes(source, [1, 2, 3, 4], "failed transferToFixedLength preserves source data");
            assert.throws(function () { source.transfer(Symbol("length")); }, TypeError);
            assertBytes(source, [1, 2, 3, 4], "failed symbol-coercion transfer preserves source data");
        }
    },
    {
        name: "resizable slice revalidates the current source length",
        body() {
            var source = new ArrayBuffer(8, { maxByteLength: 16 });
            fill(source, [0, 1, 2, 3, 4, 5, 6, 7]);
            var dest = source.slice({ valueOf: function () { source.resize(4); return 2; } }, 6);
            assert.areEqual(4, dest.byteLength);
            assertBytes(dest, [2, 3, 0, 0], "slice start coercion shrink");

            source = new ArrayBuffer(8, { maxByteLength: 16 });
            fill(source, [0, 1, 2, 3, 4, 5, 6, 7]);
            dest = source.slice(1, { valueOf: function () { source.resize(3); return 6; } });
            assert.areEqual(5, dest.byteLength);
            assertBytes(dest, [1, 2, 0, 0, 0], "slice end coercion shrink");

            source = new ArrayBuffer(8, { maxByteLength: 16 });
            fill(source, [0, 1, 2, 3, 4, 5, 6, 7]);
            function Species(length) {
                source.resize(3);
                return new ArrayBuffer(length);
            }
            Species[Symbol.species] = Species;
            source.constructor = Species;
            dest = source.slice(2, 7);
            assert.areEqual(5, dest.byteLength);
            assertBytes(dest, [2, 0, 0, 0, 0], "slice species shrink");

            source = new ArrayBuffer(8, { maxByteLength: 16 });
            fill(source, [0, 1, 2, 3, 4, 5, 6, 7]);
            function PrefilledSpecies(length) {
                var buffer = new ArrayBuffer(length);
                new Uint8Array(buffer).fill(0xAA);
                return buffer;
            }
            PrefilledSpecies[Symbol.species] = PrefilledSpecies;
            source.constructor = PrefilledSpecies;
            dest = source.slice({ valueOf: function () { source.resize(3); return 2; } }, 7);
            assert.areEqual(5, dest.byteLength);
            assertBytes(dest, [2, 0xAA, 0xAA, 0xAA, 0xAA], "slice leaves species destination bytes beyond copy intact");

            source = new ArrayBuffer(8, { maxByteLength: 16 });
            fill(source, [0, 1, 2, 3, 4, 5, 6, 7]);
            dest = source.slice();
            assertBytes(dest, [0, 1, 2, 3, 4, 5, 6, 7], "slice fixed full copy");
        }
    },
    {
        name: "DataView revalidation and auto-length tracking",
        body() {
            var buffer = new ArrayBuffer(8, { maxByteLength: 16 });
            assert.throws(function () {
                new DataView(buffer, 2, { valueOf: function () { buffer.resize(4); return 4; } });
            }, RangeError, "DataView constructor revalidates after byteLength coercion");
            assert.areEqual(4, buffer.byteLength, "constructor did not detach/resize the buffer");

            buffer = new ArrayBuffer(8, { maxByteLength: 16 });
            assert.throws(function () {
                new DataView(buffer, { valueOf: function () { buffer.resize(4); return 6; } });
            }, RangeError, "DataView constructor revalidates offset after coercion");

            buffer = new ArrayBuffer(4, { maxByteLength: 5 });
            var auto = new DataView(buffer, 1);
            assert.areEqual(1, auto.byteOffset);
            assert.areEqual(3, auto.byteLength);
            buffer.resize(5);
            assert.areEqual(4, auto.byteLength, "auto DataView byteLength grows");
            buffer.resize(3);
            assert.areEqual(2, auto.byteLength, "auto DataView byteLength shrinks");
            buffer.resize(1);
            assert.areEqual(0, auto.byteLength, "auto DataView boundary");
            assert.areEqual(1, auto.byteOffset, "auto DataView byteOffset boundary");
            buffer.resize(0);
            assert.throws(function () { auto.byteLength; }, TypeError, "auto DataView byteLength OOB");
            assert.throws(function () { auto.byteOffset; }, TypeError, "auto DataView byteOffset OOB");
            assert.throws(function () { auto.getUint8(0); }, TypeError, "auto DataView access OOB");

            buffer = new ArrayBuffer(4, { maxByteLength: 5 });
            var fixed = new DataView(buffer, 1, 2);
            buffer.resize(3);
            assert.areEqual(2, fixed.byteLength, "fixed DataView fits after shrink");
            assert.areEqual(1, fixed.byteOffset);
            buffer.resize(2);
            assert.throws(function () { fixed.byteLength; }, TypeError, "fixed DataView byteLength OOB");
            assert.throws(function () { fixed.byteOffset; }, TypeError, "fixed DataView byteOffset OOB");
            assert.throws(function () { fixed.getUint8(0); }, TypeError, "fixed DataView access OOB");

            function secondReadTarget(action) {
                var reads = 0;
                var target = function () { }.bind(null);
                Object.defineProperty(target, "prototype", {
                    get: function () {
                        reads++;
                        if (reads === 1) {
                            return {};
                        }
                        return action();
                    }
                });
                return target;
            }

            buffer = new ArrayBuffer(8, { maxByteLength: 16 });
            var detachOnSecondRead = secondReadTarget(function () {
                buffer.transfer();
                return {};
            });
            assert.throws(function () {
                Reflect.construct(DataView, [buffer, 0, 4], detachOnSecondRead);
            }, TypeError, "DataView revalidates detach from the final prototype read");

            buffer = new ArrayBuffer(8, { maxByteLength: 16 });
            var shrinkOnSecondRead = secondReadTarget(function () {
                buffer.resize(2);
                return {};
            });
            assert.throws(function () {
                Reflect.construct(DataView, [buffer, 0, 4], shrinkOnSecondRead);
            }, RangeError, "DataView revalidates shrink from the final prototype read");

            buffer = new ArrayBuffer(1);
            var detached = new DataView(buffer);
            buffer.transfer();
            assert.throws(function () { detached.byteLength; }, TypeError, "detached DataView byteLength");
            assert.throws(function () { detached.byteOffset; }, TypeError, "detached DataView byteOffset");
        }
    },
    {
        name: "max-0 buffers, source views and GC during transfer",
        body() {
            var zero = new ArrayBuffer(0, { maxByteLength: 0 });
            assert.isTrue(zero.resizable);
            assert.areEqual(0, zero.maxByteLength);
            assert.isUndefined(zero.resize(0));
            assert.throws(function () { zero.resize(1); }, RangeError);
            var zeroDest = zero.transfer();
            assert.isTrue(zeroDest.resizable);
            assert.areEqual(0, zeroDest.byteLength);
            assert.areEqual(0, zeroDest.maxByteLength);
            assert.isTrue(zero.detached);

            var zeroFixedSource = new ArrayBuffer(0, { maxByteLength: 0 });
            var zeroFixed = zeroFixedSource.transferToFixedLength();
            assert.isFalse(zeroFixed.resizable);
            assert.areEqual(0, zeroFixed.byteLength);
            assert.areEqual(0, zeroFixed.maxByteLength);
            assert.isTrue(zeroFixedSource.detached);

            var source = new ArrayBuffer(4, { maxByteLength: 8 });
            fill(source, [1, 2, 3, 4]);
            var typed = new Uint8Array(source);
            var view = new DataView(source, 0, 4);
            if (typeof CollectGarbage === "function") {
                CollectGarbage();
            }
            var dest = source.transfer(6);
            if (typeof CollectGarbage === "function") {
                CollectGarbage();
            }
            assert.isTrue(source.detached);
            assert.areEqual(0, typed.length, "typed array length cleared on transfer detach");
            assert.areEqual(0, typed.byteLength);
            assert.throws(function () { view.byteLength; }, TypeError, "DataView detached after transfer");
            assert.throws(function () { view.getUint8(0); }, TypeError, "DataView access detached after transfer");
            assertBytes(dest, [1, 2, 3, 4, 0, 0], "source views did not disturb transfer copy");
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
            assert.areEqual(16, dv2.byteLength, "auto DataView tracks grow");
            assert.areEqual(0, dv2.getUint8(0), "DataView readable after grow");
            rab.resize(4);
            assert.areEqual(4, dv2.byteLength, "auto DataView tracks shrink");
            assert.areEqual(0, dv2.getUint8(0), "DataView readable after in-bounds shrink");
            rab.resize(0);
            assert.areEqual(0, dv2.byteLength, "auto DataView zero-length at zero");
            assert.areEqual(0, dv2.byteOffset, "auto DataView offset remains zero");
            assert.throws(function () { dv2.getUint8(0); }, RangeError, "auto DataView read beyond zero length");
            assert.throws(function () { dv2.setUint8(0, 1); }, RangeError, "auto DataView write beyond zero length");

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
