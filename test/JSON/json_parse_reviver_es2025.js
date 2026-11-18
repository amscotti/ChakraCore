//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function gcNow() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function assertSame(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        throw new Error((msg || "assertSame") + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertTrue(cond, msg) {
    if (!cond) {
        throw new Error(msg || "assertion failed");
    }
}

function assertArrayEqual(actual, expected, msg) {
    assertSame(actual.length, expected.length, (msg || "array") + " length");
    for (var i = 0; i < expected.length; i++) {
        assertSame(actual[i], expected[i], (msg || "array") + "[" + i + "]");
    }
}

function assertThrows(ctor, fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof ctor)) {
            throw new Error((msg || "assertThrows") + ": expected " + ctor.name + ", got " + e);
        }
    }
    if (!threw) {
        throw new Error((msg || "assertThrows") + ": expected " + ctor.name + " to be thrown");
    }
}

function CustomError(msg) {
    this.message = msg;
}

// 1. Wrapper object is ordinary, extensible, and does not invoke Object.prototype[''] setter.
(function testWrapperObject() {
    Object.defineProperty(Object.prototype, "", {
        set: function () {
            throw new CustomError("prototype setter must not run for wrapper");
        },
        configurable: true
    });
    try {
        var wrapper;
        var out = JSON.parse("42", function (k, v) {
            wrapper = this;
            assertSame(k, "", "wrapper key");
            assertSame(v, 42, "wrapper value");
            return v + 1;
        });
        assertSame(out, 43, "wrapper return value");
        assertSame(Object.getPrototypeOf(wrapper), Object.prototype, "wrapper prototype");
        assertTrue(Object.isExtensible(wrapper), "wrapper extensible");
        var desc = Object.getOwnPropertyDescriptor(wrapper, "");
        assertSame(desc.value, 42, "wrapper desc.value");
        assertTrue(desc.writable && desc.enumerable && desc.configurable, "wrapper desc attrs");
    } finally {
        delete Object.prototype[""];
    }
})();

// 2. Prototype getters & deleted properties on arrays and objects (vs old DirectGetItem / GetProperty bugs).
(function testPrototypeWalksAndGetters() {
    // Array hole reading data and getter from Array.prototype.
    Array.prototype[1] = 30;
    try {
        var arr = JSON.parse("[10, 20]", function (k, v) {
            if (k === "0") {
                assertTrue(delete this[1], "delete array[1]");
            }
            return v;
        });
        assertSame(arr[0], 10, "arr[0]");
        assertTrue(Object.prototype.hasOwnProperty.call(arr, "1"), "arr[1] re-created as own data property");
        assertSame(arr[1], 30, "arr[1] revived from prototype");
    } finally {
        delete Array.prototype[1];
    }

    // Array getter on own element and on prototype must execute and propagate errors.
    assertThrows(CustomError, function () {
        JSON.parse("[1, 2]", function () {
            Object.defineProperty(this, "1", {
                get: function () {
                    throw new CustomError("own array getter");
                },
                configurable: true
            });
        });
    }, "own array getter throws");

    Object.defineProperty(Array.prototype, 1, {
        get: function () {
            throw new CustomError("proto array getter");
        },
        configurable: true
    });
    try {
        assertThrows(CustomError, function () {
            JSON.parse("[1, 2]", function (k, v) {
                if (k === "0") {
                    delete this[1];
                }
                return v;
            });
        }, "proto array getter throws");
    } finally {
        delete Array.prototype[1];
    }

    // Object property deleted during walk reads from Object.prototype (including getter).
    Object.prototype.b = 99;
    try {
        var obj = JSON.parse('{"a": 1, "b": 2}', function (k, v) {
            if (k === "a") {
                assertTrue(delete this.b, "delete obj.b");
            }
            return v;
        });
        assertSame(obj.a, 1, "obj.a");
        assertTrue(Object.prototype.hasOwnProperty.call(obj, "b"), "obj.b materialized");
        assertSame(obj.b, 99, "obj.b from prototype");
    } finally {
        delete Object.prototype.b;
    }

    // Converting an own configurable accessor to a data property via CreateDataProperty.
    var accessorCalls = 0;
    var objAcc = JSON.parse('{"a": 1, "b": 2}', function (k, v) {
        if (k === "a") {
            Object.defineProperty(this, "b", {
                get: function () {
                    accessorCalls++;
                    return 77;
                },
                set: function () {
                    throw new CustomError("CreateDataProperty must not invoke setter");
                },
                enumerable: true,
                configurable: true
            });
        }
        if (k === "b") {
            return v + 3;
        }
        return v;
    });
    assertSame(accessorCalls, 1, "accessor getter called once");
    var bDesc = Object.getOwnPropertyDescriptor(objAcc, "b");
    assertSame(bDesc.value, 80, "accessor replaced by data property");
    assertTrue(bDesc.writable && bDesc.enumerable && bDesc.configurable, "replaced data descriptor attrs");

    // Prototype setter must NOT be invoked by CreateDataProperty.
    Object.defineProperty(Object.prototype, "c", {
        set: function () {
            throw new CustomError("prototype setter must not run during CreateDataProperty");
        },
        configurable: true
    });
    try {
        var objProtoSet = JSON.parse('{"c": 5}', function (k, v) {
            return k === "c" ? 6 : v;
        });
        assertSame(objProtoSet.c, 6, "own c created without prototype setter");
    } finally {
        delete Object.prototype.c;
    }
})();

