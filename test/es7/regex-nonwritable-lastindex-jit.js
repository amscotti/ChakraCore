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

// 3. ES6RegExSymbols & ES6RegExPrototypeProperties warmed JIT bailouts & ImplicitCall safety.
function jitSplit(str, re) {
    return str.split(re);
}

function jitMatch(str, re) {
    return str.match(re);
}

function jitSearch(str, re) {
    return str.search(re);
}

function jitReplaceLiteralUnused(s, holder) {
    var x = holder.val;
    s.replace(/a/, "b");
    return x + holder.val;
}

function jitMatchLiteralCapture(s) {
    return s.match(/a/);
}

function jitImplicitCallOnFastReplace(s, re, holder) {
    var x = holder.val;
    var r = s.replace(re, "b");
    return x + ":" + holder.val + ":" + r;
}

function jitCallExecRaw(target, s) {
    return RegExp.prototype.exec.call(target, s);
}

function testJitSplitNoLastIndexMutation() {
    var re = /b/g;
    for (var i = 0; i < 20000; i++) {
        re.lastIndex = 15;
        var parts = jitSplit("aabbcc", re);
        assert(parts.length === 3 && parts[0] === "aa" && parts[1] === "" && parts[2] === "cc",
            "jitSplit global result mismatch");
        assert(re.lastIndex === 15, "jitSplit mutated re.lastIndex: " + re.lastIndex);
    }

    var rNonWritable = /b/g;
    Object.defineProperty(rNonWritable, "lastIndex", { value: 5, writable: false });
    for (var j = 0; j < 2000; j++) {
        var p2 = jitSplit("abc", rNonWritable);
        assert(p2.length === 2 && p2[0] === "a" && p2[1] === "c", "jitSplit non-writable failed");
        assert(rNonWritable.lastIndex === 5, "jitSplit mutated non-writable lastIndex");
    }
}

