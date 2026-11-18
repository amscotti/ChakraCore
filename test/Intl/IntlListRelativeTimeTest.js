//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.ListFormat and Intl.RelativeTimeFormat native state:
// iterator abrupt-close reentrancy, -0, numberingSystem reconciliation, cross-realm
// prototypes, and GC finalization of the owned ICU handles.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function collect() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function makeIterable(values, hooks) {
    hooks = hooks || {};
    let i = 0;
    const it = {
        returnCalls: 0,
        nextCalls: 0,
        [Symbol.iterator]() { return this; },
        next() {
            this.nextCalls++;
            if (hooks.onNext) {
                hooks.onNext(this.nextCalls);
            }
            if (i >= values.length) {
                return { done: true };
            }
            return { done: false, value: values[i++] };
        },
        "return"() {
            this.returnCalls++;
            if (hooks.onReturn) {
                return hooks.onReturn();
            }
            return {};
        },
    };
    return it;
}

const tests = [
    {
        name: "ListFormat basic format and formatToParts",
        body() {
            const lf = new Intl.ListFormat("en-US");
            assert.areEqual("", lf.format([]), "empty list");
            assert.areEqual("a", lf.format(["a"]), "single");
            assert.areEqual("a and b", lf.format(["a", "b"]), "pair");
            assert.areEqual("a, b, and c", lf.format(["a", "b", "c"]), "triple");
            assert.areEqual("", lf.format(), "undefined iterable is the empty list");

            const or = new Intl.ListFormat("en-US", { type: "disjunction" });
            assert.areEqual("a, b, or c", or.format(["a", "b", "c"]), "disjunction");

            const parts = lf.formatToParts(["a", "b"]);
            assert.areEqual(3, parts.length);
            assert.areEqual("element", parts[0].type);
            assert.areEqual("literal", parts[1].type);
            assert.areEqual(" and ", parts[1].value);
            assert.areEqual("b", parts[2].value);
            assert.areEqual(2, Object.keys(parts[0]).length, "part has only type and value");

            const resolved = new Intl.ListFormat("en", { type: "unit", style: "narrow" }).resolvedOptions();
            assert.areEqual("en", resolved.locale);
            assert.areEqual("unit", resolved.type);
            assert.areEqual("narrow", resolved.style);
            assert.areEqual("locale,type,style", Object.getOwnPropertyNames(resolved).join(","));
        }
    },
    {
        name: "ListFormat non-string element closes the iterator exactly once",
        body() {
            const lf = new Intl.ListFormat("en");
            const it = makeIterable(["a", 3, "c"]);
            assert.throws(() => lf.format(it), TypeError);
            assert.areEqual(2, it.nextCalls, "stops at the offending element");
            assert.areEqual(1, it.returnCalls, "return() called once");

            const it2 = makeIterable(["a", new String("b")]);
            assert.throws(() => lf.formatToParts(it2), TypeError, "String objects are not strings");
            assert.areEqual(1, it2.returnCalls);
        }
    },
    {
        name: "ListFormat iterator abrupt completion paths",
        body() {
            const lf = new Intl.ListFormat("en");

            // return() that throws must not mask the TypeError, and return() that is
            // not callable is ignored on the abrupt path.
            const throwing = makeIterable([1], { onReturn() { throw new RangeError("ignored"); } });
            assert.throws(() => lf.format(throwing), TypeError);
            assert.areEqual(1, throwing.returnCalls);

            // next() throwing is not closed (iterator is already broken).
            const failingNext = makeIterable(["a"], { onNext(n) { if (n === 2) { throw new EvalError("boom"); } } });
            assert.throws(() => lf.format(failingNext), EvalError);
            assert.areEqual(0, failingNext.returnCalls, "no close after next() throws");

            // A throwing @@iterator or non-iterable never reaches the native formatter.
            assert.throws(() => lf.format({ [Symbol.iterator]() { throw new SyntaxError("x"); } }), SyntaxError);
            assert.throws(() => lf.format(5), TypeError);
            assert.throws(() => lf.formatToParts(null), TypeError);

            // Strings are iterable by code point.
            assert.areEqual("f, o, and o", lf.format("foo"));
        }
    },
    {
        name: "ListFormat reentrancy: GC and nested formatting from next()",
        body() {
            const lf = new Intl.ListFormat("en");
            const inner = new Intl.ListFormat("en", { type: "disjunction" });
            let nested = null;
            const it = makeIterable(["x".repeat(40), "y".repeat(40), "z".repeat(40)], {
                onNext(n) {
                    collect();
                    for (let i = 0; i < 20; i++) {
                        new Intl.ListFormat("en").format(["p", "q"]);
                    }
                    if (n === 2) {
                        nested = inner.format(["1", "2"]);
                    }
                }
            });
            const out = lf.format(it);
            assert.areEqual("x".repeat(40) + ", " + "y".repeat(40) + ", and " + "z".repeat(40), out);
            assert.areEqual("1 or 2", nested);

            // The receiver is only reachable through the call frame while GC runs in next().
            const out2 = new Intl.ListFormat("en").format(makeIterable(["a", "b"], { onNext() { collect(); } }));
            assert.areEqual("a and b", out2);
        }
    },
    {
        name: "ListFormat and RelativeTimeFormat receiver and construction checks",
        body() {
            assert.throws(() => Intl.ListFormat.prototype.format.call({}, []), TypeError);
            assert.throws(() => Intl.ListFormat.prototype.resolvedOptions.call(new Intl.RelativeTimeFormat()), TypeError);
            assert.throws(() => Intl.RelativeTimeFormat.prototype.format.call(new Intl.ListFormat(), 1, "day"), TypeError);
            assert.throws(() => Intl.ListFormat(), TypeError);
            assert.throws(() => Intl.RelativeTimeFormat(), TypeError);
            assert.throws(() => new Intl.ListFormat("en", null), TypeError);
            assert.throws(() => new Intl.ListFormat("en", { type: "nope" }), RangeError);
            assert.throws(() => new Intl.RelativeTimeFormat("en", { numeric: "nope" }), RangeError);
            assert.areEqual("function", typeof Intl.ListFormat.supportedLocalesOf);
            assert.areEqual("en", Intl.RelativeTimeFormat.supportedLocalesOf("en")[0]);
        }
    },
    {
        name: "RelativeTimeFormat negative zero",
        body() {
            const rtf = new Intl.RelativeTimeFormat("en-US");
            assert.areEqual("in 0 days", rtf.format(0, "day"));
            assert.areEqual("0 days ago", rtf.format(-0, "day"));
            assert.areEqual("0 days ago", rtf.format("-0", "days"));
            assert.areEqual(rtf.format(-0, "second"), rtf.format(-0, "seconds"));

            const parts = rtf.formatToParts(-0, "day");
            assert.areEqual(2, parts.length);
            assert.areEqual("integer", parts[0].type);
            assert.areEqual("0", parts[0].value);
            assert.areEqual("day", parts[0].unit);
            assert.areEqual("literal", parts[1].type);
            assert.areEqual(" days ago", parts[1].value);
            assert.isFalse(parts[1].hasOwnProperty("unit"), "literal parts carry no unit");

            const auto = new Intl.RelativeTimeFormat("en-US", { numeric: "auto" });
            assert.areEqual("today", auto.format(0, "day"));
            assert.areEqual("yesterday", auto.format(-1, "day"));
        }
    },
    {
        name: "RelativeTimeFormat value and unit coercion order and errors",
        body() {
            const rtf = new Intl.RelativeTimeFormat("en-US");
            const order = [];
            const value = { valueOf() { order.push("value"); return 2; } };
            const unit = { toString() { order.push("unit"); return "hours"; } };
            assert.areEqual("in 2 hours", rtf.format(value, unit));
            assert.areEqual("value,unit", order.join(","));

            assert.throws(() => rtf.format(NaN, "day"), RangeError);
            assert.throws(() => rtf.format(Infinity, "day"), RangeError);
            assert.throws(() => rtf.format(-Infinity, "day"), RangeError);
            assert.throws(() => rtf.format(1, "decade"), RangeError);
            assert.throws(() => rtf.format(Symbol(), "day"), TypeError);
            assert.throws(() => rtf.format(1, Symbol()), TypeError);
            assert.throws(() => rtf.formatToParts(1), RangeError, "missing unit");

            // Coercion getters may run GC; the receiver and formatter must stay valid.
            let ok = null;
            ok = rtf.format({ valueOf() { collect(); return 5; } }, { toString() { collect(); return "minute"; } });
            assert.areEqual("in 5 minutes", ok);
        }
    },
    {
        name: "RelativeTimeFormat formatToParts number fields",
        body() {
            const rtf = new Intl.RelativeTimeFormat("en-US");
            const parts = rtf.formatToParts(-1234.5, "week");
            assert.areEqual(
                "integer:1:week|group:,:week|integer:234:week|decimal:.:week|fraction:5:week|literal: weeks ago:",
                parts.map(function (p) { return `${p.type}:${p.value}:${p.unit === undefined ? "" : p.unit}`; }).join("|"));

            const autoParts = new Intl.RelativeTimeFormat("en-US", { numeric: "auto" }).formatToParts(1, "day");
            assert.areEqual(1, autoParts.length);
            assert.areEqual("literal", autoParts[0].type);
            assert.areEqual("tomorrow", autoParts[0].value);
            assert.isFalse(autoParts[0].hasOwnProperty("unit"));
        }
    },
    {
        name: "RelativeTimeFormat honors CLDR minimum grouping digits",
        body() {
            // pl and es group only from five digits (minimumGroupingDigits = 2); en groups from four.
            assert.areEqual("za 1000 sekund", new Intl.RelativeTimeFormat("pl").format(1000, "second"));
            assert.areEqual("za 10\u00a0000 sekund", new Intl.RelativeTimeFormat("pl").format(10000, "second"));
            assert.areEqual("in 1,000 seconds", new Intl.RelativeTimeFormat("en").format(1000, "second"));
            const parts = new Intl.RelativeTimeFormat("pl").formatToParts(1000, "second");
            assert.areEqual(3, parts.length);
            assert.areEqual("integer", parts[1].type);
            assert.areEqual("1000", parts[1].value);
        }
    },
    {
        name: "RelativeTimeFormat numberingSystem option and extension reconciliation",
        body() {
            const cases = [
                // [locale, numberingSystem option, expected locale, expected nu]
                ["en", undefined, "en", "latn"],
                ["en", "arab", "en", "arab"],
                ["en", "abc", "en", "latn"],
                ["en-u-nu-arab", undefined, "en-u-nu-arab", "arab"],
                ["en-u-nu-arab", "arab", "en-u-nu-arab", "arab"],
                ["en-u-nu-arab", "invalid", "en-u-nu-arab", "arab"],
                ["en-u-nu-latn", "arab", "en", "arab"],
                ["en-u-nu-invalid", "invalid2", "en", "latn"],
            ];
            for (const [locale, nu, expectedLocale, expectedNu] of cases) {
                const options = nu === undefined ? {} : { numberingSystem: nu };
                const r = new Intl.RelativeTimeFormat(locale, options).resolvedOptions();
                assert.areEqual(expectedLocale, r.locale, `locale for ${locale}/${nu}`);
                assert.areEqual(expectedNu, r.numberingSystem, `numberingSystem for ${locale}/${nu}`);
            }

            const arab = new Intl.RelativeTimeFormat("en", { numberingSystem: "arab" });
            assert.areEqual("in \u0663 days", arab.format(3, "day"));
            const part = arab.formatToParts(3, "day")[1];
            assert.areEqual("integer", part.type);
            assert.areEqual("\u0663", part.value);

            const viaExtension = new Intl.RelativeTimeFormat("en-u-nu-arab");
            assert.areEqual(arab.format(3, "day"), viaExtension.format(3, "day"));

            assert.throws(() => new Intl.RelativeTimeFormat("en", { numberingSystem: "ab" }), RangeError);
            assert.throws(() => new Intl.RelativeTimeFormat("en", { numberingSystem: "latn-" }), RangeError);
            assert.throws(() => new Intl.RelativeTimeFormat("en", { numberingSystem: "lat\u00e9" }), RangeError);
            assert.areEqual("latn", new Intl.RelativeTimeFormat("en", { numberingSystem: "1234abcd-abc123" }).resolvedOptions().numberingSystem);

            const order = [];
            new Intl.RelativeTimeFormat("en", {
                get numeric() { order.push("numeric"); return undefined; },
                get style() { order.push("style"); return undefined; },
                get numberingSystem() { order.push("numberingSystem"); return undefined; },
                get localeMatcher() { order.push("localeMatcher"); return undefined; },
            });
            assert.areEqual("localeMatcher,numberingSystem,style,numeric", order.join(","));
        }
    },
    {
        name: "Cross-realm prototype resolution",
        body() {
            const other = WScript.LoadScript("", "samethread");
            assert.isTrue(other.Intl !== Intl, "separate realm");

            [["ListFormat", []], ["RelativeTimeFormat", []]].forEach(([name]) => {
                const newTarget = new other.Function();
                newTarget.prototype = undefined;
                const obj = Reflect.construct(Intl[name], [], newTarget);
                assert.isTrue(other.Intl[name].prototype === Object.getPrototypeOf(obj), `${name}: realm of newTarget`);
                // Methods of this realm accept state created through the other prototype.
                assert.areEqual("object", typeof obj.resolvedOptions());
            });

            const otherLf = new other.Intl.ListFormat("en");
            assert.areEqual("a and b", Intl.ListFormat.prototype.format.call(otherLf, ["a", "b"]),
                "state is shared across realms");
            const otherRtf = new other.Intl.RelativeTimeFormat("en");
            assert.areEqual("in 1 day", Intl.RelativeTimeFormat.prototype.format.call(otherRtf, 1, "day"));

            // A cross-realm iterable producing strings works; the realm's own iterator close is used.
            const it = other.eval("({ i: 0, r: 0, [Symbol.iterator]() { return this; }, next() { return this.i < 2 ? { done: false, value: 'a' + this.i++ } : { done: true }; }, return() { this.r++; return {}; } })");
            assert.areEqual("a0 and a1", new Intl.ListFormat("en").format(it));
            const bad = other.eval("({ r: 0, [Symbol.iterator]() { return this; }, next() { return { done: false, value: 1 }; }, return() { this.r++; return {}; } })");
            assert.throws(() => new Intl.ListFormat("en").format(bad), TypeError);
            assert.areEqual(1, bad.r, "cross-realm iterator is closed");
        }
    },
    {
        name: "GC finalization of ICU handles",
        body() {
            // Unreferenced instances are finalized (ulistfmt_close / ureldatefmt_close in Dispose).
            for (let round = 0; round < 5; round++) {
                for (let i = 0; i < 300; i++) {
                    const lf = new Intl.ListFormat(i % 2 ? "de" : "en", { type: i % 3 ? "unit" : "conjunction", style: "short" });
                    lf.format(["a", "b", "c"]);
                    const rtf = new Intl.RelativeTimeFormat(i % 2 ? "pl" : "en", { numeric: "auto", style: "narrow" });
                    rtf.format(i - 150, "day");
                    rtf.formatToParts(i, "hour");
                }
                collect();
            }

            // Survivors across collections keep working and keep their retained JS strings.
            const lf = new Intl.ListFormat("es", { style: "long" });
            const rtf = new Intl.RelativeTimeFormat("de", { style: "long" });
            const before = [lf.format(["a", "b", "c"]), rtf.format(2, "day"), JSON.stringify(lf.resolvedOptions()), JSON.stringify(rtf.resolvedOptions())];
            for (let i = 0; i < 3; i++) {
                for (let j = 0; j < 500; j++) {
                    new Intl.ListFormat().resolvedOptions();
                    new Intl.RelativeTimeFormat().resolvedOptions();
                }
                collect();
            }
            const after = [lf.format(["a", "b", "c"]), rtf.format(2, "day"), JSON.stringify(lf.resolvedOptions()), JSON.stringify(rtf.resolvedOptions())];
            assert.areEqual(before.join("\n"), after.join("\n"));
            assert.areEqual("a, b y c", before[0]);
            assert.areEqual("in 2 Tagen", before[1]);
        }
    },
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