// 3. Proxy handling: IsArray, nested Proxy, object Proxy vs array Proxy, revoked Proxy.
(function testProxyHandling() {
    var arrayProxy = new Proxy([1, 2], {});
    arrayProxy.extra = 999;
    var nestedArrayProxy = new Proxy(arrayProxy, {});
    var seen = [];
    var injectedArrProxy = false;
    JSON.parse("[0, 0]", function (k, v) {
        if (k !== "") {
            seen.push(k);
        }
        if (!injectedArrProxy && k === "0") {
            injectedArrProxy = true;
            this[1] = nestedArrayProxy;
        }
        return v;
    });
    assertArrayEqual(seen, ["0", "0", "1", "1"], "nested array Proxy visits only array indices");

    var objectProxy = new Proxy({ length: 2, 0: "x", 1: "y", hidden: "z" }, {});
    Object.defineProperty(objectProxy, "hidden", { enumerable: false });
    var objSeen = [];
    var injectedObjProxy = false;
    JSON.parse("[0, 0]", function (k, v) {
        if (k !== "") {
            objSeen.push(k);
        }
        if (!injectedObjProxy && k === "0") {
            injectedObjProxy = true;
            this[1] = objectProxy;
        }
        return v;
    });
    assertArrayEqual(objSeen, ["0", "0", "1", "length", "1"], "object Proxy skips non-enumerable and visits enumerable keys");

    // Proxy ownKeys filtering Symbols & non-enumerable via getOwnPropertyDescriptor trap.
    var sym = Symbol("s");
    var trapLog = [];
    var customTarget = { a: 1, b: 2, [sym]: 3 };
    var customEnumProxy = new Proxy(customTarget, {
        ownKeys: function () {
            trapLog.push("ownKeys");
            return [sym, "b", "a"];
        },
        getOwnPropertyDescriptor: function (target, prop) {
            trapLog.push("gopd:" + String(prop));
            if (prop === "a") {
                return { value: 1, writable: true, enumerable: false, configurable: true };
            }
            return Reflect.getOwnPropertyDescriptor(target, prop);
        },
        get: function (target, prop) {
            trapLog.push("get:" + String(prop));
            return target[prop] * 10;
        },
        defineProperty: function (target, prop, desc) {
            trapLog.push("define:" + String(prop) + "=" + desc.value);
            return Reflect.defineProperty(target, prop, desc);
        }
    });
    var injectedCustomEnum = false;
    var resProxy = JSON.parse("[0, 0]", function (k, v) {
        if (!injectedCustomEnum && k === "0") {
            injectedCustomEnum = true;
            this[1] = customEnumProxy;
        }
        return v;
    });
    assertArrayEqual(trapLog, ["ownKeys", "gopd:b", "gopd:a", "get:b", "define:b=20"], "Proxy ownKeys + gopd + get + defineProperty order");
    assertSame(customTarget.b, 20, "Proxy updated underlying target property");

    // Revoked Proxy throws TypeError (both array and object targets, and nested).
    var revArr = Proxy.revocable([], {});
    revArr.revoke();
    assertThrows(TypeError, function () {
        JSON.parse("[0, 0]", function () {
            this[1] = revArr.proxy;
        });
    }, "revoked array proxy");

    var revObj = Proxy.revocable({}, {});
    revObj.revoke();
    assertThrows(TypeError, function () {
        JSON.parse("[0, 0]", function () {
            this[1] = revObj.proxy;
        });
    }, "revoked object proxy");

    var revNested = Proxy.revocable([], {});
    var nestedRevoked = new Proxy(revNested.proxy, {});
    revNested.revoke();
    assertThrows(TypeError, function () {
        JSON.parse("[0, 0]", function () {
            this[1] = nestedRevoked;
        });
    }, "nested revoked proxy");

    // Revoked callable Proxy as reviver itself.
    var revFn = Proxy.revocable(function (k, v) { return v; }, {});
    revFn.revoke();
    assertThrows(TypeError, function () {
        JSON.parse("1", revFn.proxy);
    }, "revoked proxy as reviver");
})();

