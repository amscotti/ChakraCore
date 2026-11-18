//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.Segmenter, %Segments% and %SegmentIterator% (Track I1).
// Covers granularities, astral/ZWJ clusters, reentrant containing() with GC, concurrent iterators over one
// %Segments%, cross-realm creation and GC finalization of undrained objects (run also with -CollectGarbage).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const runGC = typeof CollectGarbage === "function" ? CollectGarbage : function () { };

function collectAll(segments) {
    const out = [];
    for (const data of segments) {
        out.push(data);
    }
    return out;
}

function texts(segments) {
    return collectAll(segments).map(function (d) { return d.segment; });
}

function churn() {
    // Allocate enough short-lived garbage that a real collection is likely to run.
    let junk = [];
    for (let i = 0; i < 2000; i++) {
        junk.push("churn" + i + "x".repeat(i % 7));
    }
    runGC();
    return junk.length;
}

const family = "\u{1F468}\u200D\u{1F469}\u200D\u{1F467}"; // ZWJ sequence, 8 UTF-16 units
const flag = "\u{1F1FA}\u{1F1F8}"; // regional indicator pair
const skin = "\u{1F44B}\u{1F3FB}"; // waving hand + light skin tone

const tests = [
    {
        name: "Constructor, resolvedOptions and descriptors",
        body: function () {
            const s = new Intl.Segmenter("en-us", { granularity: "word", lineBreakStyle: "loose" });
            const ro = s.resolvedOptions();
            assert.areEqual("en-US", ro.locale, "locale");
            assert.areEqual("word", ro.granularity, "granularity");
            assert.areEqual(["locale", "granularity"], Object.getOwnPropertyNames(ro), "property order, no lineBreakStyle");
            assert.isTrue(ro !== s.resolvedOptions(), "fresh object each call");
            assert.areEqual("grapheme", new Intl.Segmenter().resolvedOptions().granularity, "default granularity");
            assert.areEqual("sentence", new Intl.Segmenter(undefined, { granularity: "sentence" }).resolvedOptions().granularity);

            assert.throws(function () { Intl.Segmenter(); }, TypeError, "call without new");
            assert.throws(function () { new Intl.Segmenter("en", { granularity: "line" }); }, RangeError, "bad granularity");
            assert.throws(function () { new Intl.Segmenter("en", { localeMatcher: "x" }); }, RangeError, "bad localeMatcher");
            assert.throws(function () { new Intl.Segmenter("en", null); }, TypeError, "null options");
            assert.throws(function () { new Intl.Segmenter("not a locale"); }, RangeError, "bad locale");
            assert.areEqual("grapheme", new Intl.Segmenter("en", {}).resolvedOptions().granularity);

            assert.areEqual(0, Intl.Segmenter.length, "ctor length");
            assert.areEqual("Segmenter", Intl.Segmenter.name, "ctor name");
            assert.areEqual("Intl.Segmenter", Intl.Segmenter.prototype[Symbol.toStringTag], "toStringTag");
            assert.areEqual(1, Intl.Segmenter.supportedLocalesOf.length);
            assert.areEqual(1, Intl.Segmenter.prototype.segment.length);
            assert.areEqual(0, Intl.Segmenter.prototype.resolvedOptions.length);
            assert.isTrue(Array.isArray(Intl.Segmenter.supportedLocalesOf(["en-US", "zz-ZZ"])), "array result");

            const segments = s.segment("abc");
            const segProto = Object.getPrototypeOf(segments);
            assert.areEqual("containing", segProto.containing.name);
            assert.areEqual(1, segProto.containing.length);
            assert.areEqual("[Symbol.iterator]", segProto[Symbol.iterator].name, "%Segments%[@@iterator] name");
            assert.areEqual(0, segProto[Symbol.iterator].length);
            const desc = Object.getOwnPropertyDescriptor(segProto, "containing");
            assert.isTrue(desc.writable && !desc.enumerable && desc.configurable, "containing attributes");
            const iterDesc = Object.getOwnPropertyDescriptor(segProto, Symbol.iterator);
            assert.isTrue(iterDesc.writable && !iterDesc.enumerable && iterDesc.configurable, "@@iterator attributes");
            assert.isTrue(Object.prototype === Object.getPrototypeOf(segProto), "%Segments.prototype% proto");
            assert.isFalse(segProto.hasOwnProperty("constructor"), "no constructor");

            const iter = segments[Symbol.iterator]();
            const iterProto = Object.getPrototypeOf(iter);
            assert.areEqual("next", iterProto.next.name);
            assert.areEqual(0, iterProto.next.length);
            assert.areEqual("Segmenter String Iterator", iterProto[Symbol.toStringTag]);
            assert.isTrue(Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())) === Object.getPrototypeOf(iterProto), "inherits %IteratorPrototype%");
            assert.isTrue(iter === iter[Symbol.iterator](), "iterator is its own iterable");
        }
    },
    {
        name: "Invalid receivers throw TypeError",
        body: function () {
            const segmenter = new Intl.Segmenter();
            const segments = segmenter.segment("abc");
            const iter = segments[Symbol.iterator]();
            const bad = [undefined, null, true, "", Symbol(), 1, {}, Intl.Segmenter, Intl.Segmenter.prototype,
                segmenter, segments, iter, new Intl.Collator()];
            bad.forEach(function (b, i) {
                if (b !== segmenter) {
                    assert.throws(function () { Intl.Segmenter.prototype.segment.call(b, "x"); }, TypeError, "segment " + i);
                    assert.throws(function () { Intl.Segmenter.prototype.resolvedOptions.call(b); }, TypeError, "resolvedOptions " + i);
                }
                if (b !== segments) {
                    const proto = Object.getPrototypeOf(segments);
                    assert.throws(function () { proto.containing.call(b, 0); }, TypeError, "containing " + i);
                    assert.throws(function () { proto[Symbol.iterator].call(b); }, TypeError, "@@iterator " + i);
                }
                if (b !== iter) {
                    const iterProto = Object.getPrototypeOf(iter);
                    assert.throws(function () { iterProto.next.call(b); }, TypeError, "next " + i);
                }
            });
        }
    },
    {
        name: "Grapheme segmentation: combining marks, ZWJ, flags, CRLF, astral",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "grapheme" });
            assert.areEqual(["e\u0301", "x"], texts(seg.segment("e\u0301x")), "combining mark");
            assert.areEqual([family], texts(seg.segment(family)), "ZWJ family is one cluster");
            assert.areEqual(8, family.length);
            assert.areEqual([flag, flag], texts(seg.segment(flag + flag)), "two flags");
            assert.areEqual([skin], texts(seg.segment(skin)), "modifier sequence");
            assert.areEqual(["\r\n", "a"], texts(seg.segment("\r\na")), "CRLF");
            assert.areEqual(["\u{10000}", "\u{10001}"], texts(seg.segment("\u{10000}\u{10001}")), "astral");
            assert.areEqual(["\u1102\u1162\u11A9"], texts(seg.segment("\u1102\u1162\u11A9")), "Hangul jamo LVT");

            const all = collectAll(seg.segment("a" + family + "b"));
            assert.areEqual(3, all.length);
            assert.areEqual([0, 1, 9], all.map(function (d) { return d.index; }), "indices are UTF-16 offsets");
            all.forEach(function (d) {
                assert.areEqual("a" + family + "b", d.input, "input");
                assert.isFalse(d.hasOwnProperty("isWordLike"), "no isWordLike for grapheme");
                assert.areEqual(["segment", "index", "input"], Object.getOwnPropertyNames(d), "order");
            });
            assert.areEqual(0, collectAll(seg.segment("")).length, "empty string");
            assert.areEqual(["\udc00", "\ud800"], texts(seg.segment("\udc00\ud800")), "unpaired surrogates split");
        }
    },
    {
        name: "Word segmentation and isWordLike",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "word" });
            const all = collectAll(seg.segment("Hello, world 42!"));
            assert.areEqual(["Hello", ",", " ", "world", " ", "42", "!"], all.map(function (d) { return d.segment; }));
            assert.areEqual([true, false, false, true, false, true, false], all.map(function (d) { return d.isWordLike; }));
            all.forEach(function (d) {
                assert.areEqual(["segment", "index", "input", "isWordLike"], Object.getOwnPropertyNames(d));
                const desc = Object.getOwnPropertyDescriptor(d, "isWordLike");
                assert.isTrue(desc.writable && desc.enumerable && desc.configurable, "data property attributes");
            });
            const emoji = collectAll(seg.segment("a " + family));
            assert.areEqual(3, emoji.length, "emoji ZWJ is a single word-break segment");
            assert.areEqual(family, emoji[2].segment);
            assert.areEqual("\u53f0\u5317", collectAll(new Intl.Segmenter("zh", { granularity: "word" }).segment("\u53f0\u5317"))
                .map(function (d) { return d.segment; }).join(""), "CJK round trips");
        }
    },
    {
        name: "Sentence segmentation",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "sentence" });
            const text = "Hi there. How are you? Fine!";
            assert.areEqual(["Hi there. ", "How are you? ", "Fine!"], texts(seg.segment(text)));
            assert.areEqual(text, texts(seg.segment(text)).join(""));
            const data = collectAll(seg.segment(text));
            assert.areEqual([0, 10, 23], data.map(function (d) { return d.index; }));
            assert.isFalse(data[0].hasOwnProperty("isWordLike"));
        }
    },
    {
        name: "containing(): boundaries, coercion and bounds",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "grapheme" });
            const text = "a" + family + "b";
            const segments = seg.segment(text);
            const first = segments.containing(0);
            assert.areEqual("a", first.segment);
            assert.areEqual(0, first.index);
            assert.areEqual(text, first.input);
            for (let i = 1; i < 9; i++) {
                const r = segments.containing(i);
                assert.areEqual(1, r.index, "inside cluster index " + i);
                assert.areEqual(family, r.segment, "inside cluster segment " + i);
            }
            assert.areEqual("b", segments.containing(9).segment);
            assert.isUndefined(segments.containing(-1), "negative");
            assert.isUndefined(segments.containing(10), "== length");
            assert.isUndefined(segments.containing(Infinity), "Infinity");
            assert.isUndefined(segments.containing(-Infinity), "-Infinity");
            assert.areEqual(0, segments.containing(NaN).index, "NaN -> 0");
            assert.areEqual(0, segments.containing().index, "undefined -> 0");
            assert.areEqual(0, segments.containing("0").index, "string -> 0");
            assert.areEqual(1, segments.containing(1.9).index, "truncation");
            assert.areEqual(0, segments.containing(-0.9).index, "-0.9 -> -0 -> in range");
            assert.areEqual(0, segments.containing(null).index, "null -> 0");
            assert.throws(function () { segments.containing(Symbol()); }, TypeError, "Symbol");
            assert.throws(function () { segments.containing(1n); }, TypeError, "BigInt");

            const empty = seg.segment("");
            assert.isUndefined(empty.containing(0), "empty string");

            const wordSegments = new Intl.Segmenter("en", { granularity: "word" }).segment("ab cd");
            const w = wordSegments.containing(1);
            assert.areEqual("ab", w.segment);
            assert.areEqual(true, w.isWordLike);
            assert.areEqual(false, wordSegments.containing(2).isWordLike);
            assert.areEqual(["segment", "index", "input", "isWordLike"], Object.getOwnPropertyNames(w));

            // lone surrogates / low surrogate of a pair
            const pair = seg.segment("x\u{1F600}y");
            assert.areEqual("\u{1F600}", pair.containing(2).segment);
            assert.areEqual(1, pair.containing(2).index);
        }
    },
    {
        name: "containing(): reentrant valueOf with GC, nesting and iteration",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "word" });
            const segments = seg.segment("Hello world, again!");
            let calls = 0;
            const evil = {
                valueOf: function () {
                    calls++;
                    churn();
                    // Reentrantly use the very same Segments, a fresh one, and an iterator.
                    const inner = segments.containing(6);
                    assert.areEqual("world", inner.segment);
                    assert.areEqual("again", seg.segment("again!").containing(2).segment);
                    const it = segments[Symbol.iterator]();
                    assert.areEqual("Hello", it.next().value.segment);
                    churn();
                    return 7;
                }
            };
            const r = segments.containing(evil);
            assert.areEqual(1, calls);
            assert.areEqual("world", r.segment);
            assert.areEqual(6, r.index);
            assert.areEqual("Hello world, again!", r.input);

            // valueOf that throws leaves everything usable.
            assert.throws(function () {
                segments.containing({ valueOf: function () { churn(); throw new RangeError("boom"); } });
            }, RangeError, "throwing valueOf");
            assert.areEqual("Hello", segments.containing(0).segment);

            // valueOf that recursively calls containing(valueOf-object) a few levels deep.
            let depth = 0;
            const nest = {
                valueOf: function () {
                    if (depth++ < 3) {
                        return segments.containing(nest).index;
                    }
                    return 0;
                }
            };
            assert.areEqual(0, segments.containing(nest).index);
        }
    },
    {
        name: "Concurrent iterators over one Segments and mix with containing()",
        body: function () {
            const segments = new Intl.Segmenter("en", { granularity: "word" }).segment("one two three");
            const a = segments[Symbol.iterator]();
            const b = segments[Symbol.iterator]();
            assert.isTrue(a !== b, "distinct iterators");
            assert.areEqual("one", a.next().value.segment);
            assert.areEqual("one", b.next().value.segment);
            assert.areEqual(" ", a.next().value.segment);
            assert.areEqual("two", segments.containing(5).segment, "containing between next() calls");
            assert.areEqual(" ", b.next().value.segment);
            assert.areEqual("two", a.next().value.segment);
            assert.areEqual("two", b.next().value.segment);
            churn();
            const rest = [];
            for (let r = a.next(); !r.done; r = a.next()) { rest.push(r.value.segment); }
            assert.areEqual([" ", "three"], rest);
            const doneResult = a.next();
            assert.isTrue(doneResult.done, "stays done");
            assert.isUndefined(doneResult.value, "value undefined when done");
            assert.areEqual(["done", "value"].sort(), Object.getOwnPropertyNames(doneResult).sort());
            assert.areEqual([" ", "three"], [b.next().value.segment, b.next().value.segment], "b independent of a");

            // nested for-of over one Segments
            const abcd = new Intl.Segmenter().segment("ABCD");
            let out = "";
            for (const v1 of abcd) {
                for (const v2 of abcd) { out += v1.segment + v2.segment; }
                out += ":";
            }
            assert.areEqual("AAABACAD:BABBBCBD:CACBCCCD:DADBDCDD:", out);

            // a fresh iterator restarts
            assert.areEqual("A", abcd[Symbol.iterator]().next().value.segment);
        }
    },
    {
        name: "Non-flat strings and long inputs survive GC between calls",
        body: function () {
            let rope = "";
            for (let i = 0; i < 300; i++) { rope += "ab" + i + " "; }
            const segments = new Intl.Segmenter("en", { granularity: "word" }).segment(rope);
            churn();
            assert.areEqual(rope, segments.containing(0).input);
            const it = segments[Symbol.iterator]();
            let joined = "";
            let n = 0;
            for (let r = it.next(); !r.done; r = it.next()) {
                joined += r.value.segment;
                if ((n++ & 63) === 0) { churn(); }
            }
            assert.areEqual(rope, joined, "segments partition the input");
            const big = "x ".repeat(20000);
            let count = 0;
            for (const d of new Intl.Segmenter("en", { granularity: "word" }).segment(big)) { count++; }
            assert.areEqual(40000, count);
        }
    },
    {
        name: "segment() coerces its argument with ToString",
        body: function () {
            const seg = new Intl.Segmenter("en", { granularity: "word" });
            const cases = [[[], "undefined"], [[null], "null"], [[12], "12"], [[["a", "b"]], "a"], [[{}], "["]];
            cases.forEach(function (c) {
                assert.areEqual(c[1], [...seg.segment(...c[0])][0].segment);
            });
            assert.throws(function () { seg.segment(Symbol()); }, TypeError);
            let calls = 0;
            const obj = { toString: function () { calls++; churn(); return "abc"; } };
            const segments = seg.segment(obj);
            assert.areEqual(1, calls, "toString called once, eagerly");
            assert.areEqual("abc", segments.containing(0).input);
            assert.areEqual("abc", segments.containing(1).input);
            assert.areEqual(1, calls, "not called again");
        }
    },
    {
        name: "Cross-realm behavior",
        body: function () {
            const other = WScript.LoadScript("this", "samethread");
            const here = new Intl.Segmenter("en");
            const there = new other.Intl.Segmenter("en", { granularity: "word" });

            // Calling this realm's method on the other realm's instance: result objects use the callee's realm.
            const mixed = Intl.Segmenter.prototype.segment.call(there, "ab cd");
            assert.isTrue(Object.getPrototypeOf(here.segment("x")) === Object.getPrototypeOf(mixed), "Segments proto from callee realm");
            assert.isTrue(Object.getPrototypeOf(there.segment("x")) !== Object.getPrototypeOf(mixed), "not the instance's realm");
            assert.areEqual("ab", mixed.containing(0).segment);
            assert.isTrue(mixed.containing(0).isWordLike, "word granularity from the instance");

            // Foreign realm's Segments methods work on this realm's Segments (brand check is realm independent).
            const foreignContaining = Object.getPrototypeOf(there.segment("x")).containing;
            assert.areEqual("a", foreignContaining.call(new Intl.Segmenter().segment("ab"), 0).segment);

            // newTarget from another realm without an object prototype uses that realm's intrinsic.
            const C = new other.Function();
            C.prototype = null;
            const o = Reflect.construct(Intl.Segmenter, [], C);
            assert.isTrue(other.Intl.Segmenter.prototype === Object.getPrototypeOf(o), "proto-from-ctor-realm");
            const D = function () { };
            D.prototype = null;
            const wrapped = Reflect.construct(Intl.Segmenter, [], D);
            assert.isTrue(Intl.Segmenter.prototype === Object.getPrototypeOf(wrapped), "same-realm fallback");
            assert.areEqual("grapheme", o.resolvedOptions().granularity);
            // The instance produced above must still be usable from either realm.
            assert.areEqual(["a", "b"], [...Intl.Segmenter.prototype.segment.call(o, "ab")].map(function (d) { return d.segment; }));
        }
    },
    {
        name: "GC finalization of undrained Segmenter, Segments and SegmentIterator",
        body: function () {
            let alive = 0;
            for (let round = 0; round < 40; round++) {
                for (let i = 0; i < 50; i++) {
                    const seg = new Intl.Segmenter("en", { granularity: ["grapheme", "word", "sentence"][i % 3] });
                    const segments = seg.segment("one two. three " + i);
                    const it = segments[Symbol.iterator]();
                    if (i % 2) { it.next(); }
                    if (i % 5 === 0) { segments.containing(1); }
                    // Intentionally never drained, never closed.
                    alive += (it ? 1 : 0);
                }
                if (round % 8 === 0) { churn(); }
            }
            churn();
            assert.areEqual(2000, alive);

            // Survivors across a collection keep working; unreachable siblings are finalized.
            const keep = new Intl.Segmenter("en", { granularity: "word" }).segment("keep going");
            const keepIt = keep[Symbol.iterator]();
            for (let i = 0; i < 20; i++) { new Intl.Segmenter().segment("garbage " + i)[Symbol.iterator](); }
            churn();
            assert.areEqual("keep", keepIt.next().value.segment);
            assert.areEqual("going", keep.containing(5).segment);
            churn();
            assert.areEqual(" ", keepIt.next().value.segment);
        }
    },
    {
        name: "Subclassing and prototype wiring",
        body: function () {
            class MySegmenter extends Intl.Segmenter { }
            const m = new MySegmenter("en", { granularity: "word" });
            assert.isTrue(MySegmenter.prototype === Object.getPrototypeOf(m), "subclass proto");
            assert.areEqual("word", m.resolvedOptions().granularity);
            assert.areEqual("hi", m.segment("hi").containing(0).segment);
            assert.areEqual("[object Intl.Segmenter]", Object.prototype.toString.call(m));
            assert.areEqual("[object Segmenter String Iterator]", Object.prototype.toString.call(m.segment("a")[Symbol.iterator]()));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
