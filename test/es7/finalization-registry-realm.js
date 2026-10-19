//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// GetFunctionRealm follows Proxy targets and bound-function targets even when
// the Proxy/bound object itself was created in the current realm. The target
// function and %FinalizationRegistry.prototype% belong to a same-thread child
// realm, so the fallback must use the target's realm, not the Proxy/bound
// object's creation context.

var realm = WScript.LoadScript(
    "function MakeRealmFunction() {" +
    "  function RealmFunction() {}" +
    "  RealmFunction.prototype = undefined;" +
    "  return RealmFunction;" +
    "}" +
    "function GetRealmPrototype() { return FinalizationRegistry.prototype; }",
    "samethread");

var realmFunction = realm.MakeRealmFunction();
var realmPrototype = realm.GetRealmPrototype();
var failures = [];
function check(condition, message) {
    if (!condition) {
        failures.push(message);
        print("finalization-registry-realm: FAIL: " + message);
        WScript.Quit(1);
    }
}

function checkRealmNewTarget(name, newTarget) {
    var registry = Reflect.construct(FinalizationRegistry, [function () {}], newTarget);
    check(Object.getPrototypeOf(registry) === realmPrototype, name);
}

checkRealmNewTarget("realm function newTarget", realmFunction);
checkRealmNewTarget("bound newTarget", Function.prototype.bind.call(realmFunction, null));
checkRealmNewTarget("proxy newTarget", new Proxy(realmFunction, {}));
checkRealmNewTarget("nested proxy newTarget", new Proxy(new Proxy(realmFunction, {}), {}));
checkRealmNewTarget("bound proxy newTarget", Function.prototype.bind.call(new Proxy(realmFunction, {}), null));
checkRealmNewTarget("proxy bound newTarget", new Proxy(Function.prototype.bind.call(realmFunction, null), {}));

// An object-valued NewTarget.prototype still wins over the realm fallback.
var customPrototype = { marker: "custom" };
function CurrentRealmCtor() {}
CurrentRealmCtor.prototype = customPrototype;
var customNewTarget = new Proxy(CurrentRealmCtor, {});
var customRegistry = Reflect.construct(FinalizationRegistry, [function () {}], customNewTarget);
check(Object.getPrototypeOf(customRegistry) === customPrototype, "object prototype wins");

check(failures.length === 0, "no recorded failures");
print("pass");