// 4. Non-configurable create & delete ignore false return; Proxy trap abrupt completions & false returns.
(function testNonConfigurableAndProxyErrors() {
    // Array non-configurable create & delete ignore false.
    var arrNC = JSON.parse("[1, 2]", function (k, v) {
        if (k === "0") {
            Object.defineProperty(this, "1", { configurable: false });
        }
        if (k === "1") {
            return 222;
        }
        return v;
    });
    assertSame(arrNC[1], 2, "non-configurable array element create ignored");

    var arrNCDel = JSON.parse("[1, 2]", function (k, v) {
        if (k === "0") {
            Object.defineProperty(this, "1", { configurable: false });
        }
        if (k === "1") {
            return undefined;
        }
        return v;
    });
    assertTrue(Object.prototype.hasOwnProperty.call(arrNCDel, "1"), "non-configurable array element delete ignored");
    assertSame(arrNCDel[1], 2, "non-configurable array element value preserved");

    // Non-extensible array missing element create ignores false.
    var arrNonExt = JSON.parse("[1, 2]", function (k, v) {
        if (k === "0") {
            delete this[1];
            Object.preventExtensions(this);
        }
        return v;
    });
    assertTrue(!Object.prototype.hasOwnProperty.call(arrNonExt, "1"), "non-extensible array hole create ignored");

    // Object non-configurable create & delete ignore false.
    var objNC = JSON.parse('{"a": 1, "b": 2}', function (k, v) {
        if (k === "a") {
            Object.defineProperty(this, "b", { configurable: false });
        }
        if (k === "b") {
            return 222;
        }
        return v;
    });
    assertSame(objNC.b, 2, "non-configurable object property create ignored");

    var objNCDel = JSON.parse('{"a": 1, "b": 2}', function (k, v) {
        if (k === "a") {
            Object.defineProperty(this, "b", { configurable: false });
        }
        if (k === "b") {
            return undefined;
        }
        return v;
    });
    assertTrue(Object.prototype.hasOwnProperty.call(objNCDel, "b"), "non-configurable object property delete ignored");
    assertSame(objNCDel.b, 2, "non-configurable object property value preserved");

    // Proxy defineProperty / deleteProperty returning false must NOT throw.
    var falseDefineArr = new Proxy([10], {
        defineProperty: function () { return false; }
    });
    var injectedFalseDefine = false;
    var falseResArr = JSON.parse("[0, 0]", function (k, v) {
        if (!injectedFalseDefine && k === "0") {
            injectedFalseDefine = true;
            this[1] = falseDefineArr;
        }
        return v === 10 ? 99 : v;
    });
    assertSame(falseResArr[1][0], 10, "Proxy defineProperty returning false does not throw");

    var falseDeleteObj = new Proxy({ x: 1 }, {
        deleteProperty: function () { return false; }
    });
    var injectedFalseDelete = false;
    var falseResObj = JSON.parse("[0, 0]", function (k, v) {
        if (!injectedFalseDelete && k === "0") {
            injectedFalseDelete = true;
            this[1] = falseDeleteObj;
        }
        return k === "x" ? undefined : v;
    });
    assertSame(falseResObj[1].x, 1, "Proxy deleteProperty returning false does not throw");

    // Abrupt Proxy trap errors propagate.
    assertThrows(CustomError, function () {
        var badLen = new Proxy([], {
            get: function (_, p) {
                if (p === "length") {
                    throw new CustomError("length get");
                }
            }
        });
        JSON.parse("[0, 0]", function () { this[1] = badLen; });
    }, "array Proxy length get error");

    assertThrows(CustomError, function () {
        var badCoerce = new Proxy([], {
            get: function (_, p) {
                if (p === "length") {
                    return { valueOf: function () { throw new CustomError("length coerce"); } };
                }
            }
        });
        JSON.parse("[0, 0]", function () { this[1] = badCoerce; });
    }, "array Proxy length ToLength error");

    assertThrows(CustomError, function () {
        var badOwnKeys = new Proxy({}, {
            ownKeys: function () { throw new CustomError("ownKeys"); }
        });
        JSON.parse("[0, 0]", function () { this[1] = badOwnKeys; });
    }, "object Proxy ownKeys error");

    assertThrows(CustomError, function () {
        var badGopd = new Proxy({ a: 1 }, {
            getOwnPropertyDescriptor: function () { throw new CustomError("gopd"); }
        });
        JSON.parse("[0, 0]", function () { this[1] = badGopd; });
    }, "object Proxy gopd error");

    assertThrows(CustomError, function () {
        var badArrDef = new Proxy([1], {
            defineProperty: function () { throw new CustomError("arr define"); }
        });
        var injectedBadArrDef = false;
        JSON.parse("[0, 0]", function (k, v) {
            if (!injectedBadArrDef && k === "0") {
                injectedBadArrDef = true;
                this[1] = badArrDef;
            }
            return v;
        });
    }, "array Proxy defineProperty error");

    assertThrows(CustomError, function () {
        var badArrDel = new Proxy([1], {
            deleteProperty: function () { throw new CustomError("arr delete"); }
        });
        var injectedBadArrDel = false;
        JSON.parse("[0, 0]", function (k, v) {
            if (!injectedBadArrDel && k === "0") {
                injectedBadArrDel = true;
                this[1] = badArrDel;
                return v;
            }
            return k === "0" ? undefined : v;
        });
    }, "array Proxy deleteProperty error");

    assertThrows(CustomError, function () {
        var badObjDef = new Proxy({ a: 1 }, {
            defineProperty: function () { throw new CustomError("obj define"); }
        });
        var injectedBadObjDef = false;
        JSON.parse("[0, 0]", function (k, v) {
            if (!injectedBadObjDef && k === "0") {
                injectedBadObjDef = true;
                this[1] = badObjDef;
            }
            return v;
        });
    }, "object Proxy defineProperty error");

    assertThrows(CustomError, function () {
        var badObjDel = new Proxy({ a: 1 }, {
            deleteProperty: function () { throw new CustomError("obj delete"); }
        });
        var injectedBadObjDel = false;
        JSON.parse("[0, 0]", function (k, v) {
            if (!injectedBadObjDel && k === "0") {
                injectedBadObjDel = true;
                this[1] = badObjDel;
                return v;
            }
            return k === "a" ? undefined : v;
        });
    }, "object Proxy deleteProperty error");
})();

