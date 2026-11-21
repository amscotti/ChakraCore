//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(msg) {
    throw new Error("FAIL: " + msg);
}

function assertSameValue(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        fail((msg || "assertSameValue") + ": expected " + String(expected) + ", got " + String(actual));
    }
}

function assertArrayEquals(actual, expected, msg) {
    if (actual.length !== expected.length) {
        fail((msg || "assertArrayEquals") + ": length " + actual.length + " !== " + expected.length);
    }
    for (var i = 0; i < actual.length; i++) {
        if (!Object.is(actual[i], expected[i])) {
            fail((msg || "assertArrayEquals") + ": [" + i + "] " + actual[i] + " !== " + expected[i]);
        }
    }
}

function assertThrows(fn, ErrorCtor, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (ErrorCtor && !(e instanceof ErrorCtor)) {
            fail((msg || "assertThrows") + ": expected " + ErrorCtor.name + ", got " + e);
        }
    }
    if (!threw) {
        fail((msg || "assertThrows") + ": did not throw");
    }
}

function maybeGC() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

// 1. String.prototype.{indexOf,includes,startsWith,endsWith} SymbolObject ToString coercion
(function testStringSearchToStringErrors() {
    var symObj = Object(Symbol("1"));
    var toPrimSym = {};
    toPrimSym[Symbol.toPrimitive] = function () { return Symbol("1"); };

    for (var i = 0; i < 30; i++) {
        assertThrows(function () { "".indexOf(Symbol("1")); }, TypeError, "indexOf(Symbol)");
        assertThrows(function () { "".indexOf(symObj); }, TypeError, "indexOf(Object(Symbol))");
        assertThrows(function () { "".indexOf(toPrimSym); }, TypeError, "indexOf(@@toPrimitive->Symbol)");
        assertThrows(function () { "".includes(symObj); }, TypeError, "includes(Object(Symbol))");
        assertThrows(function () { "".startsWith(symObj); }, TypeError, "startsWith(Object(Symbol))");
        assertThrows(function () { "".endsWith(symObj); }, TypeError, "endsWith(Object(Symbol))");
    }
})();

// 2. Date.UTC IEEE-754 intermediate rounding in MakeTime / MakeDate
(function testDateUTCFpOrder() {
    for (var i = 0; i < 30; i++) {
        assertSameValue(
            Date.UTC(1970, 0, 1, 80063993375, 29, 1, -288230376151711740),
            29312,
            "MakeTime order"
        );
        assertSameValue(
            Date.UTC(1970, 0, 213503982336, 0, 0, 0, -18446744073709552000),
            34447360,
            "MakeDate precision"
        );
    }
})();

// 4. Date.parse TimeClip maximum range
(function testDateParseRange() {
    var minStr = "-271821-04-20T00:00:00.000Z";
    var maxStr = "+275760-09-13T00:00:00.000Z";
    var belowStr = "-271821-04-19T23:59:59.999Z";
    var aboveStr = "+275760-09-13T00:00:00.001Z";

    for (var i = 0; i < 20; i++) {
        assertSameValue(Date.parse(minStr), -8640000000000000, "min boundary");
        assertSameValue(Date.parse(maxStr), 8640000000000000, "max boundary");
        assertSameValue(Date.parse(belowStr), NaN, "below min");
        assertSameValue(Date.parse(aboveStr), NaN, "above max");
        assertSameValue(Date.parse("+999999-01-01T00:00:00.000Z"), NaN, "far future");
        assertSameValue(Date.parse("-999999-01-01T00:00:00.000Z"), NaN, "far past");
    }
})();

// 5. JSON.stringify replacer sees properties deleted during stringification (with GC)
(function testJsonStringifyDeletedPropertyReplacer() {
    for (var i = 0; i < 30; i++) {
        var obj = {
            get a() {
                delete this.b;
                maybeGC();
                return 1;
            },
            b: 2
        };
        var seenKeys = [];
        var res = JSON.stringify(obj, function (key, value) {
            seenKeys.push(key);
            if (key === "b") {
                assertSameValue(value, undefined, "deleted property passes undefined");
                maybeGC();
                return "<replaced>";
            }
            return value;
        });
        assertSameValue(res, '{"a":1,"b":"<replaced>"}', "plain obj stringify");
        assertArrayEquals(seenKeys, ["", "a", "b"], "plain obj keys");

        // Dictionary-mode object + numeric property + prototype shadow
        var proto = { c: 99 };
        var dictObj = Object.create(proto);
        Object.defineProperty(dictObj, "0", {
            enumerable: true,
            configurable: true,
            get: function () {
                delete this[1];
                return 10;
            }
        });
        dictObj[1] = 20;
        Object.defineProperty(dictObj, "a", {
            enumerable: true,
            configurable: true,
            get: function () {
                delete this.b;
                delete this.c;
                return 1;
            }
        });
        dictObj.b = 2;
        dictObj.c = 3;
        dictObj.extra = 4;
        delete dictObj.extra;

        var res2 = JSON.stringify(dictObj, function (key, value) {
            if (key === "1") {
                assertSameValue(value, undefined, "dict deleted numeric 1");
                return "Z";
            }
            if (key === "b") {
                assertSameValue(value, undefined, "dict deleted b");
                return "B";
            }
            return value;
        });
        assertSameValue(res2, '{"0":10,"1":"Z","a":1,"b":"B","c":99}', "dict obj stringify");
    }
})();