function testJitInstanceAndCoerceBailouts() {
    for (var i = 0; i < 20000; i++) {
        assert(jitReplace("aba", /a/, "x") === "xba", "warmup replace");
        assert(jitMatch("aba", /a/)[0] === "a", "warmup match");
        assert(jitSplit("aba", /b/).join(",") === "a,a", "warmup split");
        assert(jitSearch("aba", /b/) === 1, "warmup search");
        assert(jitCallExecRaw(/b/, "aba")[0] === "b", "warmup exec.call");
    }

    // Own [Symbol.*] overrides on a RegExp instance
    var rRep = /a/;
    rRep[Symbol.replace] = function (s, rep) { return "own-rep:" + s + ":" + rep; };
    assert(jitReplace("aba", rRep, "Z") === "own-rep:aba:Z", "instance Symbol.replace ignored in JIT");

    var rMat = /a/;
    rMat[Symbol.match] = function (s) { return ["own-match:" + s]; };
    assert(jitMatch("aba", rMat)[0] === "own-match:aba", "instance Symbol.match ignored in JIT");

    var rSpl = /a/;
    rSpl[Symbol.split] = function (s, lim) { return ["own-split", s, String(lim)]; };
    var sr = jitSplit("aba", rSpl);
    assert(sr[0] === "own-split" && sr[1] === "aba" && sr[2] === "undefined", "instance Symbol.split ignored in JIT");

    var rSea = /a/;
    rSea[Symbol.search] = function (s) { return 42; };
    assert(jitSearch("aba", rSea) === 42, "instance Symbol.search ignored in JIT");

    // Own exec on a RegExp instance
    var rExec = /a/;
    rExec.exec = function () { return { 0: "customExec", index: 7 }; };
    assert(fastExec(rExec, "aba")[0] === "customExec", "instance exec ignored in JIT");
    assert(jitMatch("aba", rExec)[0] === "customExec", "match via instance exec ignored in JIT");
    assert(jitSearch("aba", rExec) === 7, "search via instance exec ignored in JIT");

    // Subclass & custom prototype
    class CustomRegExp extends RegExp {
        [Symbol.replace](s, rep) { return "sub-rep:" + s + ":" + rep; }
        [Symbol.match](s) { return ["sub-match:" + s]; }
        [Symbol.split](s) { return ["sub-split", s]; }
        [Symbol.search](s) { return 99; }
    }
    var sub = new CustomRegExp("a");
    assert(jitReplace("aba", sub, "q") === "sub-rep:aba:q", "subclass replace failed");
    assert(jitMatch("aba", sub)[0] === "sub-match:aba", "subclass match failed");
    assert(jitSplit("aba", sub).join("|") === "sub-split|aba", "subclass split failed");
    assert(jitSearch("aba", sub) === 99, "subclass search failed");

    var rProto = /a/;
    Object.setPrototypeOf(rProto, {
        [Symbol.replace]: function (s, rep) { return "proto-obj-rep:" + s + ":" + rep; },
        [Symbol.match]: function (s) { return ["proto-obj-match:" + s]; },
        [Symbol.split]: function (s) { return ["proto-obj-split", s]; },
        [Symbol.search]: function () { return 77; }
    });
    assert(jitReplace("aba", rProto, "k") === "proto-obj-rep:aba:k", "custom proto replace failed");
    assert(jitMatch("aba", rProto)[0] === "proto-obj-match:aba", "custom proto match failed");
    assert(jitSplit("aba", rProto).join("|") === "proto-obj-split|aba", "custom proto split failed");
    assert(jitSearch("aba", rProto) === 77, "custom proto search failed");

    // Coerce_StrOrRegex & Coerce_Regex bailouts: non-RegExp symbol objects and Proxy must not ToString/CreateRegEx early
    var symObj = {
        toString: function () { throw new Error("unexpected toString"); },
        [Symbol.replace]: function (s, rep) { return "sym-rep:" + s + ":" + rep; },
        [Symbol.match]: function (s) { return ["sym-match:" + s]; },
        [Symbol.split]: function (s) { return ["sym-split", s]; },
        [Symbol.search]: function () { return 123; }
    };
    assert(jitReplace("hi", symObj, "!") === "sym-rep:hi:!", "Coerce_StrOrRegex destroyed Symbol.replace");
    assert(jitMatch("hi", symObj)[0] === "sym-match:hi", "Coerce_Regex destroyed Symbol.match");
    assert(jitSplit("hi", symObj).join("|") === "sym-split|hi", "split destroyed Symbol.split");
    assert(jitSearch("hi", symObj) === 123, "search destroyed Symbol.search");

    var proxyHits = [];
    var proxyRe = new Proxy(/a/, {
        get: function (t, prop) {
            proxyHits.push(String(prop));
            var v = Reflect.get(t, prop, t);
            return typeof v === "function" ? v.bind(t) : v;
        }
    });
    assert(jitReplace("aba", proxyRe, "x") === "xba", "Proxy replace failed in JIT");
    assert(proxyHits.indexOf("Symbol(Symbol.replace)") !== -1, "Proxy replace bypassed Symbol.replace trap");
    proxyHits.length = 0;
    assert(jitMatch("aba", proxyRe)[0] === "a", "Proxy match failed in JIT");
    assert(proxyHits.indexOf("Symbol(Symbol.match)") !== -1, "Proxy match bypassed Symbol.match trap");
    assertThrowsTypeError(function () { jitCallExecRaw(proxyRe, "aba"); }, "RegExp.prototype.exec.call(Proxy)");
    assertThrowsTypeError(function () { jitCallExecRaw({}, "a"); }, "RegExp.prototype.exec.call({})");
}

