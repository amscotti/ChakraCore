//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S2: BigInt SameValueZero keys in Map/Set, BigInt literal property names,
// and DataView 64-bit BigInt accessors. See
// doc/c2-s2-bigint-collections.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var int64Min = -9223372036854775808n;
var int64Max = 9223372036854775807n;
var uint64Max = 18446744073709551615n;

function assertThrowsTypeError(fn, message) {
    assert.throws(fn, TypeError, message);
}

function assertThrowsRangeError(fn, message) {
    assert.throws(fn, RangeError, message);
}

var tests = [
    {
        name: "Map and Set use BigInt SameValueZero identity",
        body() {
            var map = new Map([[1n, "one"], [1n, "again"]]);
            assert.strictEqual(1, map.size, "identical BigInt keys collapse");
            assert.strictEqual("again", map.get(1n), "the latest value is retained");
            assert.isTrue(map.has(1n), "has finds an equal BigInt");
            assert.isTrue(map.delete(1n), "delete removes an equal BigInt");
            assert.strictEqual(0, map.size, "delete removed the entry");
            assert.isFalse(map.has(1n), "deleted key is gone");
            assert.strictEqual(undefined, map.get(1n), "deleted key has no value");

            // Same value produced by different paths, including a large value
            // that exercises more than one digit.
            var big = 123456789012345678901234567890n;
            var bigFromString = BigInt("123456789012345678901234567890");
            map.set(big, 1);
            map.set(bigFromString, 2);
            assert.strictEqual(1, map.size, "equal large BigInt values share a key");
            assert.strictEqual(2, map.get(big), "large BigInt lookup by value");
            assert.strictEqual(2, map.get(bigFromString), "large BigInt lookup by parsed value");

            map.clear();
            map.set(-1n, "negative");
            assert.strictEqual("negative", map.get(-0n - 1n), "negative BigInt key");
            map.set(0n, "zero");
            assert.strictEqual(2, map.size, "zero and negative one are distinct");

            var set = new Set([1n, 1n, 2n, 2n]);
            assert.strictEqual(2, set.size, "Set dedupes equal BigInts");
            assert.isTrue(set.has(1n), "Set.has equal BigInt");
            assert.isTrue(set.delete(2n), "Set.delete equal BigInt");
            assert.strictEqual(1, set.size, "Set.delete removed one value");
            assert.isFalse(set.has(2n), "deleted Set value is gone");

            var largeSet = new Set([big, bigFromString]);
            assert.strictEqual(1, largeSet.size, "Set dedupes large equal BigInts");
        }
    },
    {
        name: "BigInt and Number remain distinct collection keys",
        body() {
            var map = new Map();
            map.set(1, "number");
            map.set(1n, "bigint");
            assert.strictEqual(2, map.size, "Number 1 and BigInt 1n are distinct");
            assert.strictEqual("number", map.get(1), "Number key keeps its value");
            assert.strictEqual("bigint", map.get(1n), "BigInt key keeps its value");

            map.set(0, "zero number");
            map.set(0n, "zero bigint");
            map.set(-0, "negative zero number");
            assert.strictEqual(4, map.size, "0/0n/-0 are the expected three keys");
            assert.strictEqual("negative zero number", map.get(0), "SameValueZero folds -0 into +0");

            var set = new Set([9007199254740991, 9007199254740991n]);
            assert.strictEqual(2, set.size, "Set keeps Number and BigInt distinct");
            assert.isTrue(set.has(9007199254740991), "Set has Number");
            assert.isTrue(set.has(9007199254740991n), "Set has BigInt");

            var objectKey = {};
            var otherObjectKey = {};
            var symbolKey = Symbol("key");
            var identityMap = new Map();
            identityMap.set(objectKey, "object");
            identityMap.set(otherObjectKey, "other object");
            identityMap.set("text", "string");
            identityMap.set("text", "string again");
            identityMap.set(symbolKey, "symbol");
            identityMap.set(NaN, "nan");
            identityMap.set(NaN, "nan again");
            assert.strictEqual(5, identityMap.size, "existing object/string/symbol/NaN identity is preserved");
            assert.strictEqual("object", identityMap.get(objectKey), "object identity lookup");
            assert.strictEqual(undefined, identityMap.get({}), "a fresh object is not an equal key");
            assert.strictEqual("string again", identityMap.get("text"), "string value key");
            assert.strictEqual("symbol", identityMap.get(symbolKey), "symbol identity lookup");
            assert.strictEqual("nan again", identityMap.get(NaN), "NaN SameValueZero identity");

            var weakMap = new WeakMap();
            weakMap.set(objectKey, "weak");
            assert.strictEqual("weak", weakMap.get(objectKey), "WeakMap object behavior is unchanged");
            var weakSet = new WeakSet();
            weakSet.add(otherObjectKey);
            assert.isTrue(weakSet.has(otherObjectKey), "WeakSet object behavior is unchanged");

            assertThrowsTypeError(function () { weakMap.set(1n, "bigint"); }, "BigInt is not a WeakMap key");
            assertThrowsTypeError(function () { weakSet.add(1n); }, "BigInt is not a WeakSet key");
        }
    },
    {
        name: "BigInt literal property names use the decimal value string",
        body() {
            var object = { 1n: "one" };
            assert.strictEqual("one", object["1"], "decimal BigInt property key");
            assert.strictEqual("one", object[1n], "BigInt property access agrees");

            var large = { 999999999999999999n: true };
            assert.isTrue(large["999999999999999999"], "key above MAX_SAFE_INTEGER");

            var hex = { 0x10n: "sixteen", 0b101n: "five", 0o17n: "fifteen" };
            assert.strictEqual("sixteen", hex[16], "hex literal key is canonical decimal");
            assert.strictEqual("five", hex[5], "binary literal key is canonical decimal");
            assert.strictEqual("fifteen", hex[15], "octal literal key is canonical decimal");

            var withSeparators = { 1_000n: "thousand" };
            assert.strictEqual("thousand", withSeparators[1000], "separators are not part of the key");

            var method = { 1n() { return "bar"; } };
            assert.strictEqual("bar", method["1"](), "BigInt-named object method");

            var accessorValue;
            var accessor = {
                get 2n() { return accessorValue; },
                set 2n(value) { accessorValue = value; }
            };
            accessor["2"] = "set through string";
            assert.strictEqual("set through string", accessor[2n], "BigInt-named setter/getter");

            var destructured;
            var { 3n: destructured } = { "3": "three" };
            assert.strictEqual("three", destructured, "BigInt-named destructuring binding");

            class C {
                4n() { return "class method"; }
                static 5n() { return "static method"; }
                [6n]() { return "computed method"; }
            }
            var instance = new C();
            assert.strictEqual("class method", instance["4"](), "BigInt-named class method");
            assert.strictEqual("static method", C["5"](), "BigInt-named static class method");
            assert.strictEqual("computed method", instance["6"](), "computed BigInt class key");

            class NamedBigInt {
                1n() {}
                static 3n() {}
                get 2n() { return "getter"; }
                set 4n(value) { this.setValue = value; }
            }
            var namedBigInt = new NamedBigInt();
            assert.strictEqual("1", namedBigInt["1"].name, "instance BigInt method name");
            assert.strictEqual("3", NamedBigInt["3"].name, "static BigInt method name");
            var getBigInt = Object.getOwnPropertyDescriptor(NamedBigInt.prototype, "2").get;
            var setBigInt = Object.getOwnPropertyDescriptor(NamedBigInt.prototype, "4").set;
            assert.strictEqual("get 2", getBigInt.name, "BigInt getter name");
            assert.strictEqual("set 4", setBigInt.name, "BigInt setter name");

            class NamedNumber {
                5() {}
                static 7() {}
                get 6() { return "getter"; }
                set 8(value) { this.setValue = value; }
            }
            var namedNumber = new NamedNumber();
            assert.strictEqual("5", namedNumber["5"].name, "instance numeric method name");
            assert.strictEqual("7", NamedNumber["7"].name, "static numeric method name");
            assert.strictEqual("get 6", Object.getOwnPropertyDescriptor(NamedNumber.prototype, "6").get.name,
                "numeric getter name");
            assert.strictEqual("set 8", Object.getOwnPropertyDescriptor(NamedNumber.prototype, "8").set.name,
                "numeric setter name");

            var computed = { [7n]: "seven" };
            assert.strictEqual("seven", computed[7], "computed BigInt property key");
            assert.strictEqual("seven", computed[7n], "computed BigInt property access");
            assert.strictEqual("7", Object.keys(computed)[0], "computed key is a string property");

            class D {
                8n = "field";
            }
            assert.strictEqual("field", new D()["8"], "BigInt-named class field");

            assert.strictEqual(1, eval("({1n: 1})")[1], "eval parses BigInt literal key");
            assert.strictEqual(3, new Function("return {3n: 3}")()[3], "new Function parses BigInt literal key");
            function deferred() { return { 4n: 4 }; }
            assert.strictEqual(4, deferred()[4], "deferred function parses BigInt literal key");
        }
    },
    {
        name: "DataView BigInt accessor shape and descriptors",
        body() {
            assert.strictEqual(1, DataView.length, "DataView constructor length");
            var proto = DataView.prototype;
            ["getBigInt64", "getBigUint64"].forEach(function (name) {
                var descriptor = Object.getOwnPropertyDescriptor(proto, name);
                assert.isTrue(!!descriptor, name + " is an own prototype property");
                assert.strictEqual("function", typeof descriptor.value, name + " is a function");
                assert.isTrue(descriptor.writable, name + " is writable");
                assert.isFalse(descriptor.enumerable, name + " is not enumerable");
                assert.isTrue(descriptor.configurable, name + " is configurable");
                assert.strictEqual(name, descriptor.value.name, name + " function name");
                assert.strictEqual(1, descriptor.value.length, name + " function length");
            });
            ["setBigInt64", "setBigUint64"].forEach(function (name) {
                var descriptor = Object.getOwnPropertyDescriptor(proto, name);
                assert.isTrue(!!descriptor, name + " is an own prototype property");
                assert.strictEqual("function", typeof descriptor.value, name + " is a function");
                assert.isTrue(descriptor.writable, name + " is writable");
                assert.isFalse(descriptor.enumerable, name + " is not enumerable");
                assert.isTrue(descriptor.configurable, name + " is configurable");
                assert.strictEqual(name, descriptor.value.name, name + " function name");
                assert.strictEqual(2, descriptor.value.length, name + " function length");
            });

            assertThrowsTypeError(function () { new proto.getBigInt64(); }, "getBigInt64 is not a constructor");
            assertThrowsTypeError(function () { new proto.setBigInt64(); }, "setBigInt64 is not a constructor");
            assertThrowsTypeError(function () { proto.getBigInt64.call({}); }, "getBigInt64 receiver must be a DataView");
            assertThrowsTypeError(function () { proto.setBigInt64.call({}, 0, 1n); }, "setBigInt64 receiver must be a DataView");
        }
    },
    {
        name: "DataView BigInt reads, endianness and exact boundaries",
        body() {
            var buffer = new ArrayBuffer(8);
            var view = new DataView(buffer);

            view.setBigUint64(0, 0x0123456789abcdefn);
            var bytes = new Uint8Array(buffer);
            assert.strictEqual(0x01, bytes[0], "big-endian high byte");
            assert.strictEqual(0x23, bytes[1], "big-endian second byte");
            assert.strictEqual(0xef, bytes[7], "big-endian low byte");
            assert.strictEqual(0x0123456789abcdefn, view.getBigUint64(0), "big-endian round trip");
            assert.strictEqual(0x0123456789abcdefn, view.getBigUint64(0, false), "explicit big endian");
            assert.strictEqual(0xefcdab8967452301n, view.getBigUint64(0, true), "little-endian read");

            view.setBigUint64(0, 0x0123456789abcdefn, true);
            assert.strictEqual(0xef, bytes[0], "little-endian low byte");
            assert.strictEqual(0x01, bytes[7], "little-endian high byte");
            assert.strictEqual(0x0123456789abcdefn, view.getBigUint64(0, true), "little-endian round trip");

            view.setBigInt64(0, int64Max);
            assert.strictEqual(int64Max, view.getBigInt64(0), "INT64_MAX");
            assert.strictEqual(int64Max, view.getBigUint64(0), "INT64_MAX unsigned");

            view.setBigInt64(0, int64Min);
            assert.strictEqual(int64Min, view.getBigInt64(0), "INT64_MIN");
            assert.strictEqual(0x8000000000000000n, view.getBigUint64(0), "INT64_MIN unsigned bits");

            view.setBigUint64(0, uint64Max);
            assert.strictEqual(uint64Max, view.getBigUint64(0), "UINT64_MAX");
            assert.strictEqual(-1n, view.getBigInt64(0), "UINT64_MAX signed bits");
            assert.strictEqual(-1n, view.getBigInt64(0, true), "UINT64_MAX signed bits little-endian");

            view.setBigInt64(0, -2n);
            assert.strictEqual(-2n, view.getBigInt64(0), "negative signed value");
            assert.strictEqual(0xfffffffffffffffen, view.getBigUint64(0), "negative signed bits unsigned");
        }
    },
    {
        name: "DataView BigInt conversion, offsets and ordering",
        body() {
            var view = new DataView(new ArrayBuffer(16), 4, 8);
            view.setBigInt64(0, 5n);
            assert.strictEqual(5n, view.getBigInt64(0), "offset view round trip");

            view.setBigInt64(0, 0x10000000000000005n);
            assert.strictEqual(5n, view.getBigInt64(0), "store keeps the low 64 bits");
            view.setBigInt64(0, -0x10000000000000005n);
            assert.strictEqual(-5n, view.getBigInt64(0), "negative wrapped store");

            view.setBigUint64(0, "42");
            assert.strictEqual(42n, view.getBigUint64(0), "string ToBigInt");
            view.setBigUint64(0, true);
            assert.strictEqual(1n, view.getBigUint64(0), "boolean ToBigInt");
            view.setBigUint64(0, { valueOf() { return 7n; } });
            assert.strictEqual(7n, view.getBigUint64(0), "object ToBigInt valueOf");

            assertThrowsTypeError(function () { view.setBigInt64(0, 1); }, "Number value is not a BigInt");
            assertThrowsTypeError(function () { view.setBigInt64(0, null); }, "null value is not a BigInt");
            assertThrowsTypeError(function () { view.setBigInt64(0); }, "missing value is undefined");
            assertThrowsTypeError(function () { view.setBigInt64(0, Symbol("value")); }, "Symbol value");
            assert.throws(function () { view.setBigInt64(0, "not a bigint"); }, SyntaxError, "invalid StringToBigInt");

            view.setBigInt64(0, 5n);
            assert.strictEqual(5n, view.getBigInt64(undefined), "undefined offset is ToIndex 0");
            view.setBigUint64(0, 42n);
            assert.strictEqual(42n, view.getBigUint64(undefined), "undefined offset is ToIndex 0 for unsigned");
            assertThrowsRangeError(function () { view.getBigInt64(-1); }, "negative offset");
            assertThrowsRangeError(function () { view.getBigInt64(Infinity); }, "infinite offset");
            assertThrowsRangeError(function () { view.getBigInt64(9007199254740992); }, "offset above ToIndex limit");
            assertThrowsTypeError(function () { view.getBigInt64(0n); }, "BigInt offset is not ToIndex-able");
            assertThrowsTypeError(function () { view.getBigInt64(Symbol("offset")); }, "Symbol offset");
            assertThrowsRangeError(function () { view.getBigInt64(9); }, "read past the view");

            var calls = 0;
            assertThrowsRangeError(function () {
                view.setBigInt64(-1, { valueOf() { calls++; return 1n; } });
            }, "invalid offset throws before value conversion");
            assert.strictEqual(0, calls, "value conversion did not run for an invalid offset");

            calls = 0;
            assert.throws(function () {
                view.setBigInt64(100, { valueOf() { calls++; throw new Error("conversion"); } });
            }, Error, "conversion runs before the range check");
            assert.strictEqual(1, calls, "value conversion ran exactly once");

            calls = 0;
            assert.throws(function () {
                view.setBigInt64(0, { valueOf() { calls++; throw new Error("conversion"); } });
            }, Error, "abrupt ToBigInt propagates");
            assert.strictEqual(1, calls, "abrupt conversion ran exactly once");
        }
    },
    {
        name: "DataView BigInt detached and resizable views",
        body() {
            var detached = new DataView(new ArrayBuffer(8));
            detached.buffer.transfer();
            assertThrowsRangeError(function () { detached.getBigInt64(Infinity); }, "detached read: ToIndex first");
            assertThrowsTypeError(function () { detached.getBigInt64(0); }, "detached read");
            assertThrowsTypeError(function () { detached.getBigInt64(13); }, "detached read before range");
            assertThrowsRangeError(function () { detached.setBigInt64(Infinity, 0n); }, "detached store: ToIndex first");
            assertThrowsTypeError(function () { detached.setBigInt64(0, 1n); }, "detached store");
            var calls = 0;
            assert.throws(function () {
                detached.setBigInt64(0, { valueOf() { calls++; throw new Error("detached conversion"); } });
            }, Error, "detached store converts before the detached check");
            assert.strictEqual(1, calls, "detached store conversion ran once");

            var rab = new ArrayBuffer(16, { maxByteLength: 24 });
            var fixedView = new DataView(rab, 0, 8);
            var trackingView = new DataView(rab);
            fixedView.setBigInt64(0, 7n);
            assert.strictEqual(7n, trackingView.getBigInt64(0), "tracking view sees write");
            rab.resize(8);
            assert.strictEqual(7n, fixedView.getBigInt64(0), "fixed view still in range");
            rab.resize(4);
            assertThrowsTypeError(function () { fixedView.getBigInt64(0); }, "fixed view out of bounds after shrink");
            assertThrowsTypeError(function () { fixedView.setBigInt64(0, 1n); }, "fixed store out of bounds after shrink");
            if (trackingView.byteLength === 4) {
                assertThrowsRangeError(function () { trackingView.getBigInt64(0); }, "tracking view shorter than 8 bytes");
            }
            rab.resize(16);
            // Bytes removed by the shrink read as zero after recovery,
            // matching the existing resizable typed-array behavior.
            assert.strictEqual(0n, fixedView.getBigInt64(0), "fixed view recovers after grow");
            fixedView.setBigInt64(0, 9n);
            assert.strictEqual(9n, trackingView.getBigInt64(0), "tracking view recovers after grow");
        }
    },
    {
        name: "Numeric DataView setters convert offset before value",
        body: function () {
            // SetViewValue: ToIndex(byteOffset) precedes ToNumber(value)
            // for every setter, matching the BigInt setter order.
            var view = new DataView(new ArrayBuffer(16));
            function orderOf(invoke) {
                var order = [];
                var idx = { valueOf: function () { order.push("offset"); return 0; } };
                var val = { valueOf: function () { order.push("value"); return 7; } };
                invoke(idx, val);
                return order.join(",");
            }
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setInt8(i, v); }), "setInt8 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setUint8(i, v); }), "setUint8 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setInt16(i, v); }), "setInt16 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setUint16(i, v); }), "setUint16 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setInt32(i, v); }), "setInt32 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setUint32(i, v); }), "setUint32 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setFloat32(i, v); }), "setFloat32 order");
            assert.strictEqual("offset,value", orderOf(function (i, v) { view.setFloat64(i, v); }), "setFloat64 order");
            assert.strictEqual(7, view.getFloat64(0), "writes still land");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