// 5. ToLength clamping, CopyOnAccess / NativeInt / NativeFloat arrays, and numeric key order.
(function testToLengthNativeArraysAndOrder() {
    // Negative length clamped to 0 by ToLength.
    var negLenProxy = new Proxy([1, 2], {
        get: function (t, p) {
            if (p === "length") {
                return -5;
            }
            return t[p];
        }
    });
    var negVisited = 0;
    var injectedNeg = false;
    JSON.parse("[0, 0]", function (k, v) {
        if (!injectedNeg && k === "0") {
            injectedNeg = true;
            this[1] = negLenProxy;
        } else if (this === negLenProxy) {
            negVisited++;
        }
        return v;
    });
    assertSame(negVisited, 0, "negative Proxy length clamped to 0");

    // NativeIntArray / CopyOnAccessNativeIntArray / NativeFloatArray injected during Walk.
    var injectedNative = false;
    var resNative = JSON.parse("[0, 0, 0]", function (k, v) {
        if (!injectedNative && k === "0") {
            injectedNative = true;
            this[1] = [1, 2, 3];
            this[2] = [1.5, 2.5];
            return v;
        }
        if (k === "1" && Array.isArray(this) && this[0] === 1) {
            return undefined;
        }
        if (k === "2" && Array.isArray(this) && this[0] === 1) {
            return "str";
        }
        if (k === "0" && Array.isArray(this) && this[1] === 2.5) {
            return 9.25;
        }
        return v;
    });
    assertTrue(!Object.prototype.hasOwnProperty.call(resNative[1], "1"), "native int array deleted element 1");
    assertSame(resNative[1][2], "str", "native int array converted for string element");
    assertSame(resNative[2][0], 9.25, "native float array updated");

    // ES2025 OwnPropertyKeys order & key snapshot semantics (added keys not visited).
    var order = [];
    var ordered = JSON.parse('{"b":1,"10":2,"2":3,"a":4}', function (k, v) {
        order.push(k);
        if (k === "2") {
            this.addedLater = 100;
        }
        return v;
    });
    assertArrayEqual(order, ["2", "10", "b", "a", ""], "OwnPropertyKeys order and snapshot");
    assertSame(ordered.addedLater, 100, "added property preserved");
})();

