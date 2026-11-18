//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function fail(msg) {
    throw new Error("FAIL: " + msg);
}

function assertSameValue(actual, expected, msg) {
    if (!Object.is(actual, expected)) {
        fail((msg || "assertSameValue") + ": expected " + String(expected) + " (neg0=" + Object.is(expected, -0) + "), got " + String(actual) + " (neg0=" + Object.is(actual, -0) + ")");
    }
}

function assertArrayEquals(actual, expected, msg) {
    if (actual.length !== expected.length) {
        fail((msg || "assertArrayEquals") + ": length mismatch " + actual.length + " !== " + expected.length);
    }
    for (var i = 0; i < actual.length; i++) {
        if (!Object.is(actual[i], expected[i])) {
            fail((msg || "assertArrayEquals") + ": index " + i + " mismatch: " + actual[i] + " !== " + expected[i]);
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

// 1. TimeClip -0 normalization
(function testTimeClipNegativeZero() {
    var inputs = [-0, -0.5, -1.23e-15, -1e-300];
    for (var i = 0; i < inputs.length; i++) {
        var d = new Date(inputs[i]);
        assertSameValue(d.getTime(), +0, "new Date(" + inputs[i] + ").getTime()");
        assertSameValue(d.valueOf(), +0, "new Date(" + inputs[i] + ").valueOf()");

        var dSet = new Date(12345);
        assertSameValue(dSet.setTime(inputs[i]), +0, "setTime(" + inputs[i] + ") return");
        assertSameValue(dSet.getTime(), +0, "setTime(" + inputs[i] + ") stored");
    }

    assertSameValue(Date.UTC(1970, 0, 1, 0, 0, 0, -0), +0, "Date.UTC -0");
    assertSameValue(Date.UTC(1970, 0, 1, -0, -0, -0, -0.5), +0, "Date.UTC all -0");

    var dUtc = new Date(0);
    assertSameValue(dUtc.setUTCMilliseconds(-0), +0, "setUTCMilliseconds(-0)");
    assertSameValue(dUtc.getTime(), +0, "setUTCMilliseconds(-0) stored");
    assertSameValue(dUtc.setUTCSeconds(0, -0), +0, "setUTCSeconds(0, -0)");

    var dLocal = new Date(1970, 0, 1, 0, 0, 0, -0);
    if (Object.is(dLocal.getTime(), -0)) {
        fail("new Date(1970,0,1,0,0,0,-0) stored -0");
    }
})();

// 2. setTime TimeClip overflow & special values
(function testSetTimeClip() {
    var maxMs = 8.64e15;
    var cases = [maxMs + 1, -maxMs - 1, Infinity, -Infinity, NaN, undefined, 1e20, -1e20];
    for (var i = 0; i < cases.length; i++) {
        var d = new Date(123456);
        var ret = d.setTime(cases[i]);
        assertSameValue(ret, NaN, "setTime(" + cases[i] + ") return");
        assertSameValue(d.getTime(), NaN, "setTime(" + cases[i] + ") stored");
    }

    var dNoArg = new Date(123456);
    assertSameValue(dNoArg.setTime(), NaN, "setTime() return");
    assertSameValue(dNoArg.getTime(), NaN, "setTime() stored");

    var dMax = new Date(0);
    assertSameValue(dMax.setTime(maxMs), maxMs, "setTime(8.64e15)");
    assertSameValue(dMax.getTime(), maxMs, "setTime(8.64e15) stored");

    var dMin = new Date(0);
    assertSameValue(dMin.setTime(-maxMs), -maxMs, "setTime(-8.64e15)");
    assertSameValue(dMin.getTime(), -maxMs, "setTime(-8.64e15) stored");
})();

// 3. valueOf() mutating `this` on invalid Date for non-FullYear/non-Year methods
(function testInvalidDateReadBeforeToNumber() {
    var methods = [
        "setMonth", "setDate", "setHours", "setMinutes", "setSeconds", "setMilliseconds",
        "setUTCMonth", "setUTCDate", "setUTCHours", "setUTCMinutes", "setUTCSeconds", "setUTCMilliseconds"
    ];
    for (var i = 0; i < methods.length; i++) {
        var m = methods[i];
        var dt = new Date(NaN);
        var calls = 0;
        var ret = dt[m]({
            valueOf: function () {
                calls++;
                dt.setTime(42);
                return 1;
            }
        });
        assertSameValue(calls, 1, m + " invalid->valid calls");
        assertSameValue(ret, NaN, m + " invalid->valid return NaN");
        assertSameValue(dt.getTime(), 42, m + " invalid->valid preserves dt.setTime(42)");

        // Even when coerced arg is NaN, must NOT overwrite dt.setTime(42) with NaN!
        var dt2 = new Date(NaN);
        var ret2 = dt2[m]({
            valueOf: function () {
                dt2.setTime(42);
                return NaN;
            }
        });
        assertSameValue(ret2, NaN, m + " invalid->valid NaN arg return NaN");
        assertSameValue(dt2.getTime(), 42, m + " invalid->valid NaN arg preserves dt.setTime(42)");
    }
})();

// 4. valueOf() mutating `this` on invalid Date for setFullYear / setUTCFullYear / setYear
(function testInvalidDateFullYearAndSetYear() {
    // setFullYear uses +0F base (Jan 1 00:00:00.000 local) even if valueOf mutates dt.setTime(...)
    var dt = new Date(NaN);
    var ret = dt.setFullYear({
        valueOf: function () {
            dt.setTime(Date.UTC(2020, 5, 15, 12, 30, 45, 500));
            return 2025;
        }
    });
    var expected = new Date(2025, 0, 1, 0, 0, 0, 0).getTime();
    assertSameValue(ret, expected, "setFullYear invalid uses +0F base return");
    assertSameValue(dt.getTime(), expected, "setFullYear invalid uses +0F base stored");

    // setUTCFullYear uses +0F base (Jan 1 00:00:00.000 UTC)
    var dtUtc = new Date(NaN);
    var retUtc = dtUtc.setUTCFullYear({
        valueOf: function () {
            dtUtc.setTime(Date.UTC(2020, 5, 15, 12, 30, 45, 500));
            return 2025;
        }
    });
    var expectedUtc = Date.UTC(2025, 0, 1, 0, 0, 0, 0);
    assertSameValue(retUtc, expectedUtc, "setUTCFullYear invalid uses +0F base return");
    assertSameValue(dtUtc.getTime(), expectedUtc, "setUTCFullYear invalid uses +0F base stored");

    // setYear uses +0F base (Jan 1 00:00:00.000 local)
    var dtYear = new Date(NaN);
    var retYear = dtYear.setYear({
        valueOf: function () {
            dtYear.setTime(Date.UTC(2020, 5, 15, 12, 30, 45, 500));
            return 95;
        }
    });
    var expectedYear = new Date(1995, 0, 1, 0, 0, 0, 0).getTime();
    assertSameValue(retYear, expectedYear, "setYear invalid uses +0F base return");
    assertSameValue(dtYear.getTime(), expectedYear, "setYear invalid uses +0F base stored");

    // When coerced arg is NaN for setFullYear/setUTCFullYear/setYear, dt.[[DateValue]] becomes NaN (overwriting valueOf mutation)
    var fyMethods = ["setFullYear", "setUTCFullYear", "setYear"];
    for (var i = 0; i < fyMethods.length; i++) {
        var fm = fyMethods[i];
        var dNan = new Date(NaN);
        var rNan = dNan[fm]({
            valueOf: function () {
                dNan.setTime(0);
                return NaN;
            }
        });
        assertSameValue(rNan, NaN, fm + " invalid + NaN arg returns NaN");
        assertSameValue(dNan.getTime(), NaN, fm + " invalid + NaN arg overwrites with NaN");
    }
})();

// 5. valueOf() mutating `this` on valid Date across all 15 methods (including setTime(NaN) & setTime(other))
(function testValidDateReadBeforeToNumber() {
    var baseTv = Date.UTC(2005, 4, 17, 13, 27, 41, 619);
    var mutators = [NaN, Date.UTC(1990, 0, 1, 0, 0, 0, 0)];
    var specs = [
        ["setFullYear", [2012]],
        ["setFullYear", [2012, 8]],
        ["setUTCFullYear", [2012]],
        ["setUTCFullYear", [2012, 8]],
        ["setYear", [99]],
        ["setMonth", [9]],
        ["setUTCMonth", [9]],
        ["setDate", [22]],
        ["setUTCDate", [22]],
        ["setHours", [6]],
        ["setHours", [6, 15]],
        ["setUTCHours", [6]],
        ["setUTCHours", [6, 15]],
        ["setMinutes", [44]],
        ["setUTCMinutes", [44]],
        ["setSeconds", [12]],
        ["setUTCSeconds", [12]],
        ["setMilliseconds", [333]],
        ["setUTCMilliseconds", [333]]
    ];

    for (var mIdx = 0; mIdx < mutators.length; mIdx++) {
        var mutVal = mutators[mIdx];
        for (var i = 0; i < specs.length; i++) {
            var method = specs[i][0];
            var rawArgs = specs[i][1];

            var ref = new Date(baseTv);
            var expected = ref[method].apply(ref, rawArgs);

            var dt = new Date(baseTv);
            var mutatingArgs = rawArgs.map(function (v, idx) {
                if (idx === 0) {
                    return {
                        valueOf: function () {
                            dt.setTime(mutVal);
                            return v;
                        }
                    };
                }
                return v;
            });
            var actual = dt[method].apply(dt, mutatingArgs);
            assertSameValue(actual, expected, method + " mut=" + mutVal + " return");
            assertSameValue(dt.getTime(), expected, method + " mut=" + mutVal + " stored");
        }
    }

    // Valid Date with NaN coerced arg overwrites valueOf's dt.setTime(1000) with NaN
    var dValid = new Date(baseTv);
    var rValid = dValid.setDate({
        valueOf: function () {
            dValid.setTime(1000);
            return NaN;
        }
    });
    assertSameValue(rValid, NaN, "valid Date + NaN arg returns NaN");
    assertSameValue(dValid.getTime(), NaN, "valid Date + NaN arg overwrites with NaN");
})();

// 6. Multi-arg left-to-right ToNumber order (without NaN/Infinity short-circuit) and arity bounds
(function testMultiArgCoercionOrder() {
    function makeLogArg(log, label, val) {
        return {
            valueOf: function () {
                log.push(label);
                return val;
            }
        };
    }

    var methodArities = [
        ["setFullYear", 3],
        ["setUTCFullYear", 3],
        ["setYear", 1],
        ["setMonth", 2],
        ["setUTCMonth", 2],
        ["setDate", 1],
        ["setUTCDate", 1],
        ["setHours", 4],
        ["setUTCHours", 4],
        ["setMinutes", 3],
        ["setUTCMinutes", 3],
        ["setSeconds", 2],
        ["setUTCSeconds", 2],
        ["setMilliseconds", 1],
        ["setUTCMilliseconds", 1]
    ];

    var badFirstVals = [NaN, Infinity, -Infinity, 1];
    var initialTimes = [0, NaN];

    for (var tIdx = 0; tIdx < initialTimes.length; tIdx++) {
        var initTv = initialTimes[tIdx];
        for (var vIdx = 0; vIdx < badFirstVals.length; vIdx++) {
            var firstVal = badFirstVals[vIdx];
            for (var i = 0; i < methodArities.length; i++) {
                var name = methodArities[i][0];
                var arity = methodArities[i][1];
                var log = [];
                var callArgs = [];
                var expectedLog = [];
                for (var k = 0; k < arity; k++) {
                    var label = "arg" + k;
                    expectedLog.push(label);
                    callArgs.push(makeLogArg(log, label, k === 0 ? firstVal : NaN));
                }
                // Extra argument beyond spec arity must NOT be coerced.
                callArgs.push(makeLogArg(log, "extra", 0));

                var dt = new Date(initTv);
                var res = dt[name].apply(dt, callArgs);
                assertArrayEquals(log, expectedLog, name + " init=" + initTv + " first=" + firstVal);
                var isYearSetter = name === "setFullYear" || name === "setUTCFullYear" || name === "setYear";
                var allArgsFinite = Number.isFinite(firstVal) && arity === 1;
                if (!Number.isFinite(firstVal) || (Number.isNaN(initTv) && !(isYearSetter && allArgsFinite)) || (!Number.isNaN(initTv) && arity > 1)) {
                    assertSameValue(res, NaN, name + " non-finite result");
                } else {
                    if (Number.isNaN(res)) {
                        fail(name + " expected finite result for init=" + initTv + " first=" + firstVal);
                    }
                }
            }
        }
    }

    // Abrupt exception in second arg stops third arg coercion and leaves Date unchanged
    var dThrow = new Date(55555);
    var thirdCalled = 0;
    assertThrows(function () {
        dThrow.setHours(
            { valueOf: function () { return NaN; } },
            { valueOf: function () { throw new RangeError("boom"); } },
            { valueOf: function () { thirdCalled++; return 0; } }
        );
    }, RangeError, "setHours throws on 2nd arg even when 1st is NaN");
    assertSameValue(thirdCalled, 0, "3rd arg not coerced after 2nd throws");
    assertSameValue(dThrow.getTime(), 55555, "Date unchanged after throw");

    // Zero arguments on valid and invalid Date
    for (var j = 0; j < methodArities.length; j++) {
        var mName = methodArities[j][0];
        var d1 = new Date(10000);
        assertSameValue(d1[mName](), NaN, mName + "() on valid return");
        assertSameValue(d1.getTime(), NaN, mName + "() on valid stored");

        var d2 = new Date(NaN);
        assertSameValue(d2[mName](), NaN, mName + "() on invalid return");
        assertSameValue(d2.getTime(), NaN, mName + "() on invalid stored");
    }
})();

// 7. Reentrancy + GC stress across SetDateData
(function testGcDuringValueOf() {
    var base = Date.UTC(2020, 2, 15, 10, 20, 30, 400);
    for (var i = 0; i < 200; i++) {
        var dt = new Date(base);
        var res = dt.setUTCHours(
            {
                valueOf: function () {
                    var garbage = [];
                    for (var g = 0; g < 100; g++) {
                        garbage.push({ x: g, s: "str" + g });
                    }
                    dt.setTime(NaN);
                    return 5;
                }
            },
            {
                valueOf: function () {
                    dt.setTime(123456);
                    return 25;
                }
            }
        );
        assertSameValue(res, Date.UTC(2020, 2, 15, 5, 25, 30, 400), "GC loop return " + i);
        assertSameValue(dt.getTime(), res, "GC loop stored " + i);
    }
})();

print("PASS");
