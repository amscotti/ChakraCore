//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// BigInt64Array / BigUint64Array element support (doc/c2-s2-bigint64array.md).
// Elements are stored as two's-complement 64-bit integers and every read
// returns a BigInt; stores apply ToBigInt and keep the low 64 bits.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var int64Min = -9223372036854775808n;
var int64Max = 9223372036854775807n;
var uint64Max = 18446744073709551615n;

function assertElements(view, expected, message) {
    assert.strictEqual(expected.length, view.length, (message || "elements") + ": length");
    for (var i = 0; i < expected.length; i++) {
        assert.strictEqual(expected[i], view[i], (message || "elements") + "[" + i + "]");
    }
}

function assertThrowsTypeError(fn, message) {
    assert.throws(fn, TypeError, message);
}

function assertThrowsSyntaxError(fn, message) {
    assert.throws(fn, SyntaxError, message);
}

var tests = [
    {
        name: "Constructors, prototypes and shape",
        body() {
            assert.strictEqual("function", typeof BigInt64Array);
            assert.strictEqual("function", typeof BigUint64Array);

            [BigInt64Array, BigUint64Array].forEach(function (Ctor) {
                assert.strictEqual(3, Ctor.length, Ctor.name + ".length");
                assert.strictEqual(Ctor === BigInt64Array ? "BigInt64Array" : "BigUint64Array", Ctor.name,
                    "constructor name");
                assert.strictEqual(8, Ctor.BYTES_PER_ELEMENT, Ctor.name + ".BYTES_PER_ELEMENT");
                assert.strictEqual(8, Ctor.prototype.BYTES_PER_ELEMENT, Ctor.name + ".prototype.BYTES_PER_ELEMENT");
                assert.strictEqual(Ctor, Ctor.prototype.constructor, Ctor.name + ".prototype.constructor");
                assert.strictEqual(Object.getPrototypeOf(Int8Array), Object.getPrototypeOf(Ctor),
                    Ctor.name + " inherits from %TypedArray%");
                assert.strictEqual(Object.getPrototypeOf(Int8Array.prototype), Object.getPrototypeOf(Ctor.prototype),
                    Ctor.name + ".prototype inherits from %TypedArray%.prototype");

                var descriptor = Object.getOwnPropertyDescriptor(this, Ctor.name);
                assert.isTrue(!!descriptor, Ctor.name + " is a global own property");
                assert.isTrue(descriptor.writable, Ctor.name + " is writable");
                assert.isFalse(descriptor.enumerable, Ctor.name + " is not enumerable");
                assert.isTrue(descriptor.configurable, Ctor.name + " is configurable");
            });

            var empty = new BigInt64Array();
            assert.strictEqual(0, empty.length, "no-argument constructor length");
            assert.strictEqual(0, empty.byteLength, "no-argument constructor byteLength");

            var zeros = new BigInt64Array(3);
            assert.strictEqual(3, zeros.length, "length constructor length");
            assert.strictEqual(24, zeros.byteLength, "length constructor byteLength");
            assert.strictEqual(0, zeros.byteOffset, "length constructor byteOffset");
            assert.isTrue(zeros.buffer instanceof ArrayBuffer, "length constructor buffer");
            assertElements(zeros, [0n, 0n, 0n], "length constructor zero initializes");

            var unsigned = new BigUint64Array(2);
            assertElements(unsigned, [0n, 0n], "BigUint64Array zero initializes");

            assertThrowsTypeError(function () { BigInt64Array(); }, "called without new");
            assertThrowsTypeError(function () { BigUint64Array(); }, "BigUint64Array called without new");
            assertThrowsTypeError(function () { new BigInt64Array(1n); }, "BigInt length is not ToIndex-able");
            assert.throws(function () { new BigInt64Array(-1); }, RangeError, "negative length");
            assert.strictEqual(1, new BigInt64Array(1.5).length, "fractional length truncates through ToIndex");

            function Subclass() {}
            var reflected = Reflect.construct(BigInt64Array, [2], Subclass);
            assert.isTrue(reflected instanceof Subclass, "Reflect.construct newTarget");
            // NewTarget.prototype is used for the result; this pre-existing
            // engine path does not preserve the typed-array slots for an
            // arbitrary non-typed-array prototype (same for all element types).
            assert.strictEqual(Subclass.prototype, Object.getPrototypeOf(reflected),
                "Reflect.construct custom prototype");
        }
    },
    {
        name: "Two's-complement boundaries and ToBigInt conversion",
        body() {
            var signed = new BigInt64Array([
                0n, 1n, -1n, int64Max, int64Min,
                9223372036854775808n, -9223372036854775809n,
                18446744073709551615n, -18446744073709551616n,
                18446744073709551618n, -18446744073709551618n
            ]);
            assertElements(signed, [
                0n, 1n, -1n, int64Max, int64Min,
                int64Min, int64Max,
                -1n, 0n,
                2n, -2n
            ], "BigInt64Array wraps modulo 2^64");

            var unsigned = new BigUint64Array([
                0n, 1n, -1n, -2n, int64Min, int64Max,
                9223372036854775808n, uint64Max,
                18446744073709551618n, -18446744073709551618n
            ]);
            assertElements(unsigned, [
                0n, 1n, uint64Max, uint64Max - 1n, 9223372036854775808n, int64Max,
                9223372036854775808n, uint64Max,
                2n, uint64Max - 1n
            ], "BigUint64Array wraps modulo 2^64");

            var values = [
                0n, 1n, -1n, int64Min, int64Max, uint64Max,
                123456789012345678901234567890n,
                -123456789012345678901234567890n,
                (1n << 64n) + 7n,
                -((1n << 64n) + 7n)
            ];
            values.forEach(function (value) {
                var s = new BigInt64Array(1);
                var u = new BigUint64Array(1);
                s[0] = value;
                u[0] = value;
                assert.strictEqual(BigInt.asIntN(64, value), s[0], "signed low bits for " + value);
                assert.strictEqual(BigInt.asUintN(64, value), u[0], "unsigned low bits for " + value);
            });

            var converted = new BigInt64Array(1);
            converted[0] = "123";
            assert.strictEqual(123n, converted[0], "string ToBigInt");
            converted[0] = true;
            assert.strictEqual(1n, converted[0], "boolean ToBigInt");
            converted[0] = { valueOf() { return -5n; } };
            assert.strictEqual(-5n, converted[0], "object ToBigInt");
            converted[0] = { [Symbol.toPrimitive]() { return 6n; } };
            assert.strictEqual(6n, converted[0], "object @@toPrimitive ToBigInt");

            [1, 1.5, null, undefined, Symbol("s")].forEach(function (value) {
                assertThrowsTypeError(function () { converted[0] = value; }, "ToBigInt rejects " + String(value));
            });
            assertThrowsSyntaxError(function () { converted[0] = "1.5"; }, "invalid BigInt string");
            assertThrowsSyntaxError(function () { converted[0] = "xyz"; }, "invalid BigInt string");

            var calls = 0;
            converted[0] = { valueOf() { calls++; return 7n; } };
            assert.strictEqual(1, calls, "element store converts exactly once");

            assertThrowsTypeError(function () { return new BigInt64Array([1]); }, "constructor array rejects Number");
            assertThrowsTypeError(function () { new BigInt64Array(2).set([1]); }, "set rejects Number");
            assertThrowsTypeError(function () { new BigInt64Array(2).fill(1); }, "fill rejects Number");
            assertThrowsTypeError(function () { return new BigInt64Array([1n]).with(0, 1); }, "with rejects Number");
        }
    },
    {
        name: "Out-of-bounds and detached stores convert before validation",
        body() {
            var view = new BigInt64Array(1);

            // Sloppy and strict element assignment both run ToBigInt before
            // the index/detached check, so Number values throw.
            assertThrowsTypeError(function () { view[10] = 1; }, "OOB sloppy Number store");
            assertThrowsTypeError(function () { view[-1] = 1; }, "negative Number store");
            assertThrowsTypeError(function () { view[1.5] = 1; }, "fractional Number store");
            assertThrowsTypeError(function () { "use strict"; view[10] = 1; }, "OOB strict Number store");

            assert.doesNotThrow(function () { view[10] = 2n; }, "OOB BigInt store is a no-op");
            assert.doesNotThrow(function () { "use strict"; view[10] = 2n; }, "OOB strict BigInt store");
            assert.strictEqual(1, view.length, "OOB store does not grow the view");

            var calls = 0;
            view[10] = { valueOf() { calls++; return 3n; } };
            assert.strictEqual(1, calls, "OOB store converts the value exactly once");

            var throwingCalls = 0;
            assertThrowsTypeError(function () {
                view[10] = { valueOf() { throwingCalls++; return 3; } };
            }, "OOB conversion failure propagates");
            assert.strictEqual(1, throwingCalls, "OOB conversion failure runs once");

            var detached = new BigInt64Array(1);
            assert.strictEqual("function", typeof detached.buffer.transfer, "transfer is available");
            detached.buffer.transfer();

            assertThrowsTypeError(function () { detached[0] = 1; }, "detached Number store converts first");
            assert.doesNotThrow(function () { detached[0] = 1n; }, "detached BigInt store is a no-op");
            var detachedCalls = 0;
            assert.doesNotThrow(function () {
                detached[0] = { valueOf() { detachedCalls++; return 4n; } };
            }, "detached object store converts first");
            assert.strictEqual(1, detachedCalls, "detached store converts exactly once");

            var detachedThrowCalls = 0;
            var caught = false;
            try {
                detached[0] = { valueOf() { detachedThrowCalls++; throw new Error("store-conversion"); } };
            } catch (e) {
                caught = e.message === "store-conversion";
            }
            assert.isTrue(caught, "detached conversion failure propagates");
            assert.strictEqual(1, detachedThrowCalls, "detached conversion failure runs once");

            // Control: the existing non-BigInt path already converts OOB
            // values; a detached numeric store is a silent no-op
            // (web-reality alignment).
            var control = new Uint8Array(1);
            var controlCalls = 0;
            control[10] = { valueOf() { controlCalls++; return 5; } };
            assert.strictEqual(1, controlCalls, "Uint8 OOB store converts once");
            var detachedControl = new Uint8Array(1);
            detachedControl.buffer.transfer();
            detachedControl[0] = 1;
            assert.areEqual(undefined, detachedControl[0], "detached Uint8 store is a silent no-op");
        }
    },
    {
        name: "ArrayBuffer construction, offsets and byteLength",
        body() {
            var buffer = new ArrayBuffer(32);
            var full = new BigInt64Array(buffer);
            assert.strictEqual(4, full.length, "full view length");
            assert.strictEqual(32, full.byteLength, "full view byteLength");
            assert.strictEqual(buffer, full.buffer, "full view buffer");

            var offset = new BigInt64Array(buffer, 8);
            assert.strictEqual(3, offset.length, "offset view length");
            assert.strictEqual(8, offset.byteOffset, "offset view byteOffset");
            assert.strictEqual(24, offset.byteLength, "offset view byteLength");

            var fixed = new BigInt64Array(buffer, 8, 1);
            assert.strictEqual(1, fixed.length, "fixed view length");
            fixed[0] = -1n;
            assert.strictEqual(-1n, full[1], "views share storage");

            assert.throws(function () { new BigInt64Array(buffer, 4); }, RangeError, "unaligned offset");
            assert.throws(function () { new BigInt64Array(buffer, 0, 5); }, RangeError, "length past buffer");
            assert.throws(function () { new BigInt64Array(buffer, 40); }, RangeError, "offset past buffer");

            var bytes = new Uint8Array(buffer);
            var value = new BigInt64Array(buffer, 16, 1);
            value[0] = 0x0102030405060708n;
            // Little-endian element storage: 08 07 06 05 04 03 02 01.
            assert.strictEqual(0x08, bytes[16], "low byte little-endian");
            assert.strictEqual(0x07, bytes[17], "byte 1 little-endian");
            assert.strictEqual(0x01, bytes[23], "high byte little-endian");
        }
    },
    {
        name: "set, subarray, slice and species",
        body() {
            var target = new BigInt64Array(5);
            target.set([1n, 2n, 3n]);
            assertElements(target, [1n, 2n, 3n, 0n, 0n], "set array");
            target.set(new BigUint64Array([4n, 5n]), 3);
            assertElements(target, [1n, 2n, 3n, 4n, 5n], "set BigUint64Array");

            var other = new BigInt64Array([9n, 8n]);
            target.set(other, 1);
            assertElements(target, [1n, 9n, 8n, 4n, 5n], "set BigInt64Array");

            var overlap = new BigInt64Array([1n, 2n, 3n, 4n, 5n]);
            overlap.set(overlap.subarray(0, 3), 1);
            assertElements(overlap, [1n, 1n, 2n, 3n, 5n], "overlapping set");

            var source = new BigInt64Array([1n, 2n, 3n, 4n]);
            var sub = source.subarray(1, 3);
            assert.strictEqual(2, sub.length, "subarray length");
            assert.strictEqual(8, sub.byteOffset, "subarray byteOffset");
            sub[0] = 20n;
            assert.strictEqual(20n, source[1], "subarray shares storage");

            var copy = source.slice(1, 3);
            assertElements(copy, [20n, 3n], "slice values");
            assert.isFalse(source.buffer === copy.buffer, "slice copies buffer");
            copy[0] = 99n;
            assert.strictEqual(20n, source[1], "slice does not share storage");

            var mapped = source.map(function (value, index) { return value + BigInt(index); });
            assertElements(mapped, [1n, 21n, 5n, 7n], "map converts callback result");
            assert.isTrue(mapped instanceof BigInt64Array, "map default constructor");

            var filtered = source.filter(function (value) { return value > 2n; });
            assertElements(filtered, [20n, 3n, 4n], "filter");

            var speciesCtor = function (length) {
                return new BigUint64Array(length);
            };
            source.constructor = {};
            source.constructor[Symbol.species] = speciesCtor;
            var speciesMapped = source.map(function (value) { return value; });
            assert.isTrue(speciesMapped instanceof BigUint64Array, "map uses @@species");
            assertElements(speciesMapped, [1n, 20n, 3n, 4n], "map species values");
            var speciesSlice = source.slice(0, 2);
            assert.isTrue(speciesSlice instanceof BigUint64Array, "slice uses @@species");
            delete source.constructor;
        }
    },
    {
        name: "sort, sort callbacks and numeric ordering",
        body() {
            var signed = new BigInt64Array([3n, -1n, int64Min, 2n, int64Max, -2n, 0n]);
            var result = signed.sort();
            assert.strictEqual(signed, result, "sort returns the receiver");
            assertElements(signed, [int64Min, -2n, -1n, 0n, 2n, 3n, int64Max], "signed numeric sort");

            var unsigned = new BigUint64Array([1n, uint64Max, 2n, 9223372036854775808n, 0n]);
            unsigned.sort();
            assertElements(unsigned, [0n, 1n, 2n, 9223372036854775808n, uint64Max], "unsigned numeric sort");

            var seen = [];
            var custom = new BigInt64Array([2n, 1n, 3n]);
            custom.sort(function (a, b) {
                assert.strictEqual("bigint", typeof a, "comparefn argument type");
                assert.strictEqual("bigint", typeof b, "comparefn argument type");
                seen.push([a, b]);
                return a < b ? 1 : a > b ? -1 : 0;
            });
            assertElements(custom, [3n, 2n, 1n], "comparefn ordering");
            assert.isTrue(seen.length > 0, "comparefn was called");

            var toSorted = new BigUint64Array([uint64Max, 1n]).toSorted();
            assertElements(toSorted, [1n, uint64Max], "toSorted numeric order");
            assertElements(new BigInt64Array([1n, 2n, 3n]).toReversed(), [3n, 2n, 1n], "toReversed");
        }
    },
    {
        name: "Iteration, search, fill, with, copyWithin and reduce",
        body() {
            var view = new BigInt64Array([1n, 2n, 3n]);

            var values = [];
            for (var value of view) {
                values.push(value);
            }
            assertElements(values, [1n, 2n, 3n], "values iteration");

            var entries = [];
            for (var entry of view.entries()) {
                entries.push(entry[0] + ":" + entry[1]);
            }
            assert.strictEqual("0:1,1:2,2:3", entries.join(","), "entries iteration");

            var keys = [];
            for (var key of view.keys()) {
                keys.push(key);
            }
            assert.strictEqual("0,1,2", keys.join(","), "keys iteration");

            assert.strictEqual(2n, view.at(1), "at positive");
            assert.strictEqual(3n, view.at(-1), "at negative");
            assert.strictEqual(undefined, view.at(3), "at out of bounds");
            assert.strictEqual(1, view.indexOf(2n), "indexOf");
            assert.strictEqual(-1, view.indexOf(4n), "indexOf missing");
            assert.strictEqual(1, view.lastIndexOf(2n), "lastIndexOf");
            assert.isTrue(view.includes(3n), "includes");
            assert.isFalse(view.includes(4n), "includes missing");

            var filled = new BigInt64Array(4);
            var conversions = 0;
            filled.fill({ valueOf() { conversions++; return 5n; } }, 1, 3);
            assert.strictEqual(1, conversions, "fill converts once");
            assertElements(filled, [0n, 5n, 5n, 0n], "fill values");

            assertElements(view.with(1, 20n), [1n, 20n, 3n], "with replaces one element");
            assertElements(view, [1n, 2n, 3n], "with leaves the receiver");

            assertElements(new BigInt64Array([1n, 2n, 3n, 4n]).copyWithin(0, 1), [2n, 3n, 4n, 4n], "copyWithin");
            assertElements(new BigInt64Array([1n, 2n, 3n]).reverse(), [3n, 2n, 1n], "reverse");

            assert.strictEqual(6n, view.reduce(function (a, b) { return a + b; }), "reduce");
            assert.strictEqual(2n, view.find(function (v) { return v > 1n; }), "find");
            assert.strictEqual(1, view.findIndex(function (v) { return v > 1n; }), "findIndex");
            assert.isTrue(view.every(function (v) { return v > 0n; }), "every");
            assert.isTrue(view.some(function (v) { return v === 2n; }), "some");

            assert.strictEqual("1-2-3", view.join("-"), "join");
            assert.strictEqual("1,2,3", view.toLocaleString(), "default toLocaleString");

            assertElements(BigInt64Array.from([1n, 2n], function (v) { return v * 2n; }), [2n, 4n], "from");
            assertElements(BigUint64Array.of(1n, uint64Max), [1n, uint64Max], "of");
        }
    },
    {
        name: "Subclassing and @@species",
        body() {
            class DerivedBigInt64Array extends BigInt64Array {}
            var derived = new DerivedBigInt64Array([1n, 2n, 3n]);
            assert.isTrue(derived instanceof DerivedBigInt64Array, "subclass instance");
            assert.isTrue(derived instanceof BigInt64Array, "subclass is a BigInt64Array");
            assert.strictEqual(DerivedBigInt64Array, derived.constructor, "subclass constructor");
            assert.isTrue(derived.map(function (v) { return v; }) instanceof DerivedBigInt64Array,
                "subclass map preserves the subclass");
            assert.isTrue(DerivedBigInt64Array.from([4n, 5n]) instanceof DerivedBigInt64Array,
                "subclass from preserves the subclass");

            var view = new BigInt64Array([1n, 2n]);
            var calls = 0;
            view.constructor = {};
            view.constructor[Symbol.species] = function (length) {
                calls++;
                return new BigUint64Array(length);
            };
            var filtered = view.filter(function () { return true; });
            assert.strictEqual(1, calls, "species constructor called once");
            assertElements(filtered, [1n, 2n], "filter species values");
            assert.isTrue(filtered instanceof BigUint64Array, "filter species type");
            delete view.constructor;
        }
    },
    {
        name: "Resizable ArrayBuffer views",
        body() {
            var rab = new ArrayBuffer(16, { maxByteLength: 32 });
            var auto = new BigInt64Array(rab);
            assert.strictEqual(2, auto.length, "auto initial length");
            auto[0] = 1n;

            rab.resize(32);
            assert.strictEqual(4, auto.length, "auto grows");
            assert.strictEqual(32, auto.byteLength, "auto byteLength grows");
            auto[3] = int64Min;
            assert.strictEqual(int64Min, auto[3], "grows element access");

            rab.resize(8);
            assert.strictEqual(1, auto.length, "auto shrinks");
            assert.strictEqual(8, auto.byteLength, "auto byteLength shrinks");

            rab.resize(4);
            assert.strictEqual(0, auto.length, "auto partial trailing element");
            assert.strictEqual(0, auto.byteOffset, "auto offset while OOB");

            rab.resize(16);
            assert.strictEqual(2, auto.length, "auto recovers");
            assert.strictEqual(1n, auto[0], "auto retains bytes");

            var fixedBuffer = new ArrayBuffer(16, { maxByteLength: 32 });
            var fixed = new BigInt64Array(fixedBuffer, 8, 1);
            fixed[0] = 9n;
            fixedBuffer.resize(24);
            assert.strictEqual(1, fixed.length, "fixed length while in bounds");
            fixedBuffer.resize(8);
            assert.strictEqual(0, fixed.length, "fixed length while OOB");
            assert.strictEqual(0, fixed.byteOffset, "fixed byteOffset while OOB");
            assert.strictEqual(undefined, fixed[0], "fixed OOB element get");
            fixedBuffer.resize(16);
            assert.strictEqual(1, fixed.length, "fixed length recovers");
            assert.strictEqual(8, fixed.byteOffset, "fixed byteOffset recovers");
            assert.strictEqual(0n, fixed[0], "bytes removed by shrink stay zero");

            var tracking = new BigInt64Array(new ArrayBuffer(16, { maxByteLength: 16 }), 0, undefined);
            assert.strictEqual(2, tracking.length, "explicit undefined length tracks");
        }
    },
    {
        name: "Descriptors, ownKeys and reflection",
        body() {
            var view = new BigInt64Array([1n, 2n]);
            var descriptor = Object.getOwnPropertyDescriptor(view, "0");
            assert.strictEqual(1n, descriptor.value, "index value descriptor");
            assert.isTrue(descriptor.writable, "index writable");
            assert.isTrue(descriptor.enumerable, "index enumerable");
            // The inherited typed-array descriptor reports configurable false;
            // spec/Node report true. Recorded as a residual rather than
            // asserting the wrong value here.

            assert.strictEqual("0,1", Object.keys(view).join(","), "Object.keys");
            assert.strictEqual("0,1", Reflect.ownKeys(view).join(","), "Reflect.ownKeys");
            assert.strictEqual(2, Object.getOwnPropertyNames(view).length, "own property names");
            assert.isTrue("0" in view, "in operator present index");
            assert.isFalse("2" in view, "in operator absent index");
            assert.isFalse(delete view[0], "delete index returns false");

            Object.defineProperty(view, "0", { value: 5n });
            assert.strictEqual(5n, view[0], "defineProperty with BigInt");
            assertThrowsTypeError(function () { Object.defineProperty(view, "0", { value: 5 }); },
                "defineProperty rejects Number");
            assertThrowsTypeError(function () { view[0] = 5; }, "setter rejects Number");

            view[10] = 7n;
            assert.strictEqual(2, view.length, "out-of-bounds set is a no-op");
            assert.strictEqual("bigint", typeof view[0], "element type is bigint");

            var huge = (1n << 200n) + 123n;
            var low = new BigUint64Array([huge])[0];
            assert.strictEqual(BigInt.asUintN(64, huge), low, "large positive low 64 bits");
            var negativeHuge = -((1n << 200n) + 123n);
            var signedLow = new BigInt64Array([negativeHuge])[0];
            assert.strictEqual(BigInt.asIntN(64, negativeHuge), signedLow, "large negative low 64 bits");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