function testJitProtoAndSpeciesTampering() {
    var origReplace = RegExp.prototype[Symbol.replace];
    var origMatch = RegExp.prototype[Symbol.match];
    var origSplit = RegExp.prototype[Symbol.split];
    var origSearch = RegExp.prototype[Symbol.search];
    var origExec = RegExp.prototype.exec;
    var origCtor = RegExp.prototype.constructor;
    var origSpeciesDesc = Object.getOwnPropertyDescriptor(RegExp, Symbol.species);
    var origFlagsDesc = Object.getOwnPropertyDescriptor(RegExp.prototype, "flags");

    // Warm up with untouched RegExp.prototype
    for (var i = 0; i < 20000; i++) {
        assert(jitReplace("aba", /a/, "x") === "xba", "pre-proto replace");
        assert(jitMatch("aba", /a/)[0] === "a", "pre-proto match");
        assert(jitSplit("aba", /b/).join(",") === "a,a", "pre-proto split");
        assert(jitSearch("aba", /b/) === 1, "pre-proto search");
        assert(fastExec(/a/, "aba")[0] === "a", "pre-proto exec");
        assert(fastTest(/a/, "aba") === true, "pre-proto test");
    }

    try {
        // 1. RegExp.prototype[Symbol.replace] override & implicit-call bailout + unused-result side effect
        var holder = { val: 1 };
        for (var w = 0; w < 20000; w++) {
            assert(jitImplicitCallOnFastReplace("ab", /a/, holder) === "1:1:bb", "implicit warmup");
            assert(jitReplaceLiteralUnused("ab", holder) === 2, "unused warmup");
        }
        RegExp.prototype[Symbol.replace] = function (s, rep) {
            holder.val = 99;
            return "proto-rep:" + this.source + ":" + s + ":" + rep;
        };
        assert(jitImplicitCallOnFastReplace("ab", /a/, holder) === "1:99:proto-rep:a:ab:b",
            "ImplicitCall bailout failed on tampered RegExp.prototype[Symbol.replace]");
        holder.val = 1;
        assert(jitReplaceLiteralUnused("ab", holder) === 100,
            "ResultNotUsed fast replace skipped tampered RegExp.prototype[Symbol.replace]");

        // Deleting Symbol.replace falls back to ToString(regexp)
        delete RegExp.prototype[Symbol.replace];
        assert(jitReplace("x/a/y", /a/, "Z") === "xZy",
            "deleted RegExp.prototype[Symbol.replace] did not fall back to ToString");
        RegExp.prototype[Symbol.replace] = origReplace;

        // 2. RegExp.prototype[Symbol.match] override (including stack-allocated literal escape across loop)
        var captured = [];
        for (var k = 0; k < 20000; k++) {
            jitMatchLiteralCapture("abc");
        }
        RegExp.prototype[Symbol.match] = function (s) {
            captured.push(this);
            return ["cap:" + s + ":" + this.source];
        };
        for (var m = 0; m < 5; m++) {
            var res = jitMatchLiteralCapture("xyz");
            assert(res[0] === "cap:xyz:a", "match override failed");
        }
        assert(captured.length === 5 && captured[0] !== captured[1], "boxed literal identity reused across iterations");
        assert(captured[0].source === "a" && captured[4].source === "a", "escaped stack RegExp corrupted");

        // Getter accessor on RegExp.prototype[Symbol.match] & deleted Symbol.match
        Object.defineProperty(RegExp.prototype, Symbol.match, {
            configurable: true,
            get: function () {
                return function (s) { return ["getter-match:" + s]; };
            }
        });
        assert(jitMatch("hi", /a/)[0] === "getter-match:hi", "accessor Symbol.match failed");
        delete RegExp.prototype[Symbol.match];
        assertThrowsTypeError(function () { jitMatch("hi", /a/); }, "deleted Symbol.match must throw TypeError");
        Object.defineProperty(RegExp.prototype, Symbol.match, {
            value: origMatch, writable: true, enumerable: false, configurable: true
        });

        // 3. RegExp.prototype[Symbol.split] override & delete
        RegExp.prototype[Symbol.split] = function (s, lim) {
            return ["proto-split", s, typeof lim];
        };
        assert(jitSplit("abc", /b/).join("|") === "proto-split|abc|undefined", "proto Symbol.split failed");
        delete RegExp.prototype[Symbol.split];
        assert(jitSplit("x/b/y", /b/).join(",") === "x,y", "deleted Symbol.split must fall back to ToString");
        RegExp.prototype[Symbol.split] = origSplit;

        // 4. RegExp.prototype[Symbol.search] override
        RegExp.prototype[Symbol.search] = function () { return 321; };
        assert(jitSearch("abc", /b/) === 321, "proto Symbol.search failed");
        RegExp.prototype[Symbol.search] = origSearch;

        // 5. RegExp.prototype.exec override observed by match/replace/split/search/test/exec
        var execCount = 0;
        RegExp.prototype.exec = function () {
            execCount++;
            return null;
        };
        assert(jitMatch("a", /a/) === null && execCount === 1, "match missed RegExp.prototype.exec");
        assert(jitReplace("a", /a/, "x") === "a" && execCount === 2, "replace missed RegExp.prototype.exec");
        assert(jitSplit("a", /a/).join(",") === "a" && execCount === 3, "split missed RegExp.prototype.exec");
        assert(jitSearch("a", /a/) === -1 && execCount === 4, "search missed RegExp.prototype.exec");
        assert(fastTest(/a/, "a") === false && execCount === 5, "test missed RegExp.prototype.exec");
        assert(fastExec(/a/, "a") === null && execCount === 6, "exec missed RegExp.prototype.exec");
        RegExp.prototype.exec = origExec;

        // 6. RegExp[Symbol.species] and RegExp.prototype.constructor overrides observed by split
        var speciesCalls = 0;
        Object.defineProperty(RegExp, Symbol.species, {
            configurable: true,
            get: function () {
                speciesCalls++;
                return function FakeSplitter(pattern, flags) {
                    return {
                        exec: function () { return null; }
                    };
                };
            }
        });
        assert(jitSplit("abc", /b/).join(",") === "abc" && speciesCalls === 1,
            "jitSplit bypassed overridden RegExp[Symbol.species]");
        Object.defineProperty(RegExp, Symbol.species, origSpeciesDesc);

        var ctorCalls = 0;
        RegExp.prototype.constructor = {
            get [Symbol.species]() {
                ctorCalls++;
                return RegExp;
            }
        };
        assert(jitSplit("aba", /b/).join(",") === "a,a" && ctorCalls === 1,
            "jitSplit bypassed overridden RegExp.prototype.constructor");
        RegExp.prototype.constructor = origCtor;

        // 7. RegExp.prototype.flags override observed by match & replace
        var flagsReads = 0;
        Object.defineProperty(RegExp.prototype, "flags", {
            configurable: true,
            get: function () {
                flagsReads++;
                return "";
            }
        });
        assert(jitMatch("aba", /a/g)[0] === "a" && flagsReads === 1,
            "jitMatch bypassed RegExp.prototype.flags");
        assert(jitReplace("aba", /a/g, "X") === "Xba" && flagsReads === 2,
            "jitReplace bypassed RegExp.prototype.flags");
        Object.defineProperty(RegExp.prototype, "flags", origFlagsDesc);
    } finally {
        RegExp.prototype[Symbol.replace] = origReplace;
        Object.defineProperty(RegExp.prototype, Symbol.match, {
            value: origMatch, writable: true, enumerable: false, configurable: true
        });
        RegExp.prototype[Symbol.split] = origSplit;
        RegExp.prototype[Symbol.search] = origSearch;
        RegExp.prototype.exec = origExec;
        RegExp.prototype.constructor = origCtor;
        if (origSpeciesDesc) {
            Object.defineProperty(RegExp, Symbol.species, origSpeciesDesc);
        }
        if (origFlagsDesc) {
            Object.defineProperty(RegExp.prototype, "flags", origFlagsDesc);
        }
    }
}

