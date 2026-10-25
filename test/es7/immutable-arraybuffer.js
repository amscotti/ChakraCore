//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Bounded immutable ArrayBuffer proposal surface (doc/c2-s8-immutable.md):
// ArrayBuffer.prototype.immutable, transferToImmutable, sliceToImmutable,
// the write protections on views, and the slice/species interactions.
// Atomics coverage is conditional because ESSharedArrayBuffer is off by
// default in this gate profile.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function assertBytes(arrayBuffer, expected, message) {
    var actual = new Uint8Array(arrayBuffer);
    assert.areEqual(expected.length, actual.length, (message || "bytes") + ": length");
    for (var i = 0; i < expected.length; i++) {
        assert.areEqual(expected[i], actual[i], (message || "bytes") + "[" + i + "]");
    }
}

function makeArrayBuffer(values) {
    var buffer = new ArrayBuffer(values.length);
    var view = new Uint8Array(buffer);
    for (var i = 0; i < values.length; i++) {
        view[i] = values[i];
    }
    return buffer;
}

function makeImmutable(values) {
    return makeArrayBuffer(values).transferToImmutable();
}

var tests = [
    {
        name: "Immutable property and method descriptors",
        body() {
            var descriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "immutable");
            assert.isTrue(typeof descriptor.get === "function", "immutable getter exists");
            assert.strictEqual(undefined, descriptor.set, "immutable setter is undefined");
            assert.isFalse(descriptor.enumerable, "immutable enumerable");
            assert.isTrue(descriptor.configurable, "immutable configurable");
            assert.strictEqual("get immutable", descriptor.get.name, "immutable getter name");
            assert.strictEqual(0, descriptor.get.length, "immutable getter length");

            ["sliceToImmutable", "transferToImmutable"].forEach(function (name) {
                var methodDescriptor = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, name);
                assert.isTrue(typeof methodDescriptor.value === "function", name + " exists");
                assert.isTrue(methodDescriptor.writable, name + " writable");
                assert.isFalse(methodDescriptor.enumerable, name + " enumerable");
                assert.isTrue(methodDescriptor.configurable, name + " configurable");
                assert.strictEqual(name, methodDescriptor.value.name, name + " name");
                assert.strictEqual(name === "sliceToImmutable" ? 2 : 0, methodDescriptor.value.length, name + " length");
                assert.throws(function () { new ArrayBuffer.prototype[name](); }, TypeError, name + " is not a constructor");
            });
        }
    },
    {
        name: "immutable getter receiver and slot checks",
        body() {
            var getter = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "immutable").get;
            [undefined, null, 0, 1, "1", true, false, {}, [], function () {}].forEach(function (receiver) {
                assert.throws(function () { getter.call(receiver); }, TypeError);
            });
            assert.throws(function () { ArrayBuffer.prototype.immutable; }, TypeError);
            assert.throws(function () { getter(); }, TypeError);
            assert.throws(function () { getter.call(new Uint8Array(1)); }, TypeError);
            assert.throws(function () { getter.call(new DataView(new ArrayBuffer(1))); }, TypeError);

            var ordinary = new ArrayBuffer(2);
            assert.isFalse(ordinary.immutable, "ordinary ArrayBuffer is mutable");
            var immutable = ordinary.transferToImmutable();
            assert.isTrue(immutable.immutable, "transferred ArrayBuffer is immutable");
            assert.isFalse(ordinary.immutable, "transferred source is an ordinary detached ArrayBuffer");

            var detached = new ArrayBuffer(2);
            detached.transfer();
            assert.isFalse(detached.immutable, "detached ordinary ArrayBuffer is mutable");

            if (typeof SharedArrayBuffer !== "undefined") {
                assert.throws(function () { getter.call(new SharedArrayBuffer(4)); }, TypeError, "SharedArrayBuffer is rejected");
            }
        }
    },
    {
        name: "transferToImmutable copies, detaches and ignores species",
        body() {
            var source = makeArrayBuffer([1, 2, 3, 4]);
            var immutable = source.transferToImmutable();
            assert.isTrue(source.detached, "source is detached");
            assert.isTrue(immutable.immutable, "result is immutable");
            assert.isFalse(immutable.resizable, "result is not resizable");
            assert.areEqual(4, immutable.byteLength);
            assert.areEqual(4, immutable.maxByteLength);
            assertBytes(immutable, [1, 2, 3, 4]);
            assert.areEqual(1, new Uint8Array(immutable)[0], "reads remain available");

            source = makeArrayBuffer([1, 2, 3, 4]);
            immutable = source.transferToImmutable(6);
            assert.isTrue(source.detached, "larger transfer detaches source");
            assertBytes(immutable, [1, 2, 3, 4, 0, 0], "larger transfer zero-fills");

            source = makeArrayBuffer([1, 2, 3, 4]);
            immutable = source.transferToImmutable(2);
            assert.isTrue(source.detached, "smaller transfer detaches source");
            assertBytes(immutable, [1, 2], "smaller transfer copies the prefix");

            source = new ArrayBuffer(4, { maxByteLength: 8 });
            new Uint8Array(source).set([5, 6, 7, 8]);
            immutable = source.transferToImmutable();
            assert.isTrue(immutable.immutable, "resizable source produces immutable result");
            assert.isFalse(immutable.resizable, "resizable source produces fixed result");
            assert.areEqual(4, immutable.maxByteLength);
            assertBytes(immutable, [5, 6, 7, 8]);

            var zero = (new ArrayBuffer(0)).transferToImmutable();
            assert.isTrue(zero.immutable, "zero-length result is immutable");
            assert.areEqual(0, zero.byteLength);

            var noSpecies = makeArrayBuffer([9]);
            Object.defineProperty(noSpecies, "constructor", {
                get() { throw new Error("constructor must not be read"); }
            });
            assertBytes(noSpecies.transferToImmutable(), [9], "transferToImmutable ignores species");

            var detached = new ArrayBuffer(2);
            detached.transfer();
            assert.throws(function () { detached.transferToImmutable(); }, TypeError, "detached receiver");

            var immutableReceiver = makeImmutable([1]);
            assert.throws(function () { immutableReceiver.transferToImmutable(); }, TypeError, "immutable receiver");
        }
    },
    {
        name: "transferToImmutable coercion order and errors",
        body() {
            var calls = [];
            var source = makeArrayBuffer([1, 2, 3, 4]);
            var result = source.transferToImmutable({
                valueOf() {
                    calls.push("valueOf");
                    return "2";
                }
            });
            assert.areEqual(1, calls.length);
            assert.areEqual("valueOf", calls[0]);
            assertBytes(result, [1, 2]);

            source = makeArrayBuffer([1]);
            assert.throws(function () { source.transferToImmutable(-1); }, RangeError);
            assert.isFalse(source.detached, "negative length leaves source attached");
            assert.throws(function () { source.transferToImmutable(Infinity); }, RangeError);
            assert.isFalse(source.detached, "infinite length leaves source attached");

            var detachDuringCoercion = makeArrayBuffer([1]);
            calls = [];
            assert.throws(function () {
                detachDuringCoercion.transferToImmutable({
                    valueOf() {
                        calls.push("valueOf");
                        detachDuringCoercion.transfer();
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(1, calls.length, "newLength is read before the detached check");

            var immutable = makeImmutable([1, 2]);
            assert.throws(function () { immutable.transfer(); }, TypeError, "transfer rejects immutable");
            assert.throws(function () { immutable.transferToFixedLength(); }, TypeError, "transferToFixedLength rejects immutable");

            calls = [];
            assert.throws(function () {
                immutable.transfer({
                    valueOf() {
                        calls.push("transfer.valueOf");
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(1, calls.length, "transfer reads newLength before mutability check");

            calls = [];
            assert.throws(function () {
                immutable.transferToFixedLength({
                    valueOf() {
                        calls.push("transferToFixedLength.valueOf");
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(1, calls.length, "transferToFixedLength reads newLength before mutability check");

            calls = [];
            assert.throws(function () {
                immutable.resize({
                    valueOf() {
                        calls.push("resize.valueOf");
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(0, calls.length, "resize verifies the slot before reading newLength");
        }
    },
    {
        name: "sliceToImmutable bounds, copies and ignores species",
        body() {
            var source = makeArrayBuffer([1, 2, 3, 4, 5, 6, 7, 8]);
            var immutable = source.sliceToImmutable();
            assert.isFalse(source.detached, "sliceToImmutable leaves the source attached");
            assert.isTrue(immutable.immutable, "sliceToImmutable result is immutable");
            assert.isFalse(immutable.resizable, "sliceToImmutable result is fixed");
            assertBytes(immutable, [1, 2, 3, 4, 5, 6, 7, 8]);
            assertBytes(source, [1, 2, 3, 4, 5, 6, 7, 8], "source unchanged");

            assertBytes(source.sliceToImmutable(2, 5), [3, 4, 5]);
            assertBytes(source.sliceToImmutable(-3), [6, 7, 8]);
            assertBytes(source.sliceToImmutable(0, -2), [1, 2, 3, 4, 5, 6]);
            assertBytes(source.sliceToImmutable(20, 30), []);
            assertBytes(source.sliceToImmutable(5, 2), []);
            assertBytes(source.sliceToImmutable(undefined, 3), [1, 2, 3]);

            immutable = source.sliceToImmutable(1, 3);
            new Uint8Array(source)[0] = 99;
            new Uint8Array(source)[1] = 88;
            assertBytes(immutable, [2, 3], "destination is unaffected by later writes");

            var noSpecies = makeArrayBuffer([7, 8]);
            Object.defineProperty(noSpecies, "constructor", {
                get() { throw new Error("constructor must not be read"); }
            });
            assertBytes(noSpecies.sliceToImmutable(), [7, 8], "sliceToImmutable ignores species");

            var detached = new ArrayBuffer(4);
            detached.transfer();
            var calls = [];
            assert.throws(function () {
                detached.sliceToImmutable({
                    valueOf() {
                        calls.push("start");
                        return 0;
                    }
                }, {
                    valueOf() {
                        calls.push("end");
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(0, calls.length, "detached receiver is rejected before bounds coercion");

            var immutableReceiver = makeImmutable([1, 2, 3]);
            assert.isTrue(immutableReceiver.sliceToImmutable(1).immutable, "sliceToImmutable accepts an immutable receiver");
            assertBytes(immutableReceiver.sliceToImmutable(1), [2, 3]);
        }
    },
    {
        name: "sliceToImmutable observes source resizing and coercion order",
        body() {
            var source = new ArrayBuffer(10, { maxByteLength: 12 });
            new Uint8Array(source).set([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
            var immutable = source.sliceToImmutable({
                valueOf() {
                    source.resize(11);
                    return -7;
                }
            }, {
                valueOf() {
                    source.resize(12);
                    return -4;
                }
            });
            assertBytes(immutable, [4, 5, 6], "bounds come from the original length");
            assert.areEqual(12, source.byteLength);

            var calls = [];
            var shrinking = new ArrayBuffer(10, { maxByteLength: 10 });
            new Uint8Array(shrinking).set([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
            assert.throws(function () {
                shrinking.sliceToImmutable({
                    valueOf() {
                        calls.push("start");
                        shrinking.resize(5);
                        return 0;
                    }
                }, {
                    valueOf() {
                        calls.push("end");
                        return 8;
                    }
                });
            }, RangeError, "end beyond the shrunk length is a RangeError");
            assert.areEqual(2, calls.length);
            assert.areEqual(["start", "end"].join(","), calls.join(","));

            var detachedDuringBounds = new ArrayBuffer(8, { maxByteLength: 8 });
            calls = [];
            assert.throws(function () {
                detachedDuringBounds.sliceToImmutable({
                    valueOf() {
                        calls.push("start");
                        return 0;
                    }
                }, {
                    valueOf() {
                        calls.push("end");
                        detachedDuringBounds.transfer();
                        return 1;
                    }
                });
            }, TypeError);
            assert.areEqual(2, calls.length, "both bounds are coerced before the detached check");
        }
    },
    {
        name: "ArrayBuffer slice species and immutable sources",
        body() {
            var source = makeImmutable([1, 2, 3, 4]);
            var slice = source.slice(1, 3);
            assert.isFalse(slice.immutable, "default slice result is mutable");
            assertBytes(slice, [2, 3], "slice copies from an immutable source");
            new Uint8Array(slice)[0] = 9;
            assert.areEqual(9, new Uint8Array(slice)[0]);
            assert.areEqual(2, new Uint8Array(source)[1], "immutable source is unchanged");

            var immutableDestination = makeImmutable([0, 0, 0, 0]);
            var calls = [];
            var target = new ArrayBuffer(4);
            target.constructor = {
                [Symbol.species]: function (length) {
                    calls.push("species:" + length);
                    return immutableDestination;
                }
            };
            assert.throws(function () { target.slice(1, 3); }, TypeError, "immutable species result is rejected");
            assert.areEqual("species:2", calls.join(","), "species result is checked before copying");

            calls = [];
            assert.throws(function () {
                target.slice({
                    valueOf() {
                        calls.push("start");
                        return 0;
                    }
                }, {
                    valueOf() {
                        calls.push("end");
                        return 2;
                    }
                });
            }, TypeError);
            assert.areEqual("start,end,species:2", calls.join(","), "bounds are read before species");
        }
    },
    {
        name: "DataView writes are rejected and reads stay available",
        body() {
            var immutable = makeImmutable([1, 2, 3, 4, 5, 6, 7, 8]);
            var view = new DataView(immutable);
            assert.areEqual(1, view.getUint8(0), "DataView read");
            assert.areEqual(0x0201, view.getUint16(0, true), "DataView multi-byte read");

            var calls = [];
            var offset = {
                valueOf() {
                    calls.push("offset");
                    return 0;
                }
            };
            var value = {
                valueOf() {
                    calls.push("value");
                    return 9;
                }
            };
            ["setUint8", "setInt32", "setFloat64", "setFloat16"].forEach(function (name) {
                calls = [];
                assert.throws(function () { view[name](offset, value); }, TypeError, name + " rejects immutable");
                assert.areEqual(0, calls.length, name + " checks mutability before arguments");
            });
            assertBytes(immutable, [1, 2, 3, 4, 5, 6, 7, 8], "DataView writes did not modify bytes");
        }
    },
    {
        name: "TypedArray mutating methods and element writes are rejected",
        body() {
            var immutable = makeImmutable([1, 2, 3, 4]);
            var ta = new Uint8Array(immutable);
            assertBytes(ta.buffer, [1, 2, 3, 4]);
            assert.areEqual(4, ta.length, "TypedArray view reads length");
            assert.areEqual(1, ta[0], "TypedArray element read");

            var genericComparisons = 0;
            assert.throws(function () { Array.prototype.fill.call(ta, 9); }, TypeError, "Array.prototype.fill rejects immutable");
            assert.throws(function () { Array.prototype.copyWithin.call(ta, 0, 1); }, TypeError, "Array.prototype.copyWithin rejects immutable");
            assert.throws(function () {
                Array.prototype.sort.call(ta, function () {
                    genericComparisons++;
                    return 0;
                });
            }, TypeError, "Array.prototype.sort rejects immutable");
            assert.isTrue(genericComparisons > 0, "generic sort compares before its write-back fails");
            assert.doesNotThrow(function () { Array.prototype.fill.call(ta, 9, 5, 5); }, "zero-length generic fill is a no-op");
            assert.doesNotThrow(function () { Array.prototype.copyWithin.call(ta, 0, 0, 0); }, "zero-count generic copyWithin is a no-op");
            assertBytes(immutable, [1, 2, 3, 4], "generic Array mutators did not modify bytes");

            var calls = [];
            assert.throws(function () { ta.fill(9, { valueOf() { calls.push("fill"); return 0; } }); }, TypeError);
            assert.throws(function () { ta.copyWithin(0, 1); }, TypeError);
            assert.throws(function () { ta.reverse(); }, TypeError);
            assert.throws(function () { ta.sort(function () { calls.push("compare"); return 0; }); }, TypeError);
            assert.throws(function () { ta.set([9], { valueOf() { calls.push("offset"); return 0; } }); }, TypeError);
            assert.throws(function () { ta.set([9]); }, TypeError);
            assert.areEqual(0, calls.length, "mutators reject before argument coercion or callbacks");
            assertBytes(immutable, [1, 2, 3, 4], "TypedArray mutations did not modify bytes");

            assert.throws(function () {
                "use strict";
                ta[0] = 9;
            }, TypeError, "strict element assignment throws");
            ta[1] = 9;
            assert.areEqual(2, ta[1], "sloppy element assignment is a silent no-op");
            assert.isFalse(Reflect.set(ta, 0, 9), "Reflect.set reports failure");
            ta.custom = 1;
            assert.areEqual(1, ta.custom, "non-index properties remain writable");

            var descriptor = Object.getOwnPropertyDescriptor(ta, "0");
            assert.isFalse(descriptor.writable, "index descriptor is not writable");
            assert.isFalse(descriptor.configurable, "index descriptor is not configurable");
            assert.isTrue(Reflect.defineProperty(ta, "0", {}), "empty descriptor is a no-op");
            assert.isTrue(Reflect.defineProperty(ta, "0", { value: 1 }), "same-value descriptor is a no-op");
            assert.isTrue(Reflect.defineProperty(ta, "0", { writable: false }), "writable:false is a no-op");
            assert.isTrue(Reflect.defineProperty(ta, "0", { configurable: false }), "configurable:false is a no-op");
            assert.isTrue(Reflect.defineProperty(ta, "0", { value: 1, writable: false, configurable: false, enumerable: true }),
                "full same-value descriptor is a no-op");
            assert.isFalse(Reflect.defineProperty(ta, "0", { value: 9 }), "different value is rejected");
            assert.isFalse(Reflect.defineProperty(ta, "0", { writable: true }), "writable:true is rejected");
            assert.isFalse(Reflect.defineProperty(ta, "0", { configurable: true }), "configurable:true is rejected");
            assert.isFalse(Reflect.defineProperty(ta, "0", { enumerable: false }), "enumerable:false is rejected");
            assert.throws(function () { Object.defineProperty(ta, "0", { value: 9 }); }, TypeError, "defineProperty rejects writes");

            var mutableTarget = new Uint8Array(4);
            mutableTarget.set(new Uint8Array(immutable));
            assertBytes(mutableTarget.buffer, [1, 2, 3, 4], "immutable view is a valid read source for set");
            var copy = new Uint8Array(ta);
            assertBytes(copy.buffer, [1, 2, 3, 4], "TypedArray constructor copies from an immutable view");
            assertBytes(ta.slice(1), [2, 3, 4], "slice reads an immutable view");
            assertBytes(ta.subarray(2), [3, 4], "subarray reads an immutable view");
        }
    },
    {
        name: "TypedArray species results backed by immutable buffers are rejected",
        body() {
            var immutable = makeImmutable([1, 2]);
            var ta = new Uint8Array([1, 2]);
            var calls = [];

            function installSpecies(species) {
                var constructor = {};
                Object.defineProperty(constructor, Symbol.species, {
                    get() {
                        calls.push("species");
                        return species;
                    }
                });
                Object.defineProperty(ta, "constructor", {
                    get() {
                        calls.push("constructor");
                        return constructor;
                    }
                });
            }
            installSpecies(function () { return new Uint8Array(immutable); });

            assert.throws(function () {
                ta.map(function (value, index) {
                    calls.push("map " + index);
                    return value;
                });
            }, TypeError, "map rejects an immutable species result");
            assert.areEqual("constructor,species", calls.join(","), "map constructs its result before visiting elements");

            calls = [];
            assert.throws(function () {
                ta.filter(function (value, index) {
                    calls.push("filter " + index);
                    return true;
                });
            }, TypeError, "filter rejects an immutable species result");
            assert.areEqual("filter 0,filter 1,constructor,species", calls.join(","), "filter visits elements before constructing");

            calls = [];
            assert.throws(function () {
                ta.slice({
                    valueOf() {
                        calls.push("start");
                        return 0;
                    }
                }, {
                    valueOf() {
                        calls.push("end");
                        return 1;
                    }
                });
            }, TypeError, "slice rejects an immutable species result");
            assert.areEqual("start,end,constructor,species", calls.join(","), "slice reads bounds before constructing");

            var ctor = function (length) {
                calls.push("construct " + length);
                return new Uint8Array(immutable);
            };
            calls = [];
            assert.throws(function () { Uint8Array.from.call(ctor, [1]); }, TypeError, "from rejects an immutable constructor result");
            assert.areEqual("construct 1", calls.join(","));

            calls = [];
            assert.throws(function () { Uint8Array.of.call(ctor, 1); }, TypeError, "of rejects an immutable constructor result");
            assert.areEqual("construct 1", calls.join(","));
        }
    },
    {
        name: "Atomics reject immutable write targets when available",
        body() {
            if (typeof Atomics === "undefined") {
                return;
            }
            var immutable = (new ArrayBuffer(8)).transferToImmutable();
            var ta = new Int32Array(immutable);
            assert.areEqual(0, Atomics.load(ta, 0), "Atomics load remains available");
            assert.throws(function () { Atomics.add(ta, 0, 1); }, TypeError, "Atomics.add rejects immutable");
            assert.throws(function () { Atomics.store(ta, 0, 1); }, TypeError, "Atomics.store rejects immutable");
            assert.areEqual(0, Atomics.notify(ta, 0, 1), "Atomics.notify on a non-shared buffer returns zero");
        }
    },
    {
        name: "Host serialization cannot externalize or detach an immutable buffer",
        body() {
            if (typeof WScript === "undefined" || typeof WScript.SerializeObject !== "function") {
                return;
            }
            var source = makeImmutable([7, 8]);
            var threw = false;
            try {
                WScript.SerializeObject(source, [source]);
            } catch (e) {
                threw = true;
            }
            assert.isTrue(threw, "SerializeObject transfer list is rejected");
            assert.isFalse(source.detached, "immutable source stays attached");
            assert.isTrue(source.immutable, "immutable source stays immutable");
            assertBytes(source, [7, 8], "immutable source keeps its bytes");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
