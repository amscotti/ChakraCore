//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function forInKeys(object) {
    var keys = [];
    for (var key in object) {
        keys.push(key);
    }
    return keys;
}

function entryKeys(object) {
    return Object.entries(object).map(function (entry) {
        return entry[0];
    });
}

var tests = [
    {
        name: "A deleted and re-added property is enumerated at its new insertion position",
        body() {
            var object = { p1: "p1", p2: "p2", p3: "p3" };
            object.p4 = "p4";
            object[2] = "2";
            object[0] = "0";
            object[1] = "1";
            delete object.p1;
            delete object.p3;
            object.p1 = "p1";

            var expected = ["0", "1", "2", "p2", "p4", "p1"];
            assert.areEqual(expected, Object.keys(object), "Object.keys follows creation order");
            assert.areEqual(expected, forInKeys(object), "for-in follows creation order");
            assert.areEqual(expected, Object.values(object), "Object.values follows creation order");
            assert.areEqual(expected, entryKeys(object), "Object.entries follows creation order");
            assert.areEqual(
                '{"0":"0","1":"1","2":"2","p2":"p2","p4":"p4","p1":"p1"}',
                JSON.stringify(object),
                "JSON.stringify follows creation order");
        }
    },
    {
        name: "Integer keys enumerate ascending and deleted integer keys are re-inserted among them",
        body() {
            var object = {};
            object.b = "b";
            object[10] = "10";
            object.a = "a";
            object[2] = "2";
            object[1] = "1";
            delete object[10];
            object[10] = "ten";
            delete object.a;
            object.a = "A";

            var expected = ["1", "2", "10", "b", "a"];
            assert.areEqual(expected, Object.keys(object), "integer indices precede strings");
            assert.areEqual(["1", "2", "ten", "b", "A"], Object.values(object), "values stay with their keys");
            assert.areEqual(expected, forInKeys(object), "for-in uses the same order");
            assert.areEqual(
                '{"1":"1","2":"2","10":"ten","b":"b","a":"A"}',
                JSON.stringify(object),
                "JSON.stringify uses the same order");
        }
    },
    {
        name: "Overwriting a property does not move it and preserves its descriptor",
        body() {
            var object = { a: 1, b: 2 };
            object.a = 3;
            assert.areEqual(["a", "b"], Object.keys(object), "overwrite keeps the original position");

            var descriptor = Object.getOwnPropertyDescriptor(object, "a");
            assert.areEqual(3, descriptor.value, "overwritten value");
            assert.isTrue(descriptor.writable, "value remains writable");
            assert.isTrue(descriptor.enumerable, "value remains enumerable");
            assert.isTrue(descriptor.configurable, "value remains configurable");

            delete object.b;
            object.b = 4;
            assert.areEqual(["a", "b"], Object.keys(object), "delete/re-add moves the key to the end");
            descriptor = Object.getOwnPropertyDescriptor(object, "b");
            assert.areEqual(4, descriptor.value, "re-added value");
            assert.isTrue(descriptor.writable && descriptor.enumerable && descriptor.configurable,
                "re-added property has default data-property attributes");
        }
    },
    {
        name: "Defining an accessor as data keeps creation order, while delete/re-add moves it",
        body() {
            var object = {};
            Object.defineProperty(object, "a", {
                get: function () { return "get"; },
                set: function (value) { },
                enumerable: true,
                configurable: true
            });
            object.b = "b";

            Object.defineProperty(object, "a", { value: 1 });
            assert.areEqual(["a", "b"], Object.keys(object), "redefinition is not a new insertion");
            var descriptor = Object.getOwnPropertyDescriptor(object, "a");
            assert.areEqual(1, descriptor.value, "accessor converted to data");
            assert.isTrue(descriptor.enumerable, "enumerable flag preserved");

            delete object.a;
            object.a = 2;
            assert.areEqual(["b", "a"], Object.keys(object), "delete/re-add is a new insertion");
        }
    },
    {
        name: "for-in shadowing is unaffected by delete/re-add",
        body() {
            var prototype = { x: "proto-x", y: "proto-y" };
            var object = Object.create(prototype);
            Object.defineProperty(object, "x", { value: "own-x", enumerable: false });
            object.y = "own-y";
            delete object.y;
            object.y = "readded-y";

            assert.areEqual(["y"], forInKeys(object), "non-enumerable own property shadows the prototype");
            assert.areEqual(["y"], Object.keys(object), "Object.keys only includes own enumerable properties");
            assert.areEqual(["readded-y"], Object.values(object), "shadowing property value");
            assert.areEqual('{"y":"readded-y"}', JSON.stringify(object), "JSON only includes own enumerable properties");
        }
    },
    {
        name: "Repeated delete/re-add preserves the most recent insertion order",
        body() {
            var object = { p1: 1, p2: 2, p3: 3 };
            delete object.p1;
            object.p1 = 1;
            delete object.p2;
            object.p2 = 2;
            delete object.p1;
            object.p1 = 1;

            assert.areEqual(["p3", "p2", "p1"], Object.keys(object), "last deletion determines the new position");
            assert.areEqual(["p3", "p2", "p1"], forInKeys(object), "for-in agrees");
        }
    },
    {
        name: "JSON.stringify does not enumerate a property added by a getter",
        body() {
            var object = { p1: "p1", p2: "p2", p3: "p3" };
            Object.defineProperty(object, "add", {
                enumerable: true,
                get: function () {
                    object.extra = "extra";
                    return "add";
                }
            });
            object.p4 = "p4";
            object[2] = "2";
            object[0] = "0";
            object[1] = "1";
            delete object.p1;
            delete object.p3;
            object.p1 = "p1";

            assert.areEqual(
                '{"0":"0","1":"1","2":"2","p2":"p2","add":"add","p4":"p4","p1":"p1"}',
                JSON.stringify(object),
                "snapshot keys exclude properties added during enumeration");
        }
    },
    {
        name: "for-in does not skip or duplicate when a visited key is deleted and re-added",
        body() {
            var object = { a: 1, b: 2, c: 3, d: 4 };
            Object.defineProperty(object, "x", {
                enumerable: true,
                configurable: true,
                get: function () { return 0; }
            });

            var seen = [];
            for (var key in object) {
                seen.push(key);
                if (key === "a") {
                    delete object.a;
                    object.a = 9;
                }
            }

            assert.areEqual(["a", "b", "c", "d", "x"], seen, "snapshot keys are visited once in order");
        }
    },
    {
        name: "for-in skips a key deleted before it is visited and keeps a re-added key once",
        body() {
            var deleted = { a: 1, b: 2, c: 3 };
            var seenDeleted = [];
            for (var key in deleted) {
                seenDeleted.push(key);
                if (key === "a") {
                    delete deleted.b;
                }
            }
            assert.areEqual(["a", "c"], seenDeleted, "deleted unvisited key is skipped");

            var reAdded = { a: 1, b: 2, c: 3 };
            var seenReAdded = [];
            for (var key in reAdded) {
                seenReAdded.push(key);
                if (key === "a") {
                    delete reAdded.b;
                    reAdded.b = 9;
                }
            }
            assert.areEqual(["a", "b", "c"], seenReAdded, "re-added unvisited key is still visited exactly once");

            var added = { a: 1, b: 2 };
            var seenAdded = [];
            for (var key in added) {
                seenAdded.push(key);
                if (key === "a") {
                    added.c = 3;
                }
            }
            assert.areEqual(["a", "b"], seenAdded, "property added during for-in is not visited");
        }
    },
    {
        name: "JSON.stringify snapshots keys when a getter deletes and re-adds a property",
        body() {
            var self = { a: 1, b: 2, c: 3 };
            Object.defineProperty(self, "a", {
                enumerable: true,
                configurable: true,
                get: function () {
                    delete self.a;
                    self.a = 9;
                    return "A";
                }
            });
            assert.areEqual('{"a":"A","b":2,"c":3}', JSON.stringify(self), "self re-add is not duplicated and b is kept");

            var other = { a: 1, b: 2, c: 3 };
            Object.defineProperty(other, "a", {
                enumerable: true,
                configurable: true,
                get: function () {
                    delete other.b;
                    other.b = 9;
                    return "A";
                }
            });
            assert.areEqual('{"a":"A","b":9,"c":3}', JSON.stringify(other), "other re-add uses its new value");

            var removed = { a: 1, b: 2, c: 3 };
            Object.defineProperty(removed, "a", {
                enumerable: true,
                configurable: true,
                get: function () {
                    delete removed.b;
                    return "A";
                }
            });
            assert.areEqual('{"a":"A","c":3}', JSON.stringify(removed), "removed key is skipped");
        }
    },
    {
        name: "Object.assign and spread snapshot keys while getters or target setters mutate",
        body() {
            var source = { a: 1, b: 2, c: 3 };
            Object.defineProperty(source, "a", {
                enumerable: true,
                configurable: true,
                get: function () {
                    delete source.a;
                    source.a = 9;
                    return "A";
                }
            });
            var assigned = {};
            Object.assign(assigned, source);
            assert.areEqual(["a", "b", "c"], Object.keys(assigned), "assign does not duplicate a or lose b");
            assert.areEqual(["A", 2, 3], Object.values(assigned), "assign uses the getter result and current values");

            var appendSource = { a: 1, b: 2, c: 3 };
            Object.defineProperty(appendSource, "a", {
                enumerable: true,
                configurable: true,
                get: function () {
                    delete appendSource.b;
                    appendSource.b = 9;
                    return "A";
                }
            });
            var spread = { ...appendSource };
            assert.areEqual(["a", "b", "c"], Object.keys(spread), "spread does not duplicate a or lose b");
            assert.areEqual(["A", 9, 3], Object.values(spread), "spread reads the current value");

            var setterSource = { a: 1, b: 2, c: 3 };
            var setterTarget = {};
            Object.defineProperty(setterTarget, "a", {
                enumerable: true,
                configurable: true,
                set: function () {
                    delete setterSource.c;
                }
            });
            Object.assign(setterTarget, setterSource);
            assert.areEqual(["a", "b"], Object.keys(setterTarget), "target setter mutation skips the removed source key");
            assert.areEqual(["a", "b"], Object.keys(setterSource), "source reflects the target setter mutation");
        }
    },
    {
        name: "Repeated delete/re-add reuses its slot and keeps the final key order",
        body() {
            var object = { k0: 0, pad: 1 };
            for (var i = 0; i < 2000; i++) {
                delete object.k0;
                object.k0 = i;
            }
            assert.areEqual(["pad", "k0"], Object.keys(object), "key order is stable");
            assert.areEqual(1999, object.k0, "final value");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