function testPrototypeReceiverGetters() {
    assert(RegExp.prototype.source === "(?:)", "RegExp.prototype.source must be '(?:)'");
    assert(RegExp.prototype.flags === "", "RegExp.prototype.flags must be ''");
    assert(RegExp.prototype.global === undefined, "RegExp.prototype.global must be undefined");
    assert(RegExp.prototype.ignoreCase === undefined, "RegExp.prototype.ignoreCase must be undefined");
    assert(RegExp.prototype.multiline === undefined, "RegExp.prototype.multiline must be undefined");
    assert(RegExp.prototype.sticky === undefined, "RegExp.prototype.sticky must be undefined");
    assert(RegExp.prototype.unicode === undefined, "RegExp.prototype.unicode must be undefined");
    assert(RegExp.prototype.dotAll === undefined, "RegExp.prototype.dotAll must be undefined");
    assert(RegExp.prototype.hasIndices === undefined, "RegExp.prototype.hasIndices must be undefined");
    assert(RegExp.prototype.unicodeSets === undefined, "RegExp.prototype.unicodeSets must be undefined");
    assertThrowsTypeError(function () {
        var o = { __proto__: /a/ };
        return o.source;
    }, "non-RegExp inheriting from RegExp throws TypeError for .source");
}

testJitFastExec();
testHelperLastIndexChecks();
if (typeof RegExp.prototype[Symbol.match] === "function") {
    testJitSplitNoLastIndexMutation();
    testJitInstanceAndCoerceBailouts();
    testJitProtoAndSpeciesTampering();
}
if (Object.getOwnPropertyDescriptor(RegExp.prototype, "source")) {
    testPrototypeReceiverGetters();
}

print("pass");
