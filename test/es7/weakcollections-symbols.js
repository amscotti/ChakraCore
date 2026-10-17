//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "WeakMap stores symbol keys through set/has/get/delete",
        body() {
            var map = new WeakMap();
            var key = Symbol("weakmap-key");

            assert.isFalse(map.has(key), "absent symbol key");
            assert.areEqual(undefined, map.get(key), "absent symbol key returns undefined");
            assert.isFalse(map.delete(key), "deleting an absent symbol key returns false");

            assert.areEqual(map, map.set(key, "value"), "set returns the receiver");
            assert.isTrue(map.has(key), "symbol key is present after set");
            assert.areEqual("value", map.get(key), "symbol key returns its value");
            assert.areEqual(undefined, map.get(Symbol("other")), "different symbol is a different key");

            assert.isTrue(map.delete(key), "deleting a present symbol key returns true");
            assert.isFalse(map.has(key), "symbol key is absent after delete");
            assert.areEqual(undefined, map.get(key), "deleted symbol key returns undefined");
            assert.isFalse(map.delete(key), "second delete returns false");
        }
    },
    {
        name: "WeakMap overwrites a symbol key value and keys stay distinct",
        body() {
            var map = new WeakMap();
            var first = Symbol("first");
            var second = Symbol("second");

            map.set(first, 1);
            map.set(first, 2);
            map.set(second, 3);

            assert.areEqual(2, map.get(first), "set overwrites the previous value");
            assert.areEqual(3, map.get(second), "distinct symbols do not share an entry");
            assert.isTrue(map.delete(first), "first key deletes");
            assert.isFalse(map.has(first), "first key is gone");
            assert.areEqual(3, map.get(second), "second key is untouched");
        }
    },
    {
        name: "well-known symbols can be weak-collection keys",
        body() {
            var map = new WeakMap();
            var set = new WeakSet();
            var symbols = [Symbol.iterator, Symbol.toPrimitive, Symbol.toStringTag, Symbol.asyncIterator];

            for (var i = 0; i < symbols.length; i++) {
                map.set(symbols[i], i);
                set.add(symbols[i]);
            }
            for (var j = 0; j < symbols.length; j++) {
                assert.areEqual(j, map.get(symbols[j]), "well-known symbol " + j + " in WeakMap");
                assert.isTrue(set.has(symbols[j]), "well-known symbol " + j + " in WeakSet");
            }
        }
    },
    {
        name: "symbol wrapper objects still use the object key path",
        body() {
            var map = new WeakMap();
            var set = new WeakSet();
            var symbol = Symbol("wrapped");
            var wrapper = Object(symbol);

            assert.isTrue(typeof wrapper === "object", "Object(symbol) is an object");
            map.set(wrapper, "wrapper-value");
            set.add(wrapper);

            assert.isTrue(map.has(wrapper), "wrapper object is present");
            assert.areEqual("wrapper-value", map.get(wrapper), "wrapper object value");
            assert.isFalse(map.has(symbol), "wrapper object and primitive symbol are different keys");
            assert.areEqual(undefined, map.get(symbol), "primitive symbol has no entry");
            assert.isTrue(set.has(wrapper), "wrapper object is in the WeakSet");
            assert.isFalse(set.has(symbol), "primitive symbol is not in the WeakSet");

            map.set(symbol, "symbol-value");
            assert.areEqual("symbol-value", map.get(symbol), "primitive symbol entry is separate");
            assert.areEqual("wrapper-value", map.get(wrapper), "wrapper entry is preserved");
            assert.isTrue(map.delete(wrapper), "wrapper object deletes");
            assert.areEqual("symbol-value", map.get(symbol), "symbol entry survives wrapper delete");
        }
    },
    {
        name: "non-key primitives throw on set/add and miss on get/has/delete",
        body() {
            var map = new WeakMap();
            var set = new WeakSet();
            var primitives = [undefined, null, true, 1, "string"];

            for (var i = 0; i < primitives.length; i++) {
                var value = primitives[i];
                assert.throws(function () { map.set(value, i); }, TypeError, "WeakMap.set rejects " + String(value));
                assert.throws(function () { set.add(value); }, TypeError, "WeakSet.add rejects " + String(value));
                assert.areEqual(undefined, map.get(value), "WeakMap.get misses " + String(value));
                assert.isFalse(map.has(value), "WeakMap.has misses " + String(value));
                assert.isFalse(map.delete(value), "WeakMap.delete misses " + String(value));
                assert.isFalse(set.has(value), "WeakSet.has misses " + String(value));
                assert.isFalse(set.delete(value), "WeakSet.delete misses " + String(value));
            }
        }
    },
    {
        name: "registered symbols cannot be held weakly",
        body() {
            var map = new WeakMap();
            var set = new WeakSet();
            var registered = Symbol.for("weakcollections-registered-key");

            assert.throws(function () { map.set(registered, 1); }, TypeError, "WeakMap.set rejects Symbol.for");
            assert.throws(function () { set.add(registered); }, TypeError, "WeakSet.add rejects Symbol.for");
            assert.areEqual(undefined, map.get(registered), "WeakMap.get misses Symbol.for");
            assert.isFalse(map.has(registered), "WeakMap.has misses Symbol.for");
            assert.isFalse(map.delete(registered), "WeakMap.delete misses Symbol.for");
            assert.isFalse(set.has(registered), "WeakSet.has misses Symbol.for");
            assert.isFalse(set.delete(registered), "WeakSet.delete misses Symbol.for");

            // An unregistered symbol with the same description is still a valid key.
            var unregistered = Symbol("weakcollections-registered-key");
            map.set(unregistered, 2);
            set.add(unregistered);
            assert.areEqual(2, map.get(unregistered), "unregistered same-description symbol is a valid key");
            assert.isTrue(set.has(unregistered), "unregistered same-description symbol is a valid value");
        }
    },
    {
        name: "WeakSet stores symbol values through add/has/delete",
        body() {
            var set = new WeakSet();
            var key = Symbol("weakset-key");

            assert.isFalse(set.has(key), "absent symbol value");
            assert.isFalse(set.delete(key), "deleting an absent symbol value returns false");
            assert.areEqual(set, set.add(key), "add returns the receiver");
            assert.isTrue(set.has(key), "symbol value is present after add");
            assert.areEqual(set, set.add(key), "adding a duplicate symbol still returns the receiver");
            assert.isTrue(set.has(key), "duplicate add keeps the value");
            assert.isTrue(set.delete(key), "deleting a present symbol value returns true");
            assert.isFalse(set.has(key), "symbol value is absent after delete");
            assert.isFalse(set.delete(key), "second delete returns false");
        }
    },
    {
        name: "constructors accept iterables with symbol keys and values",
        body() {
            var first = Symbol("first");
            var second = Symbol("second");
            var wrapper = Object(first);
            var map = new WeakMap([[first, 1], [second, 2], [wrapper, 3]]);
            var set = new WeakSet([first, second, wrapper]);

            assert.areEqual(1, map.get(first), "iterable symbol key first");
            assert.areEqual(2, map.get(second), "iterable symbol key second");
            assert.areEqual(3, map.get(wrapper), "iterable wrapper key");
            assert.isFalse(map.has(Object(second)), "another wrapper for the same symbol is a distinct object key");
            assert.isTrue(set.has(first), "iterable symbol value first");
            assert.isTrue(set.has(second), "iterable symbol value second");
            assert.isTrue(set.has(wrapper), "iterable wrapper value");
            assert.isFalse(set.has(Object(second)), "another wrapper is a distinct value");
        }
    },
    {
        name: "symbol entries are isolated between collections and kinds",
        body() {
            var key = Symbol("shared");
            var first = new WeakMap([[key, "first"]]);
            var second = new WeakMap([[key, "second"]]);
            var set = new WeakSet([key]);

            assert.areEqual("first", first.get(key), "first map value");
            assert.areEqual("second", second.get(key), "second map value");
            assert.isTrue(set.has(key), "set sees the symbol");

            assert.isTrue(second.delete(key), "second map deletes its own entry");
            assert.isFalse(second.has(key), "second map entry is gone");
            assert.areEqual("first", first.get(key), "first map entry is untouched");
            assert.isTrue(set.has(key), "set entry is untouched");

            assert.isTrue(set.delete(key), "set deletes its entry");
            assert.areEqual("first", first.get(key), "map entry survives set delete");
        }
    },
    {
        name: "WeakSet symbol keys do not leave stale entries after collection",
        body() {
            var set = new WeakSet();
            for (var i = 0; i < 20000; i++) {
                set.add(Symbol("drop-" + i));
            }
            CollectGarbage();
            CollectGarbage();

            // The dropped keys must not alias fresh symbols through stale
            // dictionary entries after collection/reuse.
            for (var j = 0; j < 50000; j++) {
                assert.isFalse(set.has(Symbol("fresh-" + j)), "fresh symbol " + j + " is absent");
            }
        }
    },
    {
        name: "symbol entries and strong values survive collection",
        body() {
            var map = new WeakMap();
            var set = new WeakSet();
            var keys = [];
            var marker = { marker: "strong-value" };

            for (var i = 0; i < 100; i++) {
                var key = Symbol("gc-" + i);
                keys.push(key);
                map.set(key, i === 0 ? marker : i);
                set.add(key);
            }
            // Object keys are weak and may be collected; symbol entries must stay.
            for (var j = 0; j < 500; j++) {
                map.set({}, j);
            }
            marker = null;
            CollectGarbage();
            CollectGarbage();

            for (var k = 0; k < keys.length; k++) {
                var expected = k === 0 ? "strong-value" : k;
                var value = map.get(keys[k]);
                if (k === 0) {
                    assert.areEqual("strong-value", value.marker, "strong value survived collection");
                } else {
                    assert.areEqual(expected, value, "symbol entry " + k + " survived collection");
                }
                assert.isTrue(set.has(keys[k]), "symbol value " + k + " survived collection");
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