// 6. -CollectGarbage stress during recursive Walk with dynamic PropertyRecords and Proxies.
(function testGcStressDuringWalk() {
    var payload = [];
    for (var i = 0; i < 40; i++) {
        payload.push('{"k_' + i + '":' + i + ',"' + (i * 3) + '":' + (i + 100) + '}');
    }
    var jsonText = "[" + payload.join(",") + "]";
    var injectedGcProxy = false;
    var sawDynamicKeyVal;
    var out = JSON.parse(jsonText, function (k, v) {
        gcNow();
        if (!injectedGcProxy && k === "15") {
            injectedGcProxy = true;
            this.k_5 = new Proxy({ dynamicKey: 555 }, {
                ownKeys: function (t) {
                    gcNow();
                    return ["dynamic_" + "Key", "dynamicKey"];
                },
                getOwnPropertyDescriptor: function (t, p) {
                    gcNow();
                    if (p === "dynamic_Key" && !Object.prototype.hasOwnProperty.call(t, p)) {
                        return { value: 111, writable: true, enumerable: true, configurable: true };
                    }
                    return Reflect.getOwnPropertyDescriptor(t, p);
                },
                get: function (t, p) {
                    gcNow();
                    return (p === "dynamic_Key" && !Object.prototype.hasOwnProperty.call(t, p)) ? 111 : t[p];
                },
                defineProperty: function (t, p, d) {
                    gcNow();
                    return Reflect.defineProperty(t, p, d);
                }
            });
        }
        if (k === "dynamic_Key") {
            sawDynamicKeyVal = v;
            return 222;
        }
        if (k === "k_7") {
            return undefined;
        }
        return v;
    });
    assertSame(out.length, 40, "gc stress length");
    assertSame(sawDynamicKeyVal, 111, "gc stress saw dynamic_Key value");
    assertSame(out[5].k_5.dynamicKey, 555, "gc stress nested proxy preserved");
    assertSame(out[5].k_5.dynamic_Key, 222, "gc stress dynamic PropertyRecord preserved across GC");
    assertTrue(!Object.prototype.hasOwnProperty.call(out[7], "k_7"), "gc stress deleted k_7");
})();

print("PASS");
