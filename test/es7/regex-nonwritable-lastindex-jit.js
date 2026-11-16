//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Verify non-writable RegExp.lastIndex throws TypeError instead of being
// silently mutated across JIT GenerateFastInlineRegExpExec, RegexEs5ReplaceImpl,
// PropagateLastMatchToRegex, GetInitialOffset, and RegExp.prototype methods.

function assert(cond, msg) {
    if (!cond) {
        print("regex-nonwritable-lastindex-jit: FAIL: " + msg);
        WScript.Quit(1);
    }
}

function assertThrowsTypeError(fn, msg) {
    var threw = false;
    try {
        fn();
    } catch (e) {
        threw = (e instanceof TypeError);
        if (!threw) {
            assert(false, msg + " threw non-TypeError: " + e);
        }
    }
    assert(threw, msg + " did not throw TypeError");
}

// 1. JIT fast BOI literal exec (/^ab/g): short-string (< 2 chars), mismatch, and match.
function fastExec(re, s) {
    return re.exec(s);
}

function fastTest(re, s) {
    return re.test(s);
}

function testJitFastExec() {
    var warmupRe = /^ab/g;
    for (var i = 0; i < 20000; i++) {
        warmupRe.lastIndex = 0;
        fastExec(warmupRe, "x");
        warmupRe.lastIndex = 0;
        fastExec(warmupRe, "cd");
        warmupRe.lastIndex = 0;
        fastExec(warmupRe, "ab");
        warmupRe.lastIndex = 0;
        fastTest(warmupRe, "x");
        warmupRe.lastIndex = 0;
        fastTest(warmupRe, "ab");
    }

    // Short string (< 2 chars) bailout must throw TypeError and preserve lastIndex.
    var rShort = /^ab/g;
    Object.defineProperty(rShort, "lastIndex", { value: 3, writable: false });
    assertThrowsTypeError(function () { fastExec(rShort, "x"); }, "JIT BOI exec < 2 chars");
    assert(rShort.lastIndex === 3, "JIT BOI exec < 2 chars mutated lastIndex");
    assertThrowsTypeError(function () { fastTest(rShort, "x"); }, "JIT BOI test < 2 chars");
    assert(rShort.lastIndex === 3, "JIT BOI test < 2 chars mutated lastIndex");

    // Mismatch (len >= 2) bailout must throw TypeError and preserve lastIndex.
    var rMismatch = /^ab/g;
    Object.defineProperty(rMismatch, "lastIndex", { value: 5, writable: false });
    assertThrowsTypeError(function () { fastExec(rMismatch, "cd"); }, "JIT BOI exec mismatch");
    assert(rMismatch.lastIndex === 5, "JIT BOI exec mismatch mutated lastIndex");
    assertThrowsTypeError(function () { fastTest(rMismatch, "cd"); }, "JIT BOI test mismatch");
    assert(rMismatch.lastIndex === 5, "JIT BOI test mismatch mutated lastIndex");

    // Match from lastIndex === 0 must throw TypeError when updating lastIndex to 2.
    var rMatch = /^ab/g;
    Object.defineProperty(rMatch, "lastIndex", { value: 0, writable: false });
    assertThrowsTypeError(function () { fastExec(rMatch, "ab"); }, "JIT BOI exec match");
    assert(rMatch.lastIndex === 0, "JIT BOI exec match mutated lastIndex");
    assertThrowsTypeError(function () { fastTest(rMatch, "ab"); }, "JIT BOI test match");
    assert(rMatch.lastIndex === 0, "JIT BOI test match mutated lastIndex");

    // Non-global, non-sticky regex with non-writable lastIndex must NOT throw and
    // must keep lastIndex intact.
    var rNonGlobal = /^ab/;
    Object.defineProperty(rNonGlobal, "lastIndex", { value: 7, writable: false });
    for (var j = 0; j < 20000; j++) {
        var m1 = fastExec(rNonGlobal, "ab");
        assert(m1 !== null && m1[0] === "ab", "non-global BOI exec failed");
        assert(fastExec(rNonGlobal, "x") === null, "non-global BOI short failed");
        assert(rNonGlobal.lastIndex === 7, "non-global BOI mutated lastIndex");
    }
}

// 2. String.prototype.replace / match / search paths (RegexEs5ReplaceImpl & PropagateLastMatchToRegex).
function jitReplace(str, re, rep) {
    return str.replace(re, rep);
}

function testHelperLastIndexChecks() {
    var warmGlobal = /a/g;
    for (var i = 0; i < 20000; i++) {
        jitReplace("aba", warmGlobal, "x");
    }

    var rgMatch = /a/g;
    Object.defineProperty(rgMatch, "lastIndex", { value: 2, writable: false });
    assertThrowsTypeError(function () { jitReplace("aba", rgMatch, "x"); }, "replace global match");
    assert(rgMatch.lastIndex === 2, "replace global match mutated lastIndex");

    var rgNoMatch = /z/g;
    Object.defineProperty(rgNoMatch, "lastIndex", { value: 4, writable: false });
    assertThrowsTypeError(function () { jitReplace("aba", rgNoMatch, "x"); }, "replace global nomatch");
    assert(rgNoMatch.lastIndex === 4, "replace global nomatch mutated lastIndex");

    // Sticky regex match & mismatch via exec/test/match/replace
    var rsMatch = /a/y;
    Object.defineProperty(rsMatch, "lastIndex", { value: 0, writable: false });
    assertThrowsTypeError(function () { rsMatch.exec("aba"); }, "sticky exec match");
    assert(rsMatch.lastIndex === 0, "sticky exec match mutated lastIndex");

    var rsNoMatch = /z/y;
    Object.defineProperty(rsNoMatch, "lastIndex", { value: 1, writable: false });
    assertThrowsTypeError(function () { rsNoMatch.exec("aba"); }, "sticky exec nomatch");
    assert(rsNoMatch.lastIndex === 1, "sticky exec nomatch mutated lastIndex");

    var rsReplace = /a/y;
    Object.defineProperty(rsReplace, "lastIndex", { value: 0, writable: false });
    assertThrowsTypeError(function () { jitReplace("aba", rsReplace, "x"); }, "sticky replace");
    assert(rsReplace.lastIndex === 0, "sticky replace mutated lastIndex");

    // Non-global, non-sticky regex in replace must NOT mutate or throw on lastIndex.
    var rng = /a/;
    Object.defineProperty(rng, "lastIndex", { value: 6, writable: false });
    assert(jitReplace("aba", rng, "x") === "xba", "non-global replace failed");
    assert(rng.lastIndex === 6, "non-global replace mutated lastIndex");

    // RegExp.prototype.compile must also throw TypeError on non-writable lastIndex.
    if (typeof RegExp.prototype.compile === "function") {
        var rCompile = /a/g;
        Object.defineProperty(rCompile, "lastIndex", { value: 3, writable: false });
        assertThrowsTypeError(function () { rCompile.compile("b", "g"); }, "compile with non-writable lastIndex");
        assert(rCompile.lastIndex === 3, "compile mutated non-writable lastIndex");
    }
}

testJitFastExec();
testHelperLastIndexChecks();

print("pass");
