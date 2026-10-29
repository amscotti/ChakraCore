//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// S2 default-enablement coverage: the BigInt surface must be available,
// correct and consistent under the DEFAULT configuration (no -ESBigInt
// flag). Arithmetic/collections/typed arrays/DataView checks mirror the
// flag-on receipts (doc/c2-s2-bigint*.md); this entry pins them to the
// default profile so a future default regression cannot hide behind the
// explicit flag. See doc/c2-s2-bigint-default.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var int64Min = -9223372036854775808n;
var int64Max = 9223372036854775807n;
var uint64Max = 18446744073709551615n;

var tests = [
    {
        name: "BigInt globals are present by default",
        body() {
            assert.strictEqual("function", typeof BigInt, "typeof BigInt");
            assert.strictEqual("bigint", typeof 1n, "typeof 1n");
            assert.strictEqual("function", typeof BigInt64Array, "typeof BigInt64Array");
            assert.strictEqual("function", typeof BigUint64Array, "typeof BigUint64Array");
            assert.strictEqual("bigint", typeof BigInt.asIntN(64, 1n), "asIntN present");
            assert.strictEqual("function", typeof DataView.prototype.getBigInt64, "DataView getBigInt64");
            assert.strictEqual("function", typeof DataView.prototype.setBigUint64, "DataView setBigUint64");
        }
    },
    {
        name: "Literals, conversions and stringification",
        body() {
            assert.strictEqual(255n, 0xffn, "hex literal");
            assert.strictEqual(8n, 0o10n, "octal literal");
            assert.strictEqual(5n, 0b101n, "binary literal");
            assert.strictEqual(12000000n, 12_000_000n, "numeric separators");
            assert.strictEqual("123456789012345678901234567890", (123456789012345678901234567890n).toString(), "multi-limb decimal toString");
            assert.strictEqual("1", String(1n), "String(1n)");
            assert.strictEqual(42n, BigInt("42"), "BigInt(string)");
            assert.strictEqual(42n, BigInt(42), "BigInt(integral number)");
            assert.strictEqual(1, Number(1n), "Number(BigInt)");
            assert.isFalse(Boolean(0n), "Boolean(0n)");
            assert.isTrue(Boolean(2n), "Boolean(2n)");
            assert.strictEqual("10", (16n).toString(16), "radix toString");
            assert.areEqual("[object BigInt]", Object.prototype.toString.call(1n), "toStringTag");
        }
    },
    {
        name: "Arithmetic, bitwise, shifts and comparisons",
        body() {
            assert.strictEqual(3n, 1n + 2n, "addition");
            assert.strictEqual(-1n, 1n - 2n, "subtraction");
            assert.strictEqual(6n, 2n * 3n, "multiplication");
            assert.strictEqual(3n, 7n / 2n, "truncating division");
            assert.strictEqual(1n, 7n % 2n, "remainder");
            assert.strictEqual(-3n, -5n >> 1n, "floor right shift");
            assert.strictEqual(16n, 1n << 4n, "left shift");
            assert.strictEqual(6n, 4n | 2n, "bitwise or");
            assert.strictEqual(-7n, ~6n, "bitwise not");
            assert.strictEqual(340282366920938463463374607431768211456n, 2n ** 128n, "exponentiation");
            assert.strictEqual(511n, BigInt.asUintN(9, -1n), "asUintN wrap");
            assert.strictEqual(-1n, BigInt.asIntN(9, 511n), "asIntN sign-extend");
            assert.isTrue(1n < 2n, "BigInt relational");
            assert.isTrue(2n == 2, "mixed abstract equality");
            assert.isTrue(1n === 1n, "strict equality");
            assert.areEqual("TypeError", (() => { try { 1n + 1; } catch (e) { return e.name; } })(), "mixed addition");
        }
    },
    {
        name: "Map and Set use BigInt value identity",
        body() {
            var map = new Map([[1n, "first"], [1n, "second"]]);
            assert.strictEqual(1, map.size, "equal BigInt keys collapse");
            assert.strictEqual("second", map.get(BigInt("1")), "lookup by equal value");
            var set = new Set([1n, BigInt("1"), 2n]);
            assert.strictEqual(2, set.size, "Set dedupes equal BigInts");
            map.set(1, "number");
            assert.strictEqual(2, map.size, "Number key is distinct from BigInt");
        }
    },
    {
        name: "BigInt64Array and BigUint64Array",
        body() {
            var signed = new BigInt64Array(3);
            signed[0] = int64Min;
            signed[1] = int64Max;
            signed[2] = 1n;
            assert.areEqual(int64Min, signed[0], "int64 min round trip");
            assert.areEqual(int64Max, signed[1], "int64 max round trip");
            assert.areEqual(1n, signed[2], "small value round trip");

            var unsigned = new BigUint64Array(2);
            unsigned[0] = uint64Max;
            unsigned[1] = -1n;
            assert.areEqual(uint64Max, unsigned[0], "uint64 max round trip");
            assert.areEqual(uint64Max, unsigned[1], "-1n wraps to uint64 max");

            assert.areEqual("9007199254740993", String(new BigInt64Array([9007199254740993n])[0]), "value above 2^53 is exact");
            assert.areEqual(int64Max, BigInt64Array.from([int64Max])[0], "from");
            assert.areEqual(int64Max, BigInt64Array.of(int64Max)[0], "of");
        }
    },
    {
        name: "DataView BigInt accessors",
        body() {
            var buffer = new ArrayBuffer(16);
            var view = new DataView(buffer);
            view.setBigInt64(0, int64Min, true);
            assert.areEqual(int64Min, view.getBigInt64(0, true), "little-endian int64 min");
            // The same eight bytes read big-endian are the byte-reversed value:
            // 0x8000000000000000 little-endian is 0x0000000000000080 big-endian.
            assert.areEqual(128n, view.getBigInt64(0, false), "big-endian read of same bytes");
            view.setBigUint64(8, uint64Max, false);
            assert.areEqual(uint64Max, view.getBigUint64(8, false), "big-endian uint64 max");
            assert.areEqual("TypeError", (() => { try { view.setBigInt64(0, 1); } catch (e) { return e.name; } })(), "ToBigInt rejects Number");
            assert.areEqual("RangeError", (() => { try { view.getBigInt64(99, true); } catch (e) { return e.name; } })(), "offset range check");
        }
    },
    {
        name: "Property names, JSON and wrappers",
        body() {
            var object = { 1n: "one", 0x10n: "sixteen" };
            assert.strictEqual("one", object["1"], "decimal literal key");
            assert.strictEqual("sixteen", object["16"], "hex literal key uses its decimal string");
            assert.areEqual("TypeError", (() => { try { JSON.stringify(1n); } catch (e) { return e.name; } })(), "JSON.stringify rejects BigInt");
            var wrapped = Object(2n);
            assert.areEqual(2n, wrapped.valueOf(), "wrapper valueOf");
            assert.areEqual(6n, wrapped * 3n, "wrapper multiplication");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
