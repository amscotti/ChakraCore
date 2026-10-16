//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2018 named capture groups on the -ES6RegExSymbols surfaces
// (doc/c2-s9-namedgroups.md): matchAll iterator results, @@replace with
// custom exec results and groups objects, and replaceAll through @@replace.
// Registered with -ES6RegExSymbols only, like matchall.js.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "matchAll results carry a groups object per step",
        body() {
            var matches = [..."a1b2".matchAll(/(?<letter>[ab])|(?<digit>\d)/g)];
            assert.areEqual(4, matches.length);
            assert.areEqual("a", matches[0].groups.letter);
            assert.areEqual(undefined, matches[0].groups.digit);
            assert.areEqual(undefined, matches[1].groups.letter);
            assert.areEqual("1", matches[1].groups.digit);
            assert.areEqual("b", matches[2].groups.letter);
            assert.areEqual("2", matches[3].groups.digit);
            var single = [..."ab".matchAll(/(?<x>a)/g)];
            assert.areEqual(1, single.length);
            assert.areEqual("a", single[0].groups.x);
        }
    },
    {
        name: "@@replace reads groups from the exec result for $<name>",
        body() {
            class FakeRegExp extends RegExp {
                exec(subject) {
                    const fakeResult = ["ab", "a"];
                    fakeResult.index = 0;
                    fakeResult.groups = { a: "b" };
                    Object.getPrototypeOf(fakeResult.groups).b = "c";
                    return fakeResult;
                }
            }
            const re = new FakeRegExp();
            assert.areEqual("b", "ab".replace(re, "$<a>"), "own property");
            assert.areEqual("c", "ab".replace(re, "$<b>"), "inherited property");
        }
    },
    {
        name: "@@replace falls back to the prototype chain for groups",
        body() {
            class FakeRegExp extends RegExp {
                exec(subject) {
                    const fakeResult = ["ab", "a"];
                    fakeResult.index = 0;
                    return fakeResult;
                }
            }
            Array.prototype.groups = { a: "b" };
            assert.areEqual("b", "ab".replace(new FakeRegExp(), "$<a>"), "Array.prototype.groups");
            Array.prototype.groups = undefined;
        }
    },
    {
        name: "@@replace string path on a plain named-group regexp",
        body() {
            assert.areEqual("badc", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/g, "$<snd>$<fst>"));
            assert.areEqual("cd", "abcd".replace(/(?<fst>.)(?<snd>.)|(?<thd>x)/, "$<thd>"));
        }
    },
    {
        name: "@@replace passes the coerced groups object to replacement functions",
        body() {
            var result = "abcd".replace(/(?<fst>.)(?<snd>.)/, function (matched, fst, snd, offset, string, groups) {
                assert.areEqual(6, arguments.length);
                assert.areEqual("a", groups.fst);
                assert.areEqual("b", groups.snd);
                return groups.snd + groups.fst;
            });
            assert.areEqual("bacd", result);
            // no groups argument when the exec result has no groups value
            var plain = "ab".replace(/(.)(.)/, function () { return String(arguments.length); });
            assert.areEqual("5", plain, "match + 2 captures + offset + string (no groups)");
        }
    },
    {
        name: "replaceAll routes named groups through @@replace",
        body() {
            assert.areEqual("aaaa", "aa".replaceAll(/(?<x>a)/g, "$<x>$<x>"));
            var calls = 0;
            class FakeRegExp extends RegExp {
                constructor() { super("(?:)", "g"); }
                exec(subject) {
                    return calls++ === 0 ? Object.assign(["ab", "a"], { index: 0, groups: { a: "b" } }) : null;
                }
            }
            assert.areEqual("b", "ab".replaceAll(new FakeRegExp(), "$<a>"), "fake groups observed once");
        }
    },
    {
        name: "exec under the flag still builds the null-prototype groups object",
        body() {
            var m = /(?<x>a)/.exec("a");
            assert.areEqual(null, Object.getPrototypeOf(m.groups));
            assert.areEqual("a", m.groups.x);
            assert.areEqual(undefined, /a/.exec("a").groups);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
