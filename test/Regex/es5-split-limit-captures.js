//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

function assertArr(actual, expected, msg) {
    if (actual.length !== expected.length) {
        throw new Error(msg + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
    }
    for (var i = 0; i < expected.length; i++) {
        if (actual[i] !== expected[i]) {
            throw new Error(msg + " at " + i + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
        }
    }
}

// RV-08 / IH-6: String.prototype.split / RegExp[Symbol.split] must stop
// immediately when limit is reached inside the capture push loop.
assertArr("a,b,c".split(/(,)/, 0), [], "limit=0");
assertArr("a,b,c".split(/(,)/, 1), ["a"], "limit=1");
assertArr("a,b,c".split(/(,)/, 2), ["a", ","], "limit=2 stops after capture");
assertArr("a,b,c".split(/(,)/, 3), ["a", ",", "b"], "limit=3");
assertArr("a,b,c".split(/(,)/, 4), ["a", ",", "b", ","], "limit=4");
assertArr("a,b,c".split(/(,)/, 5), ["a", ",", "b", ",", "c"], "limit=5");

// Undefined optional capture group reaching limit.
assertArr("abc".split(/(x)|/, 2), ["a", undefined], "empty match + undefined capture at limit=2");
assertArr("abc".split(/(x)|/, 3), ["a", undefined, "b"], "empty match + undefined capture at limit=3");

// Multiple capture groups where limit lands mid-capture-loop.
assertArr("a,:b,:c".split(/(,)(:)/, 2), ["a", ","], "multi-capture limit=2");
assertArr("a,:b,:c".split(/(,)(:)/, 3), ["a", ",", ":"], "multi-capture limit=3");
assertArr("a,:b,:c".split(/(,)(:)/, 4), ["a", ",", ":", "b"], "multi-capture limit=4");

// RegExp.$1 propagation still updates when stopping inside capture loop.
RegExp.input = "";
"x-y-z".split(/(-)/, 2);
if (RegExp.$1 !== "-") {
    throw new Error("Expected RegExp.$1 to be '-', got " + RegExp.$1);
}

// Direct Symbol.split invocation (both default fast path and ES6 subclass path).
if (typeof Symbol !== "undefined" && Symbol.split && RegExp.prototype[Symbol.split]) {
    assertArr(/(,)/[Symbol.split]("a,b,c", 2), ["a", ","], "Symbol.split fast path limit=2");
    class SubRegExp extends RegExp {}
    assertArr(new SubRegExp("(,)")[Symbol.split]("a,b,c", 2), ["a", ","], "Symbol.split subclass limit=2");
}

print("pass");
