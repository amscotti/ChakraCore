//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function parses(code) {
    try {
        eval("function test() {" + code + "}");
        return true;
    } catch (e) {
        return false;
    }
}

var tests = [
    {
        name: "Basic logical assignment on identifiers",
        body() {
            let x;
            x = 0; x ||= 5; assert.areEqual(5, x);
            x = 3; x ||= 5; assert.areEqual(3, x);
            x = 0; x &&= 5; assert.areEqual(0, x);
            x = 3; x &&= 5; assert.areEqual(5, x);
            x = null; x ??= 5; assert.areEqual(5, x);
            x = undefined; x ??= 5; assert.areEqual(5, x);
            x = 0; x ??= 5; assert.areEqual(0, x);
            x = ""; x ??= 5; assert.areEqual("", x);
        }
    },
    {
        name: "Expression value is the resulting value",
        body() {
            let x;
            assert.areEqual(9, (x = undefined, x ??= 9));
            assert.areEqual(2, (x = 1, x &&= 2));
            assert.areEqual(1, (x = 1, x ||= 2));
            assert.areEqual(4, (x = null, x ??= x ??= 4));
        }
    },
    {
        name: "Right-hand side is evaluated only when assigning",
        body() {
            let count = 0;
            const rhs = () => { count++; return 1; };
            let x;
            x = 1; x ||= rhs(); assert.areEqual(0, count);
            x = 0; x &&= rhs(); assert.areEqual(0, count);
            x = 0; x ??= rhs(); assert.areEqual(0, count);
            x = null; x ??= rhs(); assert.areEqual(1, count); assert.areEqual(1, x);
            x = 0; x ||= rhs(); assert.areEqual(2, count);
        }
    },
    {
        name: "Member reference is evaluated once, setter only when assigning",
        body() {
            let gets = 0, sets = 0;
            const o = { _q: 0 };
            Object.defineProperty(o, "q", {
                get() { gets++; return this._q; },
                set(v) { sets++; this._q = v; }
            });
            o.q ||= 11;
            assert.areEqual(1, gets); assert.areEqual(1, sets); assert.areEqual(11, o._q);
            gets = 0; sets = 0;
            o.q ||= 22;
            assert.areEqual(1, gets); assert.areEqual(0, sets); assert.areEqual(11, o._q);

            let bases = 0;
            const getBase = () => { bases++; return o; };
            getBase()._q &&= 5;
            assert.areEqual(1, bases);
            assert.areEqual(5, o._q);
        }
    },
    {
        name: "Computed key is evaluated once, before the right-hand side",
        body() {
            let count = 0;
            const obj = {};
            const incr = () => ++count;
            assert.areEqual(undefined, obj[incr()] &&= incr());
            assert.areEqual(1, count);
            obj[2] = 1;
            assert.areEqual(3, obj[incr()] &&= incr());
            assert.areEqual(3, obj[2]);
            assert.areEqual(3, count);
        }
    },
    {
        name: "Short-circuited assignment to a const does not throw",
        body() {
            const c = 1;
            assert.areEqual(1, c ||= 2);
            assert.areEqual(1, c ??= 2);
            assert.throws(() => { const z = 0; z ||= 2; }, TypeError);
        }
    },
    {
        name: "Target validation matches plain assignment, with no call leniency",
        body() {
            assert.isFalse(parses("1 &&= 2"));
            assert.isFalse(parses("1 ||= 2"));
            assert.isFalse(parses("[1] ??= 2"));
            assert.isFalse(parses("function f() {} f() &&= 1"));
            assert.isFalse(parses("function f() {} f() ||= 1"));
            assert.isFalse(parses("function f() {} f() ??= 1"));
            assert.isFalse(parses("var a; a?.b &&= 1"));
            assert.isTrue(parses("var a; a.b &&= 1"));
            assert.isTrue(parses("var a; a[b()] ??= 1"));
        }
    },
    {
        name: "Functions assigned via ??= and ?? survive return (escape analysis)",
        body() {
            // Regression: AssignRegisters' knopCoalesce/knopAsgCoalesce
            // case skipped the escaped-use walk other binary operators
            // get, so a function expression operand could be
            // stack-allocated and then returned past its frame (debug
            // assert in InterpreterStackFrame, release crash). The
            // plain-'??' half predates logical assignment. The &&=/||=/=
            // forms are controls: they always had the walk.
            function viaNullishAssign() { var y; y ??= function () { return 1; }; return y; }
            assert.areEqual(1, viaNullishAssign()());
            function viaCoalesce() { var y; var z = y ?? function () { return 2; }; return z; }
            assert.areEqual(2, viaCoalesce()());
            function viaCoalesceReturn() { var y; return y ?? function () { return 3; }; }
            assert.areEqual(3, viaCoalesceReturn()());
            function viaAndAssign() { var y = 1; y &&= function () { return 4; }; return y; }
            assert.areEqual(4, viaAndAssign()());
            function viaOrAssign() { var y; y ||= function () { return 5; }; return y; }
            assert.areEqual(5, viaOrAssign()());
            function viaPlainAssign() { var y; y = function () { return 6; }; return y; }
            assert.areEqual(6, viaPlainAssign()());
            // The returned closure must still capture its frame.
            function viaCapture() { var v = 7; var y; y ??= function () { return v; }; v = 8; return y; }
            assert.areEqual(8, viaCapture()());
            function viaArrow() { var y; y ??= () => 9; return y; }
            assert.areEqual(9, viaArrow()());
        }
    },
    {
        name: "Deferred parse rejects parenthesized call targets and runs bodies",
        body() {
            // This file is registered with -forcedeferparse (see
            // rlexe.xml): the parenthesized-call early error must be
            // raised by the deferred first pass, not only on reparse
            // (debug builds assert when a syntax error first surfaces
            // there), and deferred bodies must run after reparse.
            assert.isFalse(parses("function f() {} (f()) &&= 1"));
            assert.isFalse(parses("function f() {} (f()) ||= 1"));
            assert.isFalse(parses("function f() {} (f()) ??= 1"));
            function deferredBody() {
                let x;
                x ??= 3;
                x &&= 4;
                x ||= 5;
                return x;
            }
            assert.areEqual(4, deferredBody());
        }
    },
    {
        name: "Mixing rules for ?? still apply around ??=",
        body() {
            assert.isFalse(parses("var a, b, c; a ?? b || c"));
            assert.isFalse(parses("var a, b, c; a || b ?? c"));
            assert.isTrue(parses("var a; a ??= 1 ?? 2"));
            let a = null;
            a ??= 1 ?? 2;
            assert.areEqual(1, a);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
