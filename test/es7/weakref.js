//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function makeObjectWeakRefs(count) {
    var refs = [];
    for (var i = 0; i < count; i++) {
        refs.push(new WeakRef({ index: i }));
    }
    return refs;
}

function makeSymbolWeakRefs(count) {
    var refs = [];
    for (var i = 0; i < count; i++) {
        refs.push(new WeakRef(Symbol("weakref-drop-" + i)));
    }
    return refs;
}

function countCleared(refs) {
    var cleared = 0;
    for (var i = 0; i < refs.length; i++) {
        if (refs[i].deref() === undefined) {
            cleared++;
        }
    }
    return cleared;
}

var tests = [
    {
        name: "WeakRef has the specified constructor and prototype shape",
        body() {
            assert.areEqual("function", typeof WeakRef, "WeakRef is a function");
            assert.areEqual(1, WeakRef.length, "WeakRef.length is 1");
            assert.areEqual("WeakRef", WeakRef.name, "WeakRef.name is WeakRef");

            var target = {};
            var weakRef = new WeakRef(target);
            assert.isTrue(weakRef instanceof WeakRef, "instance is a WeakRef");
            assert.areEqual(WeakRef.prototype, Object.getPrototypeOf(weakRef), "default prototype");
            assert.isTrue(Object.isExtensible(weakRef), "instances are extensible");
            assert.isFalse(Object.prototype.hasOwnProperty.call(weakRef, "target"), "no own target property");

            var descriptor = Object.getOwnPropertyDescriptor(WeakRef, "prototype");
            assert.isFalse(descriptor.writable, "WeakRef.prototype is not writable");
            assert.isFalse(descriptor.enumerable, "WeakRef.prototype is not enumerable");
            assert.isFalse(descriptor.configurable, "WeakRef.prototype is not configurable");

            descriptor = Object.getOwnPropertyDescriptor(WeakRef.prototype, "deref");
            assert.isTrue(descriptor.writable, "deref is writable");
            assert.isFalse(descriptor.enumerable, "deref is not enumerable");
            assert.isTrue(descriptor.configurable, "deref is configurable");
            assert.areEqual(0, WeakRef.prototype.deref.length, "deref.length is 0");
            assert.areEqual("deref", WeakRef.prototype.deref.name, "deref.name is deref");

            descriptor = Object.getOwnPropertyDescriptor(WeakRef.prototype, Symbol.toStringTag);
            assert.areEqual("WeakRef", descriptor.value, "toStringTag value");
            assert.isFalse(descriptor.writable, "toStringTag is not writable");
            assert.isFalse(descriptor.enumerable, "toStringTag is not enumerable");
            assert.isTrue(descriptor.configurable, "toStringTag is configurable");
        }
    },
    {
        name: "WeakRef rejects calls without new and targets that cannot be held weakly",
        body() {
            assert.throws(function () { WeakRef({}); }, TypeError, "call without new throws");
            assert.throws(function () { WeakRef.call(null, {}); }, TypeError, "call with this throws");
            assert.throws(function () { new WeakRef(); }, TypeError, "implicit undefined target throws");
            assert.throws(function () { new WeakRef(undefined); }, TypeError, "undefined target throws");
            assert.throws(function () { new WeakRef(null); }, TypeError, "null target throws");
            assert.throws(function () { new WeakRef(1); }, TypeError, "number target throws");
            assert.throws(function () { new WeakRef("object"); }, TypeError, "string target throws");
            assert.throws(function () { new WeakRef(true); }, TypeError, "boolean target throws");
            assert.throws(function () { new WeakRef(Symbol.for("weakref-registered")); }, TypeError, "registered symbol target throws");
        }
    },
    {
        name: "deref returns the exact live object, symbol, and wrapper target",
        body() {
            var objectTarget = {};
            var objectWeakRef = new WeakRef(objectTarget);
            CollectGarbage();
            CollectGarbage();
            assert.isTrue(objectWeakRef.deref() === objectTarget, "object target identity survives collection");
            assert.isTrue(objectWeakRef.deref() === objectTarget, "object target identity is stable across derefs");

            var symbolTarget = Symbol("weakref-live-symbol");
            var symbolWeakRef = new WeakRef(symbolTarget);
            CollectGarbage();
            CollectGarbage();
            assert.isTrue(symbolWeakRef.deref() === symbolTarget, "symbol target identity survives collection");
            assert.isTrue(new WeakRef(Symbol.hasInstance).deref() === Symbol.hasInstance, "well-known symbol target");

            var symbolObjectTarget = Object(Symbol("weakref-wrapper"));
            var wrapperWeakRef = new WeakRef(symbolObjectTarget);
            assert.isTrue(wrapperWeakRef.deref() === symbolObjectTarget, "symbol wrapper is an ordinary object target");
        }
    },
    {
        name: "deref brand-checks its receiver",
        body() {
            var deref = WeakRef.prototype.deref;
            var weakRef = new WeakRef({});

            assert.throws(function () { deref.call(undefined); }, TypeError, "undefined receiver throws");
            assert.throws(function () { deref.call(null); }, TypeError, "null receiver throws");
            assert.throws(function () { deref.call(1); }, TypeError, "number receiver throws");
            assert.throws(function () { deref.call(Symbol()); }, TypeError, "symbol receiver throws");
            assert.throws(function () { deref.call({}); }, TypeError, "ordinary object receiver throws");
            assert.throws(function () { deref.call(WeakRef.prototype); }, TypeError, "prototype receiver throws");
            assert.throws(function () { deref.call(WeakRef); }, TypeError, "constructor receiver throws");
            assert.throws(function () { deref.call(new WeakMap()); }, TypeError, "WeakMap receiver throws");
            assert.throws(function () { deref.call(new WeakSet()); }, TypeError, "WeakSet receiver throws");
            assert.isTrue(deref.call(weakRef) !== undefined, "valid receiver returns its live target");

            assert.throws(function () { new WeakRef.prototype.deref(); }, TypeError, "deref is not a constructor");
        }
    },
    {
        name: "unreachable object and symbol targets are cleared by collection",
        body() {
            // Only the WeakRef instances keep these targets observable. The
            // helper frames have returned, so an implementation that stores a
            // strong target (or fails to trace the weak handle) leaves every
            // deref non-undefined and fails this check.
            var objectRefs = makeObjectWeakRefs(2000);
            var symbolRefs = makeSymbolWeakRefs(2000);

            // The same object must be cleared when it is also a WeakMap key:
            // the WeakRef observes that the WeakMap's weak key path did not
            // start retaining the object.
            var weakMap = new WeakMap();
            var weakMapRefs = [];
            for (var i = 0; i < 1000; i++) {
                var key = { index: i };
                weakMap.set(key, i);
                weakMapRefs.push(new WeakRef(key));
            }

            CollectGarbage();
            CollectGarbage();

            var clearedObjects = countCleared(objectRefs);
            var clearedSymbols = countCleared(symbolRefs);
            var clearedWeakMapKeys = countCleared(weakMapRefs);
            assert.isTrue(clearedObjects > 0, "at least one unreachable object target was cleared (got " + clearedObjects + ")");
            assert.isTrue(clearedSymbols > 0, "at least one unreachable symbol target was cleared (got " + clearedSymbols + ")");
            assert.isTrue(clearedWeakMapKeys > 0, "at least one WeakMap object key was cleared through its WeakRef (got " + clearedWeakMapKeys + ")");
        }
    },
    {
        name: "WeakRef supports subclassing and Reflect.construct prototype selection",
        body() {
            var target = {};
            class SubWeakRef extends WeakRef {}
            var sub = new SubWeakRef(target);
            assert.isTrue(sub instanceof SubWeakRef, "subclass instance");
            assert.isTrue(sub instanceof WeakRef, "subclass instance is also a WeakRef");
            assert.isTrue(sub.deref() === target, "subclass deref returns target");

            var reflected = Reflect.construct(WeakRef, [target], Object);
            assert.areEqual(Object.prototype, Object.getPrototypeOf(reflected), "Reflect.construct uses Object.prototype");
            assert.isTrue(WeakRef.prototype.deref.call(reflected) === target, "Reflect.construct target is usable");

            var protoCalls = 0;
            var newTarget = function () {}.bind(null);
            Object.defineProperty(newTarget, "prototype", {
                get: function () {
                    protoCalls++;
                    throw new Error("prototype getter");
                }
            });
            assert.throws(function () {
                Reflect.construct(WeakRef, [target], newTarget);
            }, Error, "abrupt prototype getter propagates");
            assert.areEqual(1, protoCalls, "prototype getter is called once");
        }
    },
    {
        name: "WeakRef target identity coexists with WeakMap/WeakSet keys",
        body() {
            var target = {};
            var weakMap = new WeakMap();
            var weakSet = new WeakSet();
            var weakRef = new WeakRef(target);

            weakMap.set(target, "value");
            weakSet.add(target);

            assert.isTrue(weakRef.deref() === target, "WeakRef target remains identical");
            assert.areEqual("value", weakMap.get(target), "WeakMap entry remains present while target is live");
            assert.isTrue(weakSet.has(target), "WeakSet entry remains present while target is live");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
