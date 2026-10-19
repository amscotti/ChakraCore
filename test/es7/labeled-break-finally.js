//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// A break/continue targeting a labeled statement nested inside a try must not
// emit Leave for the enclosing try. Doing so ran the finally early and left the
// try scope a second time, corrupting the interpreter's resume offset (SIGSEGV).
// Expected values below were checked against Node.js.

function evalCompletion(source) {
    return eval(source);
}

var tests = [
    {
        name: "labeled break out of a nested try-finally",
        body() {
            assert.areEqual(undefined, evalCompletion("1; try { L: { try { break L; } finally {} } } finally {}"),
                "inner try-finally exits, outer try-finally does not");
            assert.areEqual(undefined, evalCompletion("1; L: { try { break L; } finally {} }"),
                "label outside the try still runs its finally");
            assert.areEqual(undefined, evalCompletion("1; L: { try { break L; } finally { 4; } }"),
                "normal finally value is discarded");
            assert.areEqual(3, evalCompletion("1; L: { try { 3; break L; } finally {} }"),
                "try value survives the break cleanup");
            assert.areEqual(4, evalCompletion("1; try { L: { 2; try { 3; break L; } finally {} } 4; } finally {}"),
                "code after the labeled block still runs");
            assert.areEqual(undefined, evalCompletion("1; try { L: { try { throw 1; } catch (e) { break L; } finally {} } } finally {}"),
                "labeled break from a catch through the inner finally");
            assert.areEqual(undefined, evalCompletion("1; try { L1: { try { L2: { try { break L1; } finally {} } } finally {} } } finally {}"),
                "nested labels through nested finally blocks");
        }
    },
    {
        name: "ordinary function returns through labeled break and finally",
        body() {
            assert.areEqual(7, (function () {
                try { L: { try { break L; } finally {} } } finally {}
                return 7;
            })(), "labeled break through inner finally in a function");

            assert.areEqual(7, (function () {
                try { L: { try { throw 1; } catch (e) { break L; } finally {} } } finally {}
                return 7;
            })(), "labeled break from a catch through inner finally");

            assert.areEqual(7, (function () {
                try { L1: { try { L2: { try { break L1; } finally {} } } finally {} } } finally {}
                return 7;
            })(), "nested labels through nested finally blocks");
        }
    },
    {
        name: "finally executes exactly once on labeled exits",
        body() {
            assert.areEqual("1,2,3,4", (function () {
                var log = [];
                try {
                    L: {
                        try { log.push(1); break L; } finally { log.push(2); }
                    }
                    log.push(3);
                } finally {
                    log.push(4);
                }
                return log.join(",");
            })(), "break through try-finally, then outer finally");

            assert.areEqual("1,2,3,4", (function () {
                var log = [];
                try {
                    L: {
                        try { throw 0; } catch (e) { log.push(1); break L; } finally { log.push(2); }
                    }
                    log.push(3);
                } finally {
                    log.push(4);
                }
                return log.join(",");
            })(), "break from catch through try-finally");

            assert.areEqual("1,2,3,4", (function () {
                var log = [];
                L: {
                    try { try { log.push(1); break L; } finally { log.push(2); } }
                    finally { log.push(3); }
                }
                log.push(4);
                return log.join(",");
            })(), "label outside nested try-finally");

            assert.areEqual("after,outer", (function () {
                var log = [];
                try {
                    L: {
                        try { try { throw 1; } finally { break L; } }
                        catch (e) { log.push("catch"); }
                    }
                    log.push("after");
                } finally {
                    log.push("outer");
                }
                return log.join(",");
            })(), "break from an inner finally leaves the outer finally intact");
        }
    },
    {
        name: "break from a finally to a label inside the enclosing try",
        body() {
            assert.areEqual(undefined, evalCompletion("1; try { L: { try {} finally { break L; } } } finally {}"),
                "inner finally breaks to a label inside the outer try");
            assert.areEqual(5, evalCompletion("1; try { L: { try {} finally { break L; } } 5; } finally {}"),
                "execution continues after the labeled block");
            assert.areEqual(undefined, evalCompletion("1; try { L: { try { 2; } finally { break L; } } } finally {}"),
                "the finally's break discards the try value");
        }
    },
    {
        name: "labeled break and continue through finally in loops",
        body() {
            assert.areEqual(36, (function () {
                var n = 0;
                outer: for (var i = 0; i < 3; i++) {
                    try {
                        for (var j = 0; j < 3; j++) {
                            n++;
                            if (j === 1) continue outer;
                        }
                    } finally {
                        n += 10;
                    }
                }
                return n;
            })(), "continue outer through a finally");

            assert.areEqual("0:0,f0,1:0,f1", (function () {
                var log = [];
                outer: for (var i = 0; i < 2; i++) {
                    try {
                        for (var j = 0; j < 2; j++) {
                            if (j === 1) continue outer;
                            log.push(i + ":" + j);
                        }
                    } finally {
                        log.push("f" + i);
                    }
                }
                return log.join(",");
            })(), "continue outer runs the inner finally once per iteration");

            assert.areEqual(3, evalCompletion("1; do { 2; try { 3; break; } finally { 4; } } while (false)"),
                "break out of a loop through try-finally keeps the try value");
        }
    },
    {
        name: "existing return-through-finally completion is unchanged",
        body() {
            assert.areEqual(2, (function () { try { return 2; } finally { 3; } })(),
                "finally does not replace return");
            assert.areEqual(2, (function () { try { try { return 2; } finally { 3; } } finally { 4; } })(),
                "nested finally does not replace return");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
