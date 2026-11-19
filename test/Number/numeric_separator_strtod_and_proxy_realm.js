//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assert(cond, msg) {
    if (!cond) {
        throw new Error("Assertion failed: " + msg);
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
        return e;
    }
    if (!threw) {
        throw new Error(msg + " (did not throw)");
    }
}

// 1. StrToDbl numeric-separator scaling (> 20 significant digits + '_')
assert(9007199254740992_000_000_000 === 9007199254740992000000000, "25-digit integer with separators");
assert(9007199254740991_000_000_000 === 9007199254740991000000000, "25-digit integer 9007199254740991e9");
assert(12345678901234567890_123456.789_012 === 12345678901234567890123456.789012, "32-digit decimal with separators");
assert(12345678901234567890_000_000.789_012 === 12345678901234567890000000.789012, "20th-digit zero + separator tail");
assert(12345678901234567890_999.999_999 === 12345678901234567890999.999999, "30-digit decimal rounding with separators");
assert(1234567890123456789_0.1_2_3_4 === 12345678901234567890.1234, "separator and dot near 20-digit boundary");
assert(1234567890123456789_1_2 === 123456789012345678912, "21-digit with separator before last digit");
assert(1_2_3_4_5_6_7_8_9_0_1_2_3_4_5_6_7_8_9_0_1_2_3_4 === 123456789012345678901234, "separators between every digit");
assert(12345678901234567890_000_000.789_012e-6 === 12345678901234567890000000.789012e-6, "decimal + separators + negative exponent");
assert(9007199254740992_000_000_000e-9 === 9007199254740992, "25-digit + e-9");
assert(1_234_567_890_123_456_789_012_345e1_0 === 1234567890123456789012345e10, "separators in mantissa and exponent");
assert(.0_1e2 === .01e2, "leading fractional zero with separator (.0_1e2)");
assert(.00_01e2 === .0001e2, "leading fractional zeros with separator (.00_01e2)");
assert(0.000_123 === 0.000123, "leading fractional zeros with separator (0.000_123)");
assert(0.000_000_12345678901234567890_123 === 0.00000012345678901234567890123, "leading zeros + >20 sig digits with separators");
assert(0x1_0000_0000_0000_0001 === 0x10000000000000001, "hex literal with separators");
assert(0b1_0000_0000 === 0b100000000, "binary literal with separators");
assert(0o7_7_7 === 0o777, "octal literal with separators");
assert(Number.isNaN(Number("9007199254740992_000_000_000")), "Number(string) still rejects numeric separators");

if (typeof Intl !== "undefined" && typeof Intl.DurationFormat === "function") {
    var df = new Intl.DurationFormat();
    assertThrows(RangeError, function () {
        df.format({ seconds: 9007199254740992_000_000_000 });
    }, "DurationFormat seconds out of range with separators");
    assertThrows(RangeError, function () {
        df.format({ seconds: 9007199254740992000000000 });
    }, "DurationFormat seconds out of range without separators");
    assertThrows(RangeError, function () {
        df.format({ nanoseconds: 9007199254740992_000_000_000 });
    }, "DurationFormat nanoseconds 2^53 throws RangeError");
    assert(typeof df.format({ nanoseconds: 9007199254740991_000_000_000 }) === "string",
        "DurationFormat nanoseconds 9007199254740991e9 must not overflow due to separator scaling");
    assert(typeof df.format({ nanoseconds: 9007199254740991000000000 }) === "string",
        "DurationFormat nanoseconds 9007199254740991e9 without separators");
}

// 2. Cross-realm revoked Proxy [[Construct]] TypeError realm + prototype lookup
var other = WScript.LoadScript("", "samethread");

var h1 = other.Proxy.revocable(function () {}, {
    get: function () {
        h1.revoke();
    }
});
var f1 = h1.proxy;
assert(typeof f1 === "function", "revocable proxy over function is typeof function");
var err1 = assertThrows(TypeError, function () {
    new f1();
}, "new revoked-during-get cross-realm proxy throws caller-realm TypeError");
assert(!(err1 instanceof other.TypeError), "err1 must not be other.TypeError");

var h2 = other.Proxy.revocable(function () {}, {
    get: function () {
        h2.revoke();
        return 42;
    }
});
var wrapped = new other.Proxy(h2.proxy, {});
var err2 = assertThrows(TypeError, function () {
    Reflect.construct(function () {}, [], wrapped);
}, "Reflect.construct with nested proxy revoked in get throws caller-realm TypeError");
assert(!(err2 instanceof other.TypeError), "err2 must not be other.TypeError");

var h3 = other.Proxy.revocable(function () {}, {});
var bound = h3.proxy.bind();
Object.defineProperty(bound, "prototype", {
    get: function () {
        h3.revoke();
    }
});
var err3 = assertThrows(TypeError, function () {
    Reflect.construct(function () {}, [], bound);
}, "Reflect.construct with bound(proxy) revoked in prototype getter throws caller-realm TypeError");
assert(!(err3 instanceof other.TypeError), "err3 must not be other.TypeError");

var protoObj = { tag: 42 };
var h4 = other.Proxy.revocable(function () { this.ok = 1; }, {
    get: function (t, p) {
        if (p === "prototype") {
            h4.revoke();
            return protoObj;
        }
    }
});
var inst4 = new h4.proxy();
assert(inst4.tag === 42 && inst4.ok === 1, "revoking in get trap while returning object prototype succeeds");
assert(Object.getPrototypeOf(inst4) === protoObj, "instance inherits returned object prototype");

var OtherCtor = other.eval("(function OtherCtor() { this.fromOther = true; })");
OtherCtor.prototype.kind = "otherProto";
var p5 = new other.Proxy(OtherCtor, {});
var inst5 = new p5();
assert(inst5.fromOther === true && inst5.kind === "otherProto", "non-revoked cross-realm constructor works");
assert(Object.getPrototypeOf(inst5) === OtherCtor.prototype, "inherits OtherCtor.prototype");

var OtherCtorNoProto = other.eval("(function OtherCtorNoProto() { this.v = 7; })");
OtherCtorNoProto.prototype = null;
var p6 = new other.Proxy(OtherCtorNoProto, {});
var inst6 = new p6();
assert(inst6.v === 7, "non-revoked cross-realm constructor with null prototype works");
assert(Object.getPrototypeOf(inst6) === other.Object.prototype, "falls back to other.Object.prototype");

var p7 = new Proxy(OtherCtorNoProto, {});
var inst7 = new p7();
assert(inst7.v === 7 && Object.getPrototypeOf(inst7) === other.Object.prototype,
    "main-realm proxy over other-realm ctor with null prototype marshals other.Object.prototype");

print("PASS");
