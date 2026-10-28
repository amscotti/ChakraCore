//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// ES2020 matchAll + the ES6RegExSymbols primitive-Symbol.* and lastIndex
// [[Set]] defect fixes (doc/c2-s7-matchall.md). This file is registered with
// -ES6RegExSymbols only: every surface it probes is behind that flag.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function collect(iter) {
    var out = [];
    for (var m of iter) {
        out.push(m);
    }
    return out;
}

var tests = [
    {
        name: "Symbol.matchAll and all four surfaces exist under the flag",
        body() {
            assert.areEqual("symbol", typeof Symbol.matchAll, "Symbol.matchAll");
            assert.areEqual("Symbol(Symbol.matchAll)", String(Symbol.matchAll), "description");
            assert.areEqual("function", typeof String.prototype.matchAll, "String.prototype.matchAll");
            assert.areEqual("function", typeof RegExp.prototype[Symbol.matchAll], "RegExp.prototype[Symbol.matchAll]");
            assert.areEqual(1, String.prototype.matchAll.length);
            assert.areEqual("matchAll", String.prototype.matchAll.name);
            assert.areEqual(1, RegExp.prototype[Symbol.matchAll].length);
            assert.areEqual("[Symbol.matchAll]", RegExp.prototype[Symbol.matchAll].name);
            var stringDesc = Object.getOwnPropertyDescriptor(String.prototype, "matchAll");
            assert.isTrue(stringDesc.writable && !stringDesc.enumerable && stringDesc.configurable);
            var reDesc = Object.getOwnPropertyDescriptor(RegExp.prototype, Symbol.matchAll);
            assert.isTrue(reDesc.writable && !reDesc.enumerable && reDesc.configurable);
        }
    },
    {
        name: "RegExp String Iterator surface",
        body() {
            var iter = "a1".matchAll(/\d/g);
            assert.areEqual("RegExp String Iterator", iter[Symbol.toStringTag]);
            var proto = Object.getPrototypeOf(iter);
            assert.areEqual("RegExp String Iterator", proto[Symbol.toStringTag]);
            var iteratorProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
            assert.areEqual(iteratorProto, Object.getPrototypeOf(proto), "%IteratorPrototype% ancestry");
            assert.areEqual(0, proto.next.length);
            assert.areEqual("next", proto.next.name);
        }
    },
    {
        name: "global iteration, non-global single result, done sticks",
        body() {
            var matches = collect("a1b2".matchAll(/\d/g));
            assert.areEqual(2, matches.length);
            assert.areEqual("1", matches[0][0]);
            assert.areEqual(1, matches[0].index);
            assert.areEqual("a1b2", matches[0].input);
            assert.areEqual("2", matches[1][0]);
            assert.areEqual(3, matches[1].index);

            var iter = /b/[Symbol.matchAll]("abc");
            var first = iter.next();
            assert.isFalse(first.done);
            assert.areEqual("b", first.value[0]);
            var second = iter.next();
            assert.isTrue(second.done);
            assert.areEqual(undefined, second.value);

            // non-global matchAll never touches the original lastIndex
            var re = /b/;
            var it = re[Symbol.matchAll]("abc");
            it.next();
            assert.areEqual(0, re.lastIndex);
        }
    },
    {
        name: "sticky matchAll yields at most one match",
        body() {
            var iter = /a/y[Symbol.matchAll]("ab");
            var first = iter.next();
            assert.isFalse(first.done);
            assert.areEqual("a", first.value[0]);
            assert.isTrue(iter.next().done);
        }
    },
    {
        name: "empty matches advance by one",
        body() {
            var indexes = collect("ab".matchAll(/()/g)).map(m => m.index);
            assert.areEqual("0,1,2", indexes.join(","));
        }
    },
    {
        name: "species constructor clones with the flags string; global/unicode come from flags",
        body() {
            var args;
            var re = /\d/gu;
            re.constructor = {
                [Symbol.species]: function () {
                    args = Array.prototype.slice.call(arguments);
                    return /\w/g;
                }
            };
            var iter = re[Symbol.matchAll]("a*b");
            assert.areEqual(2, args.length);
            assert.areEqual(re, args[0]);
            assert.areEqual("gu", args[1], "flags string (internal fallback, no proto getter)");
            assert.areEqual("a", iter.next().value[0]);
            assert.areEqual("b", iter.next().value[0]);
            assert.isTrue(iter.next().done);
        }
    },
    {
        name: "the matcher's global/unicode properties are never read",
        body() {
            // A plain-object matcher (non-regexp species result) whose
            // "global" is a throwing getter: global is derived from the
            // flags string, so nothing reads it.
            var re = /./g;
            var execCalls = 0;
            re.constructor = {
                [Symbol.species]: function () {
                    return {
                        exec: function (input) {
                            return ++execCalls <= 2 ? { 0: "x", index: 0, input, length: 1 } : null;
                        },
                        get global() { throw new Error("global read"); },
                        get unicode() { throw new Error("unicode read"); }
                    };
                }
            };
            var iter = re[Symbol.matchAll]("ab");
            assert.areEqual("x", iter.next().value[0]);
            assert.areEqual("x", iter.next().value[0]);
            assert.isTrue(iter.next().done);
        }
    },
    {
        name: "own flags value overrides; lastIndex is cached at @@matchAll time",
        body() {
            var re = /\w/;
            Object.defineProperty(re, "flags", { value: "g" });
            var matches = collect(re[Symbol.matchAll]("a*b"));
            assert.areEqual(2, matches.length);

            var r2 = /./g;
            r2.lastIndex = 2;
            var iter = r2[Symbol.matchAll]("abcd");
            r2.lastIndex = 0;
            assert.areEqual(2, iter.next().value.index, "cached, not re-read");
            assert.areEqual(3, iter.next().value.index);
        }
    },
    {
        name: "brand checks on next",
        body() {
            var proto = Object.getPrototypeOf(/./[Symbol.matchAll](""));
            assert.throws(() => proto.next.call({}), TypeError);
            assert.throws(() => proto.next.call(Object.create(/./g[Symbol.matchAll]("x"))), TypeError);
            assert.throws(() => proto.next.call(42), TypeError);
        }
    },
    {
        name: "custom exec is looked up per next and validated",
        body() {
            var re = /./g;
            var calls = 0;
            var savedExec = RegExp.prototype.exec;
            RegExp.prototype.exec = function (s) {
                calls++;
                return calls <= 2 ? { 0: "x", index: calls - 1, input: s, length: 1 } : null;
            };
            try {
                var iter = re[Symbol.matchAll]("abc");
                assert.areEqual("x", iter.next().value[0]);
                assert.areEqual("x", iter.next().value[0]);
                assert.isTrue(iter.next().done);
                assert.areEqual(3, calls);
            } finally {
                RegExp.prototype.exec = savedExec;
            }

            RegExp.prototype.exec = function () { return 42; };
            try {
                assert.throws(() => /./g[Symbol.matchAll]("a").next(), TypeError);
            } finally {
                RegExp.prototype.exec = savedExec;
            }
        }
    },
    {
        name: "String.prototype.matchAll protocol",
        body() {
            assert.areEqual("null", collect("-null-".matchAll(null))[0][0], "null becomes /null/g");
            var empty = "a".matchAll(undefined).next();
            assert.isFalse(empty.done, "undefined becomes the empty regex");
            assert.areEqual("", empty.value[0]);
            assert.areEqual(1, collect("a,b".matchAll(",")).length, "primitive separator");

            var obj = {};
            obj[Symbol.matchAll] = function (s) { return ["called", s]; };
            assert.areEqual("called,str", "str".matchAll(obj).join(","), "custom @@matchAll dispatch");
            assert.throws(() => "".matchAll(/a/), TypeError, "non-global regexp");
            assert.throws(() => "".matchAll(/a/u), TypeError, "non-global u regexp");
            var nonglobal = {};
            Object.defineProperty(nonglobal, "flags", { value: "muyi" });
            nonglobal[Symbol.match] = true;
            assert.throws(() => "".matchAll(nonglobal), TypeError, "flags without g");
            var undefFlags = {};
            undefFlags[Symbol.match] = true;
            assert.throws(() => "".matchAll(undefFlags), TypeError, "undefined flags");
            assert.throws(() => String.prototype.matchAll.call(null, "x"), TypeError, "RequireObjectCoercible");
            var ret = {};
            var seenThis;
            var saved = RegExp.prototype[Symbol.matchAll];
            RegExp.prototype[Symbol.matchAll] = function () { seenThis = this; return ret; };
            try {
                var dispatch = /x/g;
                assert.areEqual(ret, "".matchAll(dispatch));
                assert.areEqual(dispatch, seenThis);
            } finally {
                RegExp.prototype[Symbol.matchAll] = saved;
            }
        }
    },
    {
        name: "matchAll with a regexp whose own @@matchAll is undefined still invokes the prototype method",
        body() {
            var ret = {};
            var arg;
            var saved = RegExp.prototype[Symbol.matchAll];
            RegExp.prototype[Symbol.matchAll] = function (s) { arg = s; return ret; };
            try {
                var re = /./g;
                re[Symbol.matchAll] = undefined;
                assert.areEqual(ret, "abc".matchAll(re));
                assert.areEqual("abc", arg);
                // and with null: the clone has no own @@matchAll either
                re[Symbol.matchAll] = null;
                assert.areEqual(ret, "abc".matchAll(re));
                assert.areEqual("abc", arg);
            } finally {
                RegExp.prototype[Symbol.matchAll] = saved;
            }
        }
    },
    {
        name: "primitive Symbol.* reads: getters on primitive prototypes never run",
        body() {
            var hits = 0;
            function poison(proto, sym) {
                Object.defineProperty(proto, sym, { get() { hits++; return undefined; }, configurable: true });
            }
            poison(String.prototype, Symbol.match);
            poison(String.prototype, Symbol.replace);
            poison(String.prototype, Symbol.search);
            poison(String.prototype, Symbol.split);
            poison(String.prototype, Symbol.matchAll);
            try {
                assert.areEqual("b", "abc".match("b")[0]);
                assert.areEqual("Xbc", "abc".replace("a", "X"));
                assert.areEqual(1, "abc".search("b"));
                assert.areEqual("a,b,c", "a,b,c".split(",").join(","));
                assert.areEqual(2, collect("a,b,c".matchAll(",")).length);
                assert.areEqual(0, hits);
            } finally {
                delete String.prototype[Symbol.match];
                delete String.prototype[Symbol.replace];
                delete String.prototype[Symbol.search];
                delete String.prototype[Symbol.split];
                delete String.prototype[Symbol.matchAll];
            }
        }
    },
    {
        name: "number and boolean primitives skip Symbol.* too",
        body() {
            var hits = 0;
            Object.defineProperty(Number.prototype, Symbol.match, { get() { hits++; return undefined; }, configurable: true });
            Object.defineProperty(Boolean.prototype, Symbol.replace, { get() { hits++; return undefined; }, configurable: true });
            try {
                "a1".match(1);
                "atrue".replace(true, "X");
                assert.areEqual(0, hits);
            } finally {
                delete Number.prototype[Symbol.match];
                delete Boolean.prototype[Symbol.replace];
            }
        }
    },
    {
        name: "lastIndex [[Set]]: non-writable lastIndex throws on failed match reset (defect fix 2)",
        body() {
            var r = /a/g;
            Object.defineProperty(r, "lastIndex", { writable: false });
            assert.throws(() => r[Symbol.match]("b"), TypeError, "global failure reset");
            assert.areEqual(false, Object.getOwnPropertyDescriptor(r, "lastIndex").writable);

            var ry = /c/y;
            Object.defineProperty(ry, "lastIndex", { writable: false });
            assert.throws(() => ry[Symbol.replace]("abc", "x"), TypeError, "sticky failure reset via @@replace");
            assert.throws(() => ry[Symbol.match]("abc"), TypeError, "sticky failure reset via @@match");

            // sloppy assignment to the non-writable property is silently ignored
            var r2 = /a/g;
            Object.defineProperty(r2, "lastIndex", { writable: false });
            r2.lastIndex = 5;
            assert.areEqual(0, r2.lastIndex);
            // and it cannot be made writable again (non-configurable)
            assert.throws(() => Object.defineProperty(r2, "lastIndex", { writable: true }), TypeError);
        }
    },
    {
        name: "lastIndex [[Set]]: successful global/sticky match updates lastIndex observably",
        body() {
            var r = /b/g;
            assert.areEqual("b", r.exec("abc")[0]);
            assert.areEqual(2, r.lastIndex, "exec success Set through [[Set]]");

            // the trailing failed exec of a global @@match resets to 0
            var rg = /b/g;
            assert.areEqual("b", rg[Symbol.match]("abc")[0]);
            assert.areEqual(0, rg.lastIndex, "failed-match reset");

            var rs = /a/y;
            rs[Symbol.match]("a");
            assert.areEqual(1, rs.lastIndex, "sticky success Set");
        }
    },
    {
        name: "matchAll iterator sets lastIndex on empty matches through [[Set]] and can throw",
        body() {
            // species returns the same regexp, so the iterator's matcher IS re
            var re = /()/g;
            re.constructor = { [Symbol.species]: function () { return re; } };
            var iter = re[Symbol.matchAll]("ab");
            assert.areEqual(0, iter.next().value.index);
            assert.areEqual(1, re.lastIndex, "advanced past the empty match");
            Object.defineProperty(re, "lastIndex", { writable: false });
            assert.throws(() => iter.next(), TypeError, "the next exec Set on non-writable lastIndex throws");
        }
    },
    {
        name: "accessor lastIndex on the iterator's matcher is invoked for the transfer and the advance",
        body() {
            var setValues = [];
            var v = 0;
            var re = /(|x)/g; // first match is empty
            re.constructor = {
                [Symbol.species]: function () {
                    return {
                        exec: function (input) {
                            return { 0: "", index: 0, input, length: 1 };
                        },
                        get lastIndex() { return v; },
                        set lastIndex(x) { setValues.push(x); v = x; }
                    };
                }
            };
            var iter = re[Symbol.matchAll]("ab");
            iter.next();
            // set 0: the lastIndex transfer Set at @@matchAll time; set 1: the
            // empty-match advance through AdvanceStringIndex
            assert.areEqual("0,1", setValues.join(","), "both Sets went through the accessor");
        }
    },
    {
        name: "Symbol.replace coerces a non-undefined groups value (groups-coercion edge)",
        body() {
            var r = /./;
            var coercible = { length: 1, 0: "", index: 0, groups: null };
            r.exec = function () { return coercible; };
            assert.throws(() => r[Symbol.replace]("bar", ""), TypeError, "ToObject(null) throws");
        }
    },
    {
        name: "Symbol.search Sets lastIndex only when the value changes (non-writable +0 succeeds)",
        body() {
            // A non-writable "lastIndex" that is already +0 is neither reset
            // nor restored: both spec Sets are guarded by SameValue.
            var r0 = /a/;
            r0.lastIndex = 0;
            Object.defineProperty(r0, "lastIndex", { writable: false });
            assert.areEqual(1, "bab".search(r0), "no Set for a +0 lastIndex");
            assert.areEqual(0, r0.lastIndex, "lastIndex untouched");

            // value != 0: the reset Set(rx, +0) runs and throws
            var r1 = /a/;
            r1.lastIndex = 1;
            Object.defineProperty(r1, "lastIndex", { writable: false });
            assert.throws(() => "bab".search(r1), TypeError, "reset Set on non-writable value 1");

            // a matching global regexp advances "lastIndex" inside exec; that
            // Set throws on a non-writable value before any restore
            var g = /b/g;
            Object.defineProperty(g, "lastIndex", { writable: false });
            assert.throws(() => "abc".search(g), TypeError, "exec advance Set throws");

            // writable lastIndex is reset for exec and restored afterwards
            var w = /b/;
            w.lastIndex = 2;
            assert.areEqual(1, "abc".search(w));
            assert.areEqual(2, w.lastIndex, "previous lastIndex restored");
            var g2 = /b/g;
            assert.areEqual(1, "abc".search(g2));
            assert.areEqual(0, g2.lastIndex, "global exec advance restored to the previous +0");
        }
    },
    {
        name: "@@matchAll transfers the unclamped ToLength lastIndex to the matcher",
        body() {
            var seen;
            var re = /a/g;
            re.lastIndex = 1e300;
            re.constructor = {
                [Symbol.species]: function () {
                    return {
                        exec: function () { return null; },
                        get lastIndex() { return 0; },
                        set lastIndex(v) { seen = v; }
                    };
                }
            };
            re[Symbol.matchAll]("aa");
            assert.areEqual(9007199254740991, seen, "ToLength(1e300), not the CharCount clamp 2147483646");
        }
    },
    {
        name: "exec with lastIndex beyond the string length",
        body() {
            // non-global: "lastIndex" is never written, not even a non-writable one
            var r = /b/;
            r.lastIndex = 3;
            Object.defineProperty(r, "lastIndex", { writable: false });
            assert.areEqual(1, r.exec("ab").index);
            assert.areEqual(3, r.lastIndex, "non-global exec leaves lastIndex alone");

            // global/sticky with lastIndex > length: the failure reset goes
            // through the observable Set and throws on a non-writable value
            var g = /b/g;
            g.lastIndex = 3;
            Object.defineProperty(g, "lastIndex", { writable: false });
            assert.throws(() => g.exec("ab"), TypeError, "global reset Set throws");
            var y = /b/y;
            y.lastIndex = 3;
            Object.defineProperty(y, "lastIndex", { writable: false });
            assert.throws(() => y.exec("ab"), TypeError, "sticky reset Set throws");

            // writable global resets to 0 through [[Set]] and returns null
            var g2 = /b/g;
            g2.lastIndex = 3;
            assert.areEqual(null, g2.exec("ab"));
            assert.areEqual(0, g2.lastIndex, "reset to 0");

            // lastIndex clamping above MaxCharCount (1e300 -> 2^53-1): the
            // early-out reset is observable as well
            var g3 = /b/g;
            g3.lastIndex = 1e300;
            assert.areEqual(null, g3.exec("ab"));
            assert.areEqual(0, g3.lastIndex, "clamped lastIndex reset through [[Set]]");
            var g4 = /b/g;
            g4.lastIndex = 1e300;
            Object.defineProperty(g4, "lastIndex", { writable: false });
            assert.throws(() => g4.exec("ab"), TypeError, "huge non-writable reset Set throws");

            // non-global ignores even a huge lastIndex
            var n = /b/;
            n.lastIndex = 1e300;
            assert.areEqual(1, n.exec("ab").index);
            assert.areEqual(1e300, n.lastIndex, "non-global never writes");
        }
    },
    {
        // S9-b repair: fullUnicode must cover `v` (ES2026 @@matchAll derives
        // it from flags containing "u" OR "v"). The `unicode` accessor alone
        // reports false for a v-only regex, so before the fix an empty-capable
        // v pattern ping-ponged forever between the iterator's +1 empty-match
        // advance and the S9-b pair-start snap in Matcher::Match
        // ("\uDE00?" over "\uD83D\uDE00X" re-matched index 0 from lastIndex 1).
        // Every assertion below was mirrored in Node 26.5.0 first.
        name: "matchAll fullUnicode covers the v flag (S9-b pair-start snap)",
        body() {
            var input = "\ud83d\ude00X"; // [pair][X]

            function collect(re, s) {
                var out = [];
                var guard = 0;
                for (var m of s.matchAll(re)) {
                    out.push([m[0].length, m.index]);
                    if (++guard > 20) { out.push(["GUARD"]); break; }
                }
                return out;
            }

            // THE hang shape: v empty-capable pattern over a surrogate pair.
            assert.areEqual("0,0|0,2|0,3",
                collect(/\uDE00?/gv, input).map(function (x) { return x.join(","); }).join("|"),
                "v empty-capable matchAll terminates on code-point boundaries");
            // controls: u (already full-Unicode) and the per-unit no-flag walk
            assert.areEqual("0,0|0,2|0,3",
                collect(/\uDE00?/gu, input).map(function (x) { return x.join(","); }).join("|"),
                "u empty-capable matchAll (control)");
            assert.areEqual("0,0|1,1|0,2|0,3",
                collect(/\uDE00?/g, input).map(function (x) { return x.join(","); }).join("|"),
                "no-flag empty-capable matchAll keeps the per-unit walk");
            // v trail-literal and pair-literal matchAll stay pair-aware
            assert.areEqual("", collect(/\uDE00X/gv, input).join(","), "v trail literal finds nothing in the pair");
            assert.areEqual("2,0", collect(/\ud83d\ude00/gv, input).map(function (x) { return x.join(","); }).join(","), "v pair literal matchAll");
            assert.areEqual("2,0", collect(/\ud83d\ude00/gu, input).map(function (x) { return x.join(","); }).join(","), "u pair literal matchAll");
            // done/count over the v walk: three empty matches then done
            var count = 0, first = -1, done = false;
            var it = input.matchAll(/X?/gv);
            var res;
            while (!(res = it.next()).done) {
                if (first < 0) { first = res.value.index; }
                count++;
            }
            assert.areEqual(3, count, "v empty walk yields one match per code point");
            assert.areEqual(0, first, "first v empty match is at 0");
            assert.isTrue(done === false && res.value === undefined, "iterator finishes done with undefined");
            // d-flag indices sit on code-point boundaries (X is at [2,3))
            var idx = [];
            for (var m of input.matchAll(/X/dgv)) { idx.push(m.indices[0].join(",")); }
            assert.areEqual("2,3", idx.join("|"), "v d-flag indices");
            // non-global still throws, v or not
            assert.throws(function () { input.matchAll(/\uDE00?/v); }, TypeError, "non-global v matchAll throws");
            assert.throws(function () { input.matchAll(/\uDE00?/u); }, TypeError, "non-global u matchAll throws");
            // manual mid-pair lastIndex transfers into the v walk (snap applies)
            var seeded = /\uDE00?/gv;
            seeded.lastIndex = 1;
            var seededIt = seeded[Symbol.matchAll](input);
            assert.areEqual(0, seededIt.next().value.index, "v matchAll with mid-pair lastIndex snaps to the pair start");
            // subclass instances take the same v path
            var Sub = class extends RegExp {};
            var subOut = [];
            var subGuard = 0;
            for (var sm of input.matchAll(new Sub("\\uDE00?", "gv"))) {
                subOut.push(sm.index);
                if (++subGuard > 20) { subOut.push("GUARD"); break; }
            }
            assert.areEqual("0,2,3", subOut.join(","), "v subclass matchAll terminates identically");
            // Symbol.matchAll directly on a v regex (flags-string path)
            var direct = [];
            var dGuard = 0;
            for (var dm of /\uDE00?/gv[Symbol.matchAll](input)) {
                direct.push(dm.index);
                if (++dGuard > 20) { direct.push("GUARD"); break; }
            }
            assert.areEqual("0,2,3", direct.join(","), "direct Symbol.matchAll v walk");
        }
    },
    {
        // S9-b repair iteration 2: @@match/@@replace/@@split derive global and
        // fullUnicode ('u' OR 'v') from ONE observable Get(R, "flags") +
        // ToString (ES2026 22.2.6.8/22.2.6.11), with @@matchAll's substrate
        // fallback when the accessor is not installed. Before this, a
        // subclassed v regex under -ES6RegExSymbols alone hung in match/
        // replace/replaceAll (the flags properties were absent, so the
        // empty-match advance stepped +1 against the S9-b pair-start snap)
        // and split stringified undefined and threw a SyntaxError building
        // the splitter. Every value mirrors Node 26.5.0.
        name: "subclass v builtins derive fullUnicode from flags (match/replace/split)",
        body() {
            var input = "\ud83d\ude00X"; // [pair][X]
            var Sub = class extends RegExp {};

            // the previously-hanging empty-capable walks terminate exactly
            assert.areEqual(",,", input.match(new Sub("\\uDE00?", "gv")).join(","), "subclass v match");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replace(new Sub("\\uDE00?", "gv"), "Q"), "subclass v replace");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replaceAll(new Sub("\\uDE00?", "gv"), "Q"), "subclass v replaceAll");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replace(new Sub("\\uDE00?", "gv"), function () { return "Q"; }), "subclass v replace function");
            assert.areEqual("\ud83d\ude00,X", input.split(new Sub("\\uDE00?", "gv")).join(","), "subclass v empty split");
            assert.areEqual("\ud83d\ude00,", input.split(new Sub("X", "gv")).join(","), "subclass v literal split");
            assert.areEqual("\ud83d\ude00", input.match(new Sub("\ud83d\ude00", "gv")).join(","), "subclass v pair-literal match");
            // controls: u (already full-Unicode) and the per-unit no-flag walk
            assert.areEqual(",,", input.match(new Sub("\\uDE00?", "gu")).join(","), "subclass u match (control)");
            assert.areEqual("Q\ud83d\ude00QXQ", input.replace(new Sub("\\uDE00?", "gu"), "Q"), "subclass u replace (control)");
            assert.areEqual(",\ude00,,", input.match(new Sub("\\uDE00?", "g")).join(","), "subclass no-flag match keeps the per-unit walk");
            assert.areEqual("Q\ud83dQQXQ", input.replace(new Sub("\\uDE00?", "g"), "Q"), "subclass no-flag replace keeps the per-unit walk");
            // one observable flags read per builtin, never re-read per
            // iteration, and the value is used (not the compiled flags)
            var reads = [];
            var recording = new Sub("\\uDE00?", "gv");
            Object.defineProperty(recording, "flags", { get() { reads.push("flags"); return "gv"; }, configurable: true });
            assert.areEqual(",,", input.match(recording).join(","), "recording-flags match result");
            assert.areEqual("flags", reads.join(","), "flags read exactly once by @@match");
            reads.length = 0;
            var counting = new Sub("X", "gv");
            var count = 0;
            Object.defineProperty(counting, "flags", { get() { count++; return "gv"; }, configurable: true });
            input.replace(counting, "Q");
            input.split(counting);
            assert.areEqual(2, count, "one flags read per replace/split builtin");
            // a user-provided observable value wins over the compiled flags:
            // an own "gu" value on a v regex walks per u
            var lying = new Sub("\\uDE00?", "gv");
            Object.defineProperty(lying, "flags", { value: "gu", configurable: true });
            assert.areEqual(",,", input.match(lying).join(","), "own flags value is used");
            // object flags are stringified
            var stringified = new Sub("\\uDE00?", "gv");
            Object.defineProperty(stringified, "flags", { value: { toString() { return "gv"; } }, configurable: true });
            assert.areEqual(",,", input.match(stringified).join(","), "object flags is ToString'd");
            // a throwing flags accessor propagates
            var throwing = new Sub("\\uDE00?", "gv");
            Object.defineProperty(throwing, "flags", { get() { throw new RangeError("boom"); }, configurable: true });
            assert.throws(function () { input.match(throwing); }, RangeError, "throwing flags propagates from @@match");
            assert.throws(function () { input.split(throwing); }, RangeError, "throwing flags propagates from @@split");
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