// 6. Math.hypot coerces all arguments before returning Infinity / NaN
(function testMathHypotOrder() {
    for (var i = 0; i < 30; i++) {
        var counter = 0;
        assertThrows(function () {
            Math.hypot(
                Infinity,
                -Infinity,
                NaN,
                0,
                -0,
                { valueOf: function () { throw new RangeError("boom"); } },
                { valueOf: function () { counter++; return 1; } }
            );
        }, RangeError, "hypot ToNumber abrupt completion");
        assertSameValue(counter, 0, "counter untouched after abrupt");

        var seen = [];
        var infRes = Math.hypot(
            NaN,
            { valueOf: function () { seen.push("a"); return Infinity; } },
            { valueOf: function () { seen.push("b"); return 3; } }
        );
        assertSameValue(infRes, Infinity, "Infinity after NaN still returns Infinity");
        assertArrayEquals(seen, ["a", "b"], "all args coerced");
    }
})();

// 7. decodeURIComponent / decodeURI reject overlong UTF-8 sequences
(function testDecodeUriOverlong() {
    var invalid = [
        "%C0%AF",
        "%C1%BF",
        "%E0%80%AF",
        "%E0%9F%BF",
        "%F0%80%80%AF",
        "%F0%8F%BF%BF",
        "%ED%A0%80",
        "%ED%BF%BF",
        "%ED%7F%BF",
        "%ED%BF",
        "%F4%90%80%80"
    ];
    for (var round = 0; round < 10; round++) {
        for (var i = 0; i < invalid.length; i++) {
            (function (seq) {
                assertThrows(function () { decodeURIComponent(seq); }, URIError, "decodeURIComponent " + seq);
                assertThrows(function () { decodeURI(seq); }, URIError, "decodeURI " + seq);
            })(invalid[i]);
        }
        assertSameValue(decodeURIComponent("%C2%80"), "\u0080", "min 2-byte");
        assertSameValue(decodeURIComponent("%E0%A0%80"), "\u0800", "min 3-byte");
        assertSameValue(decodeURIComponent("%F0%90%80%80"), "\uD800\uDC00", "min 4-byte");
        assertSameValue(decodeURIComponent("%F4%8F%BF%BF"), "\uDBFF\uDFFF", "max 4-byte");
    }
})();

// 8. RegExp.$1..$9 writable & value descriptor consistency
(function testRegExpConstructorWritable() {
    /(x)/.exec("x");
    assertSameValue(RegExp.$1, "x", "RegExp.$1 populated");

    assertSameValue(
        Reflect.defineProperty(RegExp, "$1", { writable: false, configurable: false }),
        true,
        "defineProperty writable:false succeeds"
    );
    var desc1 = Reflect.getOwnPropertyDescriptor(RegExp, "$1");
    assertSameValue(desc1.writable, false, "$1 reports non-writable");
    assertSameValue(desc1.configurable, false, "$1 remains non-configurable");
    assertSameValue(desc1.value, "x", "$1 retains value");

    // Subsequent regex match must not mutate frozen $1, while non-frozen $2 still updates
    /(y)(z)/.exec("yz");
    var desc2 = Reflect.getOwnPropertyDescriptor(RegExp, "$1");
    assertSameValue(desc2.value, "x", "frozen $1 value unchanged after exec");
    assertSameValue(RegExp.$2, "z", "non-frozen $2 still updates");
    assertSameValue(
        Reflect.defineProperty(RegExp, "$1", { writable: true }),
        false,
        "cannot make frozen $1 writable again"
    );
})();

// 9. RegExp.prototype[Symbol.split] limit side effect & RegExp.prototype.compile TypeError
(function testRegExpSplitAndCompile() {
    for (var i = 0; i < 30; i++) {
        var re = /a/;
        var limit = {
            valueOf: function () {
                re.compile("b");
                maybeGC();
                return -1;
            }
        };
        var result = re[Symbol.split]("abba", limit);
        assertArrayEquals(result, ["", "bb", ""], "split ignores compile during limit ToUint32");

        // Also test capturing group + RegExp.$1 propagation when recompiled during limit
        var reCap = /(a)/;
        var limitCap = {
            valueOf: function () {
                reCap.compile("(b)");
                maybeGC();
                return -1;
            }
        };
        var capResult = reCap[Symbol.split]("aba", limitCap);
        assertArrayEquals(capResult, ["", "a", "b", "a", ""], "split capture ignores compile");
        assertSameValue(RegExp.lastMatch, "a", "RegExp.lastMatch safe after recompile during split");
    }

    var reCompile = /./;
    reCompile.lastIndex = 23;
    var badFlags = [null, 0, "", false, {}, []];
    for (var k = 0; k < badFlags.length; k++) {
        (function (f) {
            assertThrows(function () { reCompile.compile(reCompile, f); }, TypeError, "compile throws TypeError");
        })(badFlags[k]);
    }
    assertSameValue(reCompile.lastIndex, 23, "lastIndex preserved on TypeError");
    reCompile.compile(reCompile, undefined);
})();

maybeGC();
print("PASS");
