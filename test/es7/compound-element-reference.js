//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

// Two focused regressions for the B4 compound-assign + let-ASI lane:
//
// 1. Element compound assignment (compound operators, logical assignment,
//    ++/--) evaluates its reference per ES2025 sec-getvalue / sec-putvalue:
//    ToObject(base) throws the null/undefined TypeError BEFORE ToPropertyKey
//    (key) runs, and the converted key is computed exactly once and reused by
//    the store (test262 S11.13.2_A7.*_T1/T2/T4).
// 2. `let` at the head of a bare statement body (if/while/for/for-in/for-of/
//    with) followed by a newline and then an identifier or `{` parses as the
//    identifier `let` with ASI, not a LexicalDeclaration (test262
//    statements/*/let-{block,identifier}-with-newline.js).

function scriptOK(name, source) {
    var passed = false;
    try {
        WScript.LoadScript(source, "samethread");
        passed = true;
    } catch (e) {
        passed = false;
    }
    assert.isTrue(passed, name + " should parse and run");
}

function scriptThrowsSyntaxError(name, source) {
    var passed = false;
    try {
        WScript.LoadScript(source, "samethread");
    } catch (e) {
        // LoadScript reports the error across the script boundary; match on
        // the constructor name rather than instanceof.
        passed = e.constructor.name === "SyntaxError";
    }
    assert.isTrue(passed, name + " should be a SyntaxError");
}

