//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Q1.2 RegExp & Unicode conformance:
//  1. /ui and /vi Simple Case Folding parity (ICU 79.1 / Unicode 18.0 C+S)
//  2. Cross-plane {0x00DF, 0x1E9E, 0x1DF95} equivalence across atoms, classes,
//     lookbehind, \q{}, and backreferences
//  3. Bounded /u and /v surrogate-pair code-point matching in negated/complement sets
//  4. RegExp.prototype[Symbol.replace] ToLength & groups fn semantics, plus
//     ES2025 RegExp.prototype.{exec,test} unconditional lastIndex access

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "1. /ui simple case folding parity (Unicode 9.0 -> 18.0 C+S)",
        body() {
            // Historical /ui CaseFolding divergences
            assert.isTrue(/[\u0390]/ui.test("\u1fd3"), "0390 class matches 1FD3");
            assert.isTrue(/[\u1fd3]/ui.test("\u0390"), "1FD3 class matches 0390");
            assert.isTrue(/\u0390/ui.test("\u1fd3"), "0390 atom matches 1FD3");
            assert.isTrue(/[\u03b0]/ui.test("\u1fe3"), "03B0 class matches 1FE3");
            assert.isTrue(/[\u1fe3]/ui.test("\u03b0"), "1FE3 class matches 03B0");
            assert.isTrue(/[\ufb05]/ui.test("\ufb06"), "FB05 class matches FB06");
            assert.isTrue(/[\ufb06]/ui.test("\ufb05"), "FB06 class matches FB05");

            // Cyrillic historic / Georgian Mtavruli / Unicode 10-18 C+S families
            assert.isTrue(/\u0412/ui.test("\u1c80"), "Cyrillic Ve <-> 1C80");
            assert.isTrue(/\u1c80/ui.test("\u0432"), "1C80 <-> Cyrillic small ve");
            assert.isTrue(/[\u0422]/ui.test("\u1c84") && /[\u1c85]/ui.test("\u0442"), "Cyrillic Te class");
            assert.isTrue(/\u0462/ui.test("\u1c87") && /\u1c87/ui.test("\u0463"), "Yat 0462/0463 <-> 1C87");
            assert.isTrue(/\ua64a/ui.test("\u1c88") && /\u1c88/ui.test("\ua64b"), "Monograph Uk <-> 1C88");
            assert.isTrue(/\u1c89/ui.test("\u1c8a"), "Cyrillic 1C89 <-> 1C8A");
            assert.isTrue(/\u10d0/ui.test("\u1c90") && /\u1cbf/ui.test("\u10ff"), "Georgian Mtavruli");
            assert.isTrue(/\u0277/ui.test("\ua7dd") && /\ua7dd/ui.test("\u0277"), "Unicode 18.0 0277 <-> A7DD");
            assert.isTrue(/\u019b/ui.test("\ua7dc") && /\u027c/ui.test("\ua7e2"), "Latin extensions");
            assert.isTrue(/\u{10d50}/ui.test("\u{10d70}"), "Garay");
            assert.isTrue(/\u{16ea0}/ui.test("\u{16ebb}"), "Beria Erfe");
            assert.isTrue(/\u{1df40}/ui.test("\u{1df41}"), "Latin extensions astral");

            // Non-/u /i keeps UnicodeData behavior and does not over-fold CaseFolding additions
            assert.isFalse(/\u0390/i.test("\u1fd3"), "non-u /i 0390 != 1FD3");
            assert.isFalse(/\u0412/i.test("\u1c80"), "non-u /i 0412 != 1C80");
            assert.isFalse(/k/i.test("\u212a"), "non-u /i k != Kelvin");
            assert.isFalse(/s/i.test("\u017f"), "non-u /i s != long s");

            // Full (F) multi-char case foldings must NOT apply under /ui
            assert.isFalse(/\u00df/ui.test("ss"), "sharp s does not fold to ss");
            assert.isFalse(/\ufb00/ui.test("ff"), "fb00 does not fold to ff");
            assert.isFalse(/\u0130/ui.test("i\u0307"), "dotted I does not fold to multi-char");
        }
    },
    {
        name: "2. Cross-plane equivalence {0x00DF, 0x1E9E, 0x1DF95}",
        body() {
            const members = ["\u00df", "\u1e9e", "\u{1df95}"];
            const pats = [/\u00df/ui, /\u1e9e/ui, /\u{1df95}/ui, /[\u00df]/ui, /[\u1e9e]/ui, /[\u{1df95}]/ui, /[\u00df]/vi, /[\u{1df95}]/vi];
            for (const re of pats) {
                for (const s of members) {
                    assert.isTrue(re.test(s), re + " should match " + escape(s));
                }
            }
            assert.isTrue(/^a\u00dfb$/ui.test("a\u{1df95}b"), "BMP literal surrounded by ASCII matches astral");
            assert.isTrue(/^a\u{1df95}b$/ui.test("a\u1e9eb"), "astral literal surrounded by ASCII matches BMP");
            assert.isTrue(/^\u00df+$/ui.test("\u00df\u1e9e\u{1df95}"), "quantified BMP matches mixed classes");
            assert.isTrue(/^\u{1df95}{3}$/ui.test("\u{1df95}\u00df\u1e9e"), "quantified astral matches mixed classes");

            // Negation, lookbehind, scoped modifiers, and \q string sets
            assert.isFalse(/[^\u00df]/ui.test("\u{1df95}"), "negated sharp s rejects 1DF95");
            assert.isFalse(/[^\u{1df95}]/ui.test("\u00df"), "negated 1DF95 rejects sharp s");
            assert.isTrue(/(?i:\u00df)/u.test("\u{1df95}"), "scoped ?i on sharp s");
            assert.isTrue(/(?i:\u{1df95})/u.test("\u1e9e"), "scoped ?i on 1DF95");
            assert.isFalse(/(?-i:\u00df)/ui.test("\u{1df95}"), "scoped ?-i disables cross-plane fold");
            assert.isTrue(/(?<=\u00df)x/ui.test("\u{1df95}x"), "lookbehind sharp s matches after 1DF95");
            assert.isTrue(/(?<=\u{1df95})x/ui.test("\u00dfx"), "lookbehind 1DF95 matches after sharp s");
            assert.isTrue(/^[\q{a\u00df}]$/vi.test("a\u{1df95}"), "\\q BMP matches astral");
            assert.isTrue(/^[\q{a\u{1df95}}]$/vi.test("a\u1e9e"), "\\q astral matches BMP");

            // Backreferences across unit lengths (1 <-> 2), forward and reverse
            for (const a of members) {
                for (const b of members) {
                    assert.isTrue(/^(.)\1$/ui.test(a + b), "backref " + escape(a) + "==" + escape(b));
                    assert.isTrue(/(?<=(.)\1)z$/ui.test(a + b + "z"), "lookbehind backref " + escape(a) + "==" + escape(b));
                }
            }
            assert.isFalse(/^(.)\1$/ui.test("\u{1df95}"), "astral group cannot self-match on end of string");
            assert.isFalse(/\u00df/i.test("\u1e9e"), "non-u /i does not fold sharp s");
        }
    },
    {
        name: "3. Negated/complement /u and /v classes decode surrogate pairs as single code points",
        body() {
            // /u negated classes
            assert.isFalse(/^[^\u{10400}]+$/u.test("\ud801\udc00"), "/u negated singleton astral rejects its pair");
            assert.isTrue(/^[^\u{10400}]+$/u.test("\ud800\udc00"), "/u negated singleton astral accepts other pair");
            assert.isFalse(/^[^\u{10000}-\u{10FFFF}]+$/u.test("\ud800\udc00"), "/u all-BMP complement rejects valid surrogate pair");
            assert.isTrue(/^[^\u{10000}-\u{10FFFF}]+$/u.test("a\ud800"), "/u all-BMP complement accepts BMP + lone high");
            assert.isTrue(/^[^\u{10000}-\u{10FFFF}]+$/u.test("\ud800\ud801"), "/u all-BMP complement accepts two lone highs");
            assert.isTrue(/^[^\ud800-\udfff]+$/u.test("\ud800\udc00"), "/u negated surrogate range accepts U+10000");
            assert.isFalse(/^[^\ud800-\udfff]+$/u.test("\ud800"), "/u negated surrogate range rejects lone high");

            // /v negated & difference complement sets
            assert.isFalse(/^[^[\u{10000}-\u{10FFFF}]]+$/v.test("\ud800\udc00"), "/v all-BMP negation rejects pair");
            assert.isFalse(/^[[^\u{10000}-\u{10FFFF}]--[a]]+$/v.test("\ud800\udc00"), "/v difference of complement rejects pair");
            assert.isTrue(/^[[^\u{10000}-\u{10FFFF}]--[a]]+$/v.test("b\ud800"), "/v difference accepts non-excluded BMP + lone");
            assert.isFalse(/^[[^\u{10000}-\u{10FFFF}]--[a]]+$/v.test("a"), "/v difference excludes a");
            assert.isFalse(/^[^[\u0000-\uD7FF\uD801-\u{10FFFF}]]+$/v.test("\ud800\udc00"), "/v surrogate singleton complement rejects pair");
            assert.isTrue(/^[^[\u0000-\uD7FF\uD801-\u{10FFFF}]]+$/v.test("\ud800"), "/v surrogate singleton complement accepts lone high");
        }
    },
    {
        name: "4. Symbol.replace lastIndex ToLength & functional groups argument",
        body() {
            // Test262 coerce-lastindex shape: Math.pow(2, 54) -> ToLength = 2^53 - 1 -> AdvanceStringIndex = 2^53
            for (const flags of ["g", "gu"]) {
                let called = false;
                const re = new RegExp(".", flags);
                re.exec = function () {
                    if (called) return null;
                    re.lastIndex = { valueOf() { return Math.pow(2, 54); } };
                    called = true;
                    return { length: 1, 0: "", index: 0 };
                };
                assert.areEqual("", re[Symbol.replace]("", ""), "replace flags=" + flags);
                assert.areEqual(Math.pow(2, 53), re.lastIndex, "lastIndex advances to 2^53 for flags=" + flags);
            }

            // Test262 named-groups-fn shape: non-undefined primitive/null groups passed raw to replacer
            let currentGroups;
            const gr = /./;
            gr.exec = function () {
                return { length: 1, 0: "a", index: 0, groups: currentGroups };
            };
            let lastArg;
            const captureArg = function () {
                lastArg = arguments[arguments.length - 1];
                return "x";
            };
            for (const g of [null, 10, "raw", false]) {
                currentGroups = g;
                assert.areEqual("x", gr[Symbol.replace]("a", captureArg));
                assert.areEqual(g, lastArg, "functional replacer keeps groups=" + String(g));
            }
            currentGroups = undefined;
            gr[Symbol.replace]("a", captureArg);
            assert.areEqual("a", lastArg, "undefined groups omitted from replacer args");

            // String substitution still requires ToObject on non-undefined groups
            currentGroups = null;
            assert.throws(() => gr[Symbol.replace]("a", "$<a>"), TypeError, "string replacer ToObject(null) throws TypeError");
            currentGroups = 10;
            assert.areEqual("", gr[Symbol.replace]("a", "$<a>"), "string replacer ToObject(10) yields missing group");
        }
    },
    {
        name: "5. Non-global/non-sticky exec & test ToLength(lastIndex) access + JIT stability",
        body() {
            function runExecTestChecks() {
                let gets = 0;
                const counter = {
                    valueOf() {
                        gets++;
                        return 99;
                    }
                };

                const re = /./;
                assert.isTrue(re.test("abc"), "warm test cache first");
                re.lastIndex = counter;

                // Repeated exec & test calls must coerce lastIndex every time without writing it
                const m1 = re.exec("abc");
                assert.areEqual("a", m1[0]);
                assert.areEqual(counter, re.lastIndex);
                assert.areEqual(1, gets, "exec #1 coerced lastIndex");

                const m2 = re.exec("abc");
                assert.areEqual("a", m2[0]);
                assert.areEqual(2, gets, "exec #2 coerced lastIndex again");

                assert.isTrue(re.test("abc"), "test #1 matched despite lastIndex=99");
                assert.areEqual(3, gets, "test #1 bypassed cache and coerced lastIndex");

                assert.isTrue(re.test("abc"), "test #2 matched");
                assert.areEqual(4, gets, "test #2 coerced lastIndex again");

                const missRe = /z/;
                missRe.lastIndex = counter;
                assert.areEqual(null, missRe.exec("abc"));
                assert.isFalse(missRe.test("abc"));
                assert.areEqual(6, gets, "failing exec/test coerced lastIndex");
                assert.areEqual(counter, missRe.lastIndex);

                // Non-writable lastIndex on non-global/non-sticky still accesses without throwing
                const nw = /a/;
                let nwGets = 0;
                Object.defineProperty(nw, "lastIndex", {
                    value: { valueOf() { nwGets++; return 5; } },
                    writable: false
                });
                assert.areEqual("a", nw.exec("abc")[0]);
                assert.isTrue(nw.test("abc"));
                assert.areEqual(2, nwGets);

                // Uncoercible Symbol/BigInt lastIndex throws TypeError on both exec and test
                const symRe = /a/;
                symRe.lastIndex = Symbol("s");
                assert.throws(() => symRe.exec("a"), TypeError, "exec on Symbol lastIndex");
                assert.throws(() => symRe.test("a"), TypeError, "test on Symbol lastIndex");
            }

            for (let iter = 0; iter < 50; iter++) {
                runExecTestChecks();
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
