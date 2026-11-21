//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Q1.5 Wave 1 Slice 2: Proxy, Object, Reflect, Mapped Arguments & Reference ES2025 conformance.

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
    }
}

function assertSame(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error(msg + " (expected " + String(expected) + ", got " + String(actual) + ")");
    }
}

function assertArrayEq(actual, expected, msg) {
    if (actual.length !== expected.length) {
        throw new Error(msg + " (length " + actual.length + " !== " + expected.length + ")");
    }
    for (var i = 0; i < expected.length; i++) {
        if (!Object.is(actual[i], expected[i])) {
            throw new Error(msg + " (at index " + i + ": expected " + String(expected[i]) + ", got " + String(actual[i]) + ")");
        }
    }
}

function assertThrows(Ctor, fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof Ctor)) {
            throw new Error(msg + " (expected " + Ctor.name + ", got " + (e && e.constructor && e.constructor.name) + ": " + e + ")");
        }
        return;
    }
    if (!threw) {
        throw new Error(msg + " (did not throw)");
    }
}

function runAll() {
    // 1. Revoked proxy typeof
    var rpObj = Proxy.revocable({}, {});
    rpObj.revoke();
    assertSame(typeof rpObj.proxy, "object", "revoked object proxy typeof");

    var rpFn = Proxy.revocable(function () {}, {});
    rpFn.revoke();
    assertSame(typeof rpFn.proxy, "function", "revoked function proxy typeof");

    // 2. Proxy traps & invariants
    var delTarget = { prop: 1 };
    var delProxy = new Proxy(delTarget, {
        deleteProperty: function (t) {
            Object.preventExtensions(t);
            return true;
        }
    });
    assertThrows(TypeError, function () {
        Reflect.deleteProperty(delProxy, "prop");
    }, "deleteProperty configurable on non-extensible target");
    assertSame(Reflect.deleteProperty(delProxy, "missing"), true, "deleteProperty missing on non-extensible target");

    var capturedSet = null;
    var protoProxy = new Proxy({}, {
        set: function (t, p, v, r) {
            capturedSet = [this, t, p, v, r];
            return true;
        }
    });
    var arrProtoTest = new Array(1);
    Object.setPrototypeOf(arrProtoTest, protoProxy);
    arrProtoTest[0] = 42;
    assert(capturedSet !== null, "prototype proxy set trap called on array index");
    assertSame(capturedSet[2], "0", "array index key stringified to '0'");
    assertSame(capturedSet[3], 42, "array index value");
    assertSame(capturedSet[4], arrProtoTest, "receiver is array");

    var innerArr = [1, 2, 3];
    var nestedProxy = new Proxy(new Proxy(innerArr, {}), { set: null });
    nestedProxy.length = 0;
    assertSame(innerArr.length, 0, "forwarded length = 0");
    Object.preventExtensions(innerArr);
    assertSame(Reflect.set(nestedProxy, "foo", 2), false, "Reflect.set with null trap forwarding to non-extensible returns false");
    assertThrows(TypeError, function () {
        "use strict";
        nestedProxy[0] = 3;
    }, "strict set with null trap forwarding to non-extensible throws");

    var extCalled = 0;
    var extProxyTarget = new Proxy({}, {
        isExtensible: function () {
            extCalled++;
            return true;
        }
    });
    var setProtoFalseProxy = new Proxy(extProxyTarget, {
        setPrototypeOf: function () {
            return 0;
        }
    });
    assertSame(Reflect.setPrototypeOf(setProtoFalseProxy, {}), false, "setPrototypeOf falsy returns false");
    assertSame(extCalled, 0, "isExtensible not called when setPrototypeOf trap returns false");

    var nonExtTarget = {};
    Object.preventExtensions(nonExtTarget);
    var setProtoMismatchProxy = new Proxy(nonExtTarget, {
        setPrototypeOf: function () {
            return true;
        }
    });
    assertThrows(TypeError, function () {
        Reflect.setPrototypeOf(setProtoMismatchProxy, {});
    }, "Reflect.setPrototypeOf invariant throws TypeError");

    var defProxy = new Proxy({}, {
        defineProperty: function (t, p) {
            Object.defineProperty(t, p, { configurable: false, writable: true });
            return true;
        }
    });
    assertThrows(TypeError, function () {
        Reflect.defineProperty(defProxy, "x", { writable: false });
    }, "defineProperty invariant non-configurable writable vs non-writable");

    var gopdProxy = new Proxy({}, {
        getOwnPropertyDescriptor: function (t, p) {
            Object.defineProperty(t, p, { configurable: false, writable: true });
            return { configurable: false, writable: false };
        }
    });
    assertThrows(TypeError, function () {
        Object.getOwnPropertyDescriptor(gopdProxy, "x");
    }, "getOwnPropertyDescriptor invariant non-configurable non-writable vs target writable");

    var prevExtProxy = new Proxy(new Proxy({}, {
        preventExtensions: function () {
            return false;
        }
    }), {});
    assertSame(Reflect.preventExtensions(prevExtProxy), false, "Reflect.preventExtensions forwards through missing trap");

    // 3. Object, Object.prototype, Reflect, TypedArray lengths
    var protoDesc = Object.getOwnPropertyDescriptor(Object.prototype, "__proto__");
    assertSame(protoDesc.get.name, "get __proto__", "__proto__ getter name");
    assertSame(protoDesc.set.name, "set __proto__", "__proto__ setter name");

    var hint = null;
    assertThrows(RangeError, function () {
        Object.prototype.hasOwnProperty.call(null, {
            toString: function () {
                hint = "string";
                throw new RangeError("order");
            }
        });
    }, "hasOwnProperty converts key before ToObject");
    assertSame(hint, "string", "ToPropertyKey ran first");

    assertSame(Reflect.setPrototypeOf(Object.prototype, {}), false, "Reflect.setPrototypeOf on Object.prototype returns false");
    assertThrows(TypeError, function () {
        Object.setPrototypeOf(Object.prototype, {});
    }, "Object.setPrototypeOf on Object.prototype still throws");

    var revToStr = Proxy.revocable([], {
        get: function () {
            revToStr.revoke();
        }
    });
    assertSame(Object.prototype.toString.call(revToStr.proxy), "[object Array]", "toString checks IsArray before @@toStringTag");

    var falsePrev = new Proxy({}, { preventExtensions: function () { return false; } });
    assertThrows(TypeError, function () { Object.freeze(falsePrev); }, "Object.freeze throws on false");
    assertThrows(TypeError, function () { Object.seal(falsePrev); }, "Object.seal throws on false");

    var s1 = Symbol("a");
    var s2 = Symbol("b");
    var assignLog = [];
    var src = {};
    Object.defineProperty(src, s1, { get: function () { assignLog.push("s1"); }, enumerable: true });
    Object.defineProperty(src, "a", { get: function () { assignLog.push("a"); }, enumerable: true });
    Object.defineProperty(src, s2, { get: function () { assignLog.push("s2"); }, enumerable: true });
    Object.defineProperty(src, "b", { get: function () { assignLog.push("b"); }, enumerable: true });
    Object.assign({}, src);
    assertArrayEq(assignLog, ["a", "b", "s1", "s2"], "Object.assign strings before symbols");

    var re = /(?:)/g;
    re.a = 1;
    Object.defineProperty(re, "lastIndex", { value: 2 });
    assertArrayEq(Reflect.ownKeys(Object.getOwnPropertyDescriptors(re)), ["lastIndex", "a"], "RegExp ownKeys order");

    var arr = [];
    arr.a = 1;
    Object.defineProperty(arr, "length", { value: 2 });
    assertArrayEq(Object.getOwnPropertyNames(arr), ["length", "a"], "Array ownKeys order");

    var str = new String("");
    str.a = 1;
    str.b = 2;
    Object.defineProperty(str, "a", { get: function () {} });
    assertArrayEq(Reflect.ownKeys(str), ["length", "a", "b"], "String ownKeys order");

    var tagDesc = Object.getOwnPropertyDescriptor(Reflect, Symbol.toStringTag);
    assertSame(tagDesc.value, "Reflect", "Reflect[@@toStringTag]");
    assertSame(tagDesc.writable, false, "Reflect[@@toStringTag].writable");
    assertSame(tagDesc.enumerable, false, "Reflect[@@toStringTag].enumerable");
    assertSame(tagDesc.configurable, true, "Reflect[@@toStringTag].configurable");

    var recvNonWritable = {};
    Object.defineProperty(recvNonWritable, "p", { value: 1, writable: false });
    assertSame(Reflect.set({}, "p", 2, recvNonWritable), false, "Reflect.set returns false for non-writable receiver");
    assertSame(recvNonWritable.p, 1, "non-writable receiver unchanged");

    // 4. Mapped arguments non-configurable delete
    (function (a) {
        var args = arguments;
        Object.defineProperty(args, "0", { configurable: false });
        assertSame(delete args[0], false, "sloppy delete non-configurable mapped argument");
        assertSame(delete args["0"], false, "sloppy string delete non-configurable mapped argument");
        assertSame(Reflect.deleteProperty(args, "0"), false, "Reflect.deleteProperty non-configurable mapped argument");
        assertThrows(TypeError, function () {
            "use strict";
            delete args[0];
        }, "strict delete non-configurable mapped argument");
        a = 10;
        assertSame(args[0], 10, "mapping preserved after failed delete (a -> arguments)");
        args[0] = 20;
        assertSame(a, 20, "mapping preserved after failed delete (arguments -> a)");
        Object.defineProperty(args, "0", { value: 30 });
        assertSame(a, 30, "mapping preserved after failed delete (defineProperty -> a)");
    })(1);

    // 5. With proxy env and primitive PutValue
    var withLog = [];
    var withProxy = new Proxy({ p: 0 }, {
        has: function (t, k) { withLog.push("has:" + String(k)); return Reflect.has(t, k); },
        get: function (t, k, r) { withLog.push("get:" + String(k)); return Reflect.get(t, k, r); },
        set: function (t, k, v, r) { withLog.push("set:" + String(k)); return Reflect.set(t, k, v, r); },
        getOwnPropertyDescriptor: function (t, k) { withLog.push("gopd:" + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
        defineProperty: function (t, k, d) { withLog.push("def:" + String(k)); return Reflect.defineProperty(t, k, d); }
    });
    with (withProxy) {
        p = 1;
    }
    assertArrayEq(withLog, [
        "has:p", "get:Symbol(Symbol.unscopables)", "has:p",
        "set:p", "gopd:p", "def:p"
    ], "with proxy assignment trap log");

    var primCalls = 0;
    var primSpy = new Proxy({}, {
        set: function () {
            primCalls++;
            return true;
        }
    });
    var savedNumProto = Object.getPrototypeOf(Number.prototype);
    var savedStrProto = Object.getPrototypeOf(String.prototype);
    var savedBoolProto = Object.getPrototypeOf(Boolean.prototype);
    var savedSymProto = Object.getPrototypeOf(Symbol.prototype);
    Object.setPrototypeOf(Number.prototype, primSpy);
    Object.setPrototypeOf(String.prototype, primSpy);
    Object.setPrototypeOf(Boolean.prototype, primSpy);
    Object.setPrototypeOf(Symbol.prototype, primSpy);
    try {
        0..q15prop = 1;
        "".q15prop = 1;
        true.q15prop = 1;
        Symbol().q15prop = 1;
        assertSame(primCalls, 4, "primitive PutValue hits prototype proxy set trap");
    } finally {
        Object.setPrototypeOf(Number.prototype, savedNumProto);
        Object.setPrototypeOf(String.prototype, savedStrProto);
        Object.setPrototypeOf(Boolean.prototype, savedBoolProto);
        Object.setPrototypeOf(Symbol.prototype, savedSymProto);
    }

    if (typeof WScript !== "undefined" && WScript.LoadScript) {
        var other = WScript.LoadScript("", "samethread");
        var realmCalls = 0;
        var realmSpy = new Proxy({}, {
            set: function () {
                realmCalls++;
                return true;
            }
        });
        Object.setPrototypeOf(other.Number.prototype, realmSpy);
        Object.setPrototypeOf(other.String.prototype, realmSpy);
        Object.setPrototypeOf(other.Boolean.prototype, realmSpy);
        Object.setPrototypeOf(other.Symbol.prototype, realmSpy);
        other.eval("0..q15prop = 1; ''.q15prop = 1; true.q15prop = 1; Symbol().q15prop = 1;");
        assertSame(realmCalls, 4, "cross-realm primitive PutValue hits prototype proxy set trap");
    }
}

for (var iter = 0; iter < 5; iter++) {
    runAll();
}
print("PASS");