var tests = [
    {
        name: "compound element assignment converts the key exactly once and reuses it for the store",
        body() {
            var conversions;
            var o;
            function freshKey() {
                return {
                    toString() {
                        assert.isTrue(conversions === 0, "ToPropertyKey ran a second time");
                        conversions++;
                        return "k";
                    }
                };
            }

            conversions = 0; o = { k: 2 };  o[freshKey()] *= 3;
            assert.areEqual(1, conversions, "*= converts once");
            assert.areEqual(6, o.k, "*= stores through the converted key");

            conversions = 0; o = { k: 2 };  o[freshKey()] += 1;
            assert.areEqual(1, conversions, "+= converts once");

            conversions = 0; o = { k: 2 };  o[freshKey()] **= 2;
            assert.areEqual(1, conversions, "**= converts once");

            conversions = 0; o = { k: "a" }; o[freshKey()] += "b";
            assert.areEqual(1, conversions, "string += converts once");
            assert.areEqual("ab", o.k, "string += stores through the converted key");
        }
    },
    {
        name: "increment, decrement and logical assignment convert the key exactly once",
        body() {
            var conversions;
            var o;
            function freshKey() {
                return {
                    toString() {
                        assert.isTrue(conversions === 0, "ToPropertyKey ran a second time");
                        conversions++;
                        return "k";
                    }
                };
            }

            conversions = 0; o = { k: 1 }; o[freshKey()]++;
            assert.areEqual(1, conversions, "postfix ++ converts once");
            assert.areEqual(2, o.k, "postfix ++ stores through the converted key");

            conversions = 0; o = { k: 1 }; ++o[freshKey()];
            assert.areEqual(1, conversions, "prefix ++ converts once");

            conversions = 0; o = { k: 1 }; o[freshKey()]--;
            assert.areEqual(1, conversions, "postfix -- converts once");

            conversions = 0; o = { k: 0 }; o[freshKey()] &&= 5;
            assert.areEqual(1, conversions, "&&= short-circuit load converts once");
            assert.areEqual(0, o.k, "&&= does not store when the load is falsy");

            conversions = 0; o = { k: 1 }; o[freshKey()] &&= 5;
            assert.areEqual(1, conversions, "&&= store path converts once");
            assert.areEqual(5, o.k, "&&= stores through the converted key");

            conversions = 0; o = { k: 1 }; o[freshKey()] ||= 5;
            assert.areEqual(1, conversions, "||= converts once");

            conversions = 0; o = { k: null }; o[freshKey()] ??= 5;
            assert.areEqual(1, conversions, "??= converts once");
            assert.areEqual(5, o.k, "??= stores through the converted key");
        }
    },
    {
        name: "primitive keys keep their fast representations through compound assignment",
        body() {
            var a = [0, 0, 0];
            var i = 1;
            a[i] += 5;
            assert.areEqual(5, a[1], "numeric key stores correctly");
            assert.areEqual(1, i, "the key variable is not clobbered by the converted-key temp");

            var o = { word: "word" };
            var s = "word";
            o[s] += "s";
            assert.areEqual("words", o.word, "string key stores correctly");
            assert.areEqual("word", s, "the key variable keeps its string value");
        }
    },
    {
        name: "null/undefined element base throws TypeError before the key conversion",
        body() {
            function check(base) {
                var conversions = 0;
                var threw = null;
                try {
                    base[{
                        toString() {
                            conversions++;
                            return "k";
                        }
                    }] *= 1;
                } catch (e) {
                    threw = e;
                }
                assert.isTrue(threw instanceof TypeError, "nullish base throws TypeError, saw " + threw);
                assert.areEqual(0, conversions, "the key's toString never ran");
            }
            check(null);
            check(undefined);

            // The store-side guard matches the load-side one.
            var conversions = 0;
            var threw = null;
            try {
                undefined [{
                    toString() {
                        conversions++;
                        return "k";
                    }
                }] |= 1;
            } catch (e) {
                threw = e;
            }
            assert.isTrue(threw instanceof TypeError, "undefined base store throws TypeError");
            assert.areEqual(0, conversions, "store-side key conversion never ran");

            // Plain loads and stores only fix the error path: the TypeError
            // must not be replaced by an exception thrown from a key used only
            // to format the message.
            conversions = 0;
            threw = null;
            try {
                null [{
                    toString() {
                        conversions++;
                        throw new Error("user error must not replace the TypeError");
                    }
                }].k;
            } catch (e) {
                threw = e;
            }
            assert.isTrue(threw instanceof TypeError, "plain load keeps the TypeError");
            assert.areEqual(0, conversions, "an object key's toString never runs for the message");

            // A string key still names the property in the message.
            var message = "";
            try {
                null["prop"].x;
            } catch (e) {
                message = e.message;
            }
            assert.isTrue(message.indexOf("prop") !== -1, "string keys keep naming the property: " + message);
        }
    },
    {
        name: "getter and setter both run through the single converted key",
        body() {
            var log = [];
            var o = {};
            Object.defineProperty(o, "k", {
                get() { log.push("get"); return 2; },
                set(v) { log.push("set:" + v); }
            });
            var conversions = 0;
            o [{
                toString() {
                    conversions++;
                    return "k";
                }
            }] *= 3;
            assert.areEqual(1, conversions, "accessor compound converts once");
            assert.areEqual("get,set:6", log.join(","), "the setter is invoked through the converted key");
        }
    },
    {
        name: "evaluation order: key conversion before the RHS, base before the key",
        body() {
            var order = "";
            var o = {};
            o [{
                toString() {
                    order += "K";
                    return "k";
                }
            }] = (order += "R", 1);
            assert.areEqual("RK", order, "plain = still converts the key after the RHS");

            order = "";
            o = {};
            var key = {
                toString() {
                    order += "K";
                    return "k";
                }
            };
            var rhs = () => (order += "R", 1);
            o[key] *= rhs();
            assert.areEqual("KR", order, "compound converts the key (for the load) before the RHS");

            // A throwing key after a nullish base: TypeError wins over the
            // key's exception (reference order, not error-message formatting).
            var threw = null;
            try {
                null [{
                    toString() {
                        throw new Error("key error");
                    }
                }] += 1;
            } catch (e) {
                threw = e;
            }
            assert.isTrue(threw instanceof TypeError, "ToObject(base) precedes ToPropertyKey(key)");
        }
    },
    {
        name: "let followed by a newline in a bare statement body is the identifier let (ASI)",
        body() {
            scriptOK("if identifier", "var x = 0;\nif (false) let\nx = 1;");
            scriptOK("else identifier", "if (true) {} else let\nx = 1;");
            scriptOK("while identifier", "while (false) let\nx = 1;");
            scriptOK("for identifier", "for (;false;) let\nx = 1;");
            scriptOK("for-in identifier", "for (var p in null) let\nx = 1;");
            scriptOK("for-of identifier", "for (var q of []) let\nx = 1;");
            scriptOK("with identifier", "if (false) { with ({}) let\nx = 1; }");
            scriptOK("if block", "if (false) let\n{}");
            scriptOK("while block", "while (false) let\n{}");
            scriptOK("for block", "for (;false;) let\n{}");
            scriptOK("labeled block", "if (false) { L: let\n{} }");
            // `let` is a real identifier here: it can carry a value.
            scriptOK("let identifier usable", "var let = 1; if (let) { let\n0; }");
        }
    },
    {
        name: "let declarations still parse where the grammar allows them",
        body() {
            // In a StatementList the newline does not turn `let x` into an
            // identifier + ASI: it stays a LexicalDeclaration.
            var source =
                "var let1 = 0;\n" +
                "{\n" +
                "  let\n" +
                "  inner = 1;\n" +
                "}\n";
            scriptOK("block declaration with newline", source);

            // A bare declaration with no newline is still a SyntaxError.
            scriptThrowsSyntaxError("if declaration without newline", "if (false) let x = 1;");
            scriptThrowsSyntaxError("while declaration without newline", "while (false) let x = 1;");
            // `let [` stays a SyntaxError in bare statement position regardless
            // of the newline (ExpressionStatement lookahead restriction).
            scriptThrowsSyntaxError("if let[ with newline", "if (false) let\n[x] = 0;");
            scriptThrowsSyntaxError("do-while let[ with newline", "do let\n[x] = 0\nwhile (false);");
            // Strict mode still rejects `let` as an identifier.
            scriptThrowsSyntaxError("strict identifier let", "'use strict'; if (false) let\nx = 1;");
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
