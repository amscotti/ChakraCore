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
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
