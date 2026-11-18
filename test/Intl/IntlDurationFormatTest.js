//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.DurationFormat: constructor options, unit defaults/conflicts,
// fractionalDigits, resolvedOptions key order, ToDurationRecord (object & ISO string),
// exact __int128 range/nanosecond math, trunc rounding, -0, formatToParts, locales/nu,
// reentrancy, cross-realm prototypes, and GC finalization.
// Run also with: ch -CollectGarbage IntlDurationFormatTest.js

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function collect() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function partsSignature(parts) {
    return parts.map(function (p) {
        const hasUnit = Object.prototype.hasOwnProperty.call(p, "unit");
        return p.type + ":" + JSON.stringify(p.value) + ":" + (hasUnit ? p.unit : "-");
    }).join("|");
}

const tests = [
    {
        name: "Constructor, prototype, descriptors, and supportedLocalesOf",
        body() {
            assert.areEqual("function", typeof Intl.DurationFormat);
            assert.areEqual("DurationFormat", Intl.DurationFormat.name);
            assert.areEqual(0, Intl.DurationFormat.length);
            assert.areEqual(1, Intl.DurationFormat.supportedLocalesOf.length);
            assert.areEqual(1, Intl.DurationFormat.prototype.format.length);
            assert.areEqual(1, Intl.DurationFormat.prototype.formatToParts.length);
            assert.areEqual(0, Intl.DurationFormat.prototype.resolvedOptions.length);

            assert.areEqual("[object Intl.DurationFormat]", Object.prototype.toString.call(Intl.DurationFormat.prototype));
            assert.areEqual("[object Intl.DurationFormat]", Object.prototype.toString.call(new Intl.DurationFormat()));
            assert.isTrue(Object.isExtensible(new Intl.DurationFormat()));
            assert.areEqual(Intl.DurationFormat, Intl.DurationFormat.prototype.constructor);

            assert.throws(() => Intl.DurationFormat(), TypeError);
            assert.throws(() => Intl.DurationFormat("en"), TypeError);
            assert.throws(() => new Intl.DurationFormat("en", null), TypeError);
            assert.throws(() => new Intl.DurationFormat("en", "short"), TypeError);
            assert.throws(() => new Intl.DurationFormat("en", 42), TypeError);
            assert.throws(() => new Intl.DurationFormat("en", true), TypeError);

            assert.areEqual("en", Intl.DurationFormat.supportedLocalesOf(["en", "zxx"])[0]);
            assert.areEqual(1, Intl.DurationFormat.supportedLocalesOf(["en", "zxx"]).length);
            assert.areEqual(0, Intl.DurationFormat.supportedLocalesOf.call(undefined).length);
            assert.throws(() => Intl.DurationFormat.supportedLocalesOf("en_US"), RangeError);
        }
    },
    {
        name: "Constructor option read order",
        body() {
            const actual = [];
            const expected = [
                "localeMatcher", "numberingSystem", "style",
                "years", "yearsDisplay",
                "months", "monthsDisplay",
                "weeks", "weeksDisplay",
                "days", "daysDisplay",
                "hours", "hoursDisplay",
                "minutes", "minutesDisplay",
                "seconds", "secondsDisplay",
                "milliseconds", "millisecondsDisplay",
                "microseconds", "microsecondsDisplay",
                "nanoseconds", "nanosecondsDisplay",
                "fractionalDigits"
            ];
            const proxy = new Proxy({}, {
                get(target, prop) {
                    actual.push(String(prop));
                    return undefined;
                }
            });
            new Intl.DurationFormat("en", proxy);
            assert.areEqual(expected.join(","), actual.join(","));
        }
    },
    {
        name: "resolvedOptions key order, defaults, unit cascading, and conflicts",
        body() {
            const def = new Intl.DurationFormat("en").resolvedOptions();
            assert.areEqual(
                "locale,numberingSystem,style," +
                "years,yearsDisplay,months,monthsDisplay,weeks,weeksDisplay,days,daysDisplay," +
                "hours,hoursDisplay,minutes,minutesDisplay,seconds,secondsDisplay," +
                "milliseconds,millisecondsDisplay,microseconds,microsecondsDisplay,nanoseconds,nanosecondsDisplay",
                Object.keys(def).join(",")
            );
            assert.areEqual("short", def.style);
            assert.isFalse(Object.prototype.hasOwnProperty.call(def, "fractionalDigits"));

            const dig = new Intl.DurationFormat("en", { style: "digital" }).resolvedOptions();
            assert.areEqual("short", dig.days);
            assert.areEqual("auto", dig.daysDisplay);
            assert.areEqual("numeric", dig.hours);
            assert.areEqual("always", dig.hoursDisplay);
            assert.areEqual("2-digit", dig.minutes);
            assert.areEqual("always", dig.minutesDisplay);
            assert.areEqual("2-digit", dig.seconds);
            assert.areEqual("always", dig.secondsDisplay);
            assert.areEqual("numeric", dig.milliseconds);
            assert.areEqual("auto", dig.millisecondsDisplay);
            assert.areEqual("numeric", dig.microseconds);
            assert.areEqual("auto", dig.microsecondsDisplay);
            assert.areEqual("numeric", dig.nanoseconds);
            assert.areEqual("auto", dig.nanosecondsDisplay);

            const expl = new Intl.DurationFormat("en", { years: "long", hours: "numeric", minutes: "numeric" }).resolvedOptions();
            assert.areEqual("long", expl.years);
            assert.areEqual("always", expl.yearsDisplay);
            assert.areEqual("numeric", expl.hours);
            assert.areEqual("always", expl.hoursDisplay);
            assert.areEqual("2-digit", expl.minutes);
            assert.areEqual("always", expl.minutesDisplay);
            assert.areEqual("2-digit", expl.seconds);
            assert.areEqual("always", expl.secondsDisplay);
            assert.areEqual("numeric", expl.milliseconds);
            assert.areEqual("auto", expl.millisecondsDisplay);

            assert.throws(() => new Intl.DurationFormat("en", { hours: "numeric", minutes: "short" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { minutes: "2-digit", seconds: "long" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { seconds: "numeric", milliseconds: "narrow" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { milliseconds: "numeric", microseconds: "short" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { milliseconds: "numeric", millisecondsDisplay: "always" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { style: "digital", millisecondsDisplay: "always" }), RangeError);
            assert.throws(() => new Intl.DurationFormat("en", { style: "digital", nanosecondsDisplay: "always" }), RangeError);
        }
    },
    {
        name: "fractionalDigits and numberingSystem validation",
        body() {
            const fd0 = new Intl.DurationFormat("en", { fractionalDigits: -0 }).resolvedOptions();
            assert.isTrue(Object.prototype.hasOwnProperty.call(fd0, "fractionalDigits"));
            assert.areEqual(0, fd0.fractionalDigits);
            assert.areEqual(8, new Intl.DurationFormat("en", { fractionalDigits: 8.9 }).resolvedOptions().fractionalDigits);
            assert.areEqual(9, new Intl.DurationFormat("en", { fractionalDigits: 9 }).resolvedOptions().fractionalDigits);
            assert.isFalse(Object.prototype.hasOwnProperty.call(
                new Intl.DurationFormat("en", { fractionalDigits: undefined }).resolvedOptions(),
                "fractionalDigits"
            ));

            [-0.5, -1, 9.5, 10, NaN, Infinity, -Infinity, "abc"].forEach(function (bad) {
                assert.throws(() => new Intl.DurationFormat("en", { fractionalDigits: bad }), RangeError);
            });
            assert.throws(() => new Intl.DurationFormat("en", { fractionalDigits: Symbol() }), TypeError);
            assert.throws(() => new Intl.DurationFormat("en", { fractionalDigits: 1n }), TypeError);

            ["", "a", "ab", "abcdefghi", "latn-", "lat\u00e9", "!bad!"].forEach(function (badNu) {
                assert.throws(() => new Intl.DurationFormat("en", { numberingSystem: badNu }), RangeError);
            });

            // NumberingSystem reconciliation between extension and options.
            let r = new Intl.DurationFormat("en-u-nu-arab", { numberingSystem: "invalid" }).resolvedOptions();
            assert.areEqual("en-u-nu-arab", r.locale);
            assert.areEqual("arab", r.numberingSystem);

            r = new Intl.DurationFormat("en-u-nu-latn", { numberingSystem: "arab" }).resolvedOptions();
            assert.areEqual("en", r.locale);
            assert.areEqual("arab", r.numberingSystem);

            r = new Intl.DurationFormat("en-u-nu-arab", { numberingSystem: "arab" }).resolvedOptions();
            assert.areEqual("en-u-nu-arab", r.locale);
            assert.areEqual("arab", r.numberingSystem);

            r = new Intl.DurationFormat("en-u-nu-invalid", { numberingSystem: "invalid2" }).resolvedOptions();
            assert.areEqual("en", r.locale);
            assert.areEqual("latn", r.numberingSystem);

            r = new Intl.DurationFormat("en", { numberingSystem: "ARAB" }).resolvedOptions();
            assert.areEqual("en", r.locale);
            assert.areEqual("latn", r.numberingSystem);
        }
    },
    {
        name: "ToDurationRecord alphabetical access, invalid/mixed/out-of-range inputs",
        body() {
            const df = new Intl.DurationFormat("en");
            const order = [];
            const proxy = new Proxy({ years: 1 }, {
                get(t, p) {
                    order.push(String(p));
                    return t[p];
                }
            });
            assert.areEqual("1 yr", df.format(proxy));
            assert.areEqual(
                "days,hours,microseconds,milliseconds,minutes,months,nanoseconds,seconds,weeks,years",
                order.join(",")
            );

            [undefined, null, true, false, 1, -12, 1n, -12n, Symbol(), {}, { year: 1 }, { years: undefined }, new String("PT1S")].forEach(function (bad) {
                assert.throws(() => df.format(bad), TypeError);
                assert.throws(() => df.formatToParts(bad), TypeError);
            });

            assert.throws(() => df.format({ hours: -1, minutes: 10 }), RangeError);
            assert.throws(() => df.format({ hours: 1, minutes: -10 }), RangeError);
            assert.throws(() => df.format({ years: 1.5 }), RangeError);
            assert.throws(() => df.format({ seconds: NaN }), RangeError);
            assert.throws(() => df.format({ days: Infinity }), RangeError);
            assert.throws(() => df.format({ days: -Infinity }), RangeError);
            assert.throws(() => df.format({ years: 1n }), TypeError);

            // -0 normalizes to +0 in ToDurationRecord so does not conflict with positive units.
            assert.areEqual("1 hr", df.format({ years: -0, hours: 1 }));
            assert.areEqual("1 yr", df.format({ years: 1, hours: -0 }));

            // Date field limit: < 2^32.
            assert.areEqual("string", typeof df.format({ years: 2 ** 32 - 1 }));
            assert.areEqual("string", typeof df.format({ years: -(2 ** 32 - 1) }));
            ["years", "months", "weeks"].forEach(function (u) {
                assert.throws(() => df.format({ [u]: 2 ** 32 }), RangeError);
                assert.throws(() => df.format({ [u]: -(2 ** 32) }), RangeError);
                assert.throws(() => df.format({ [u]: Number.MAX_VALUE }), RangeError);
            });

            // Time field limit: < 2^53 seconds (exact __int128 math, no UB on huge doubles).
            assert.throws(() => df.format({ days: Number.MAX_VALUE }), RangeError);
            assert.throws(() => df.format({ seconds: -Number.MAX_VALUE }), RangeError);
            assert.throws(() => df.format({ nanoseconds: 1.7976931348623157e+308 }), RangeError);
            assert.throws(() => df.format({ seconds: 9007199254740992 }), RangeError);
            assert.throws(() => df.format({ seconds: 9007199254740991, nanoseconds: 1_000_000_000 }), RangeError);
            assert.throws(() => df.format({ nanoseconds: 9007199254740992000000000 }), RangeError);
            assert.areEqual(
                "string",
                typeof df.format({ seconds: 9007199254740991, milliseconds: 999, microseconds: 999, nanoseconds: 999 })
            );
            assert.areEqual(
                "string",
                typeof df.format({ milliseconds: 4503599627370497_000, microseconds: 4503599627370495_000000 })
            );
        }
    },
    {
        name: "ISO 8601 duration strings",
        body() {
            const dfLong = new Intl.DurationFormat("en", { style: "long" });
            assert.areEqual("", dfLong.format("PT0S"));
            assert.areEqual("", dfLong.format("-PT0S"));
            assert.areEqual(
                dfLong.format({ years: 1, months: 2, weeks: 3, days: 4, hours: 5, minutes: 6, seconds: 7, milliseconds: 8, microseconds: 9, nanoseconds: 10 }),
                dfLong.format("P1Y2M3W4DT5H6M7.00800901S")
            );
            assert.areEqual(
                dfLong.format({ years: -1, months: -2, weeks: -3, days: -4, hours: -5, minutes: -6, seconds: -7, milliseconds: -8, microseconds: -9, nanoseconds: -10 }),
                dfLong.format("-p1y2m3w4dt5h6m7,00800901s")
            );
            assert.areEqual("1 day", dfLong.format("+P1D"));
            assert.areEqual("-1 day", dfLong.format("\u2212P1D"));
            assert.areEqual("1 hour, 30 minutes", dfLong.format("PT1.5H"));
            assert.areEqual("1 minute, 30 seconds", dfLong.format("PT1.5M"));
            assert.areEqual("59 minutes, 59 seconds, 999 milliseconds, 996 microseconds, 400 nanoseconds", dfLong.format("PT0.999999999H"));
            assert.areEqual("-24 hours, -34 minutes, -4 seconds, -404 milliseconds, -442 microseconds, -800 nanoseconds".replace(/ -/g, " "), dfLong.format("-PT24.567890123H"));

            const dfDig = new Intl.DurationFormat("en", { style: "digital", hoursDisplay: "always" });
            assert.areEqual("-0:00:01", dfDig.format("-PT1S"));
            assert.areEqual("0:00:00", dfDig.format("-PT0S"));

            [
                "", "P", "PT", "-P", "+PT", "P1DT", "P0.5D", "P1D2M", "P1Y2Y",
                "PT1.5H1M", "PT0.1H0M", "PT1.S", "PT.5S", "PT1.1234567891S",
                "P1D ", " P1D", "P" + "9".repeat(100) + "Y", "PT" + "9".repeat(100) + "S"
            ].forEach(function (bad) {
                assert.throws(() => dfLong.format(bad), RangeError, "bad string: " + bad);
                assert.throws(() => dfLong.formatToParts(bad), RangeError, "bad string parts: " + bad);
            });
        }
    },
    {
        name: "Exact nanosecond precision, trunc rounding, -0, and digital formatting",
        body() {
            const df = new Intl.DurationFormat("en", { style: "digital" });
            assert.areEqual("0:00:10000000.000000001", df.format({ seconds: 10_000_000, nanoseconds: 1 }));
            assert.areEqual("0:00:9007200.256743991", df.format({ seconds: 1, milliseconds: 2, microseconds: 3, nanoseconds: Number.MAX_SAFE_INTEGER }));
            assert.areEqual("0:00:9007199254740991.975424", df.format({ milliseconds: 4503599627370497_000, microseconds: 4503599627370495_000000 }));
            assert.areEqual("1234:1234567:12345678", df.format({ hours: 1234, minutes: 1234567, seconds: 12345678 }));

            // Rounding mode trunc (towards zero), not halfEven or floor.
            assert.areEqual("1", new Intl.DurationFormat("en", { seconds: "numeric", fractionalDigits: 0 }).format({ seconds: 1, milliseconds: 900 }));
            assert.areEqual("-1", new Intl.DurationFormat("en", { seconds: "numeric", fractionalDigits: 0 }).format({ seconds: -1, milliseconds: -900 }));
            assert.areEqual("-0", new Intl.DurationFormat("en", { seconds: "numeric", fractionalDigits: 0 }).format({ seconds: 0, nanoseconds: -1 }));
            assert.areEqual("0.001", new Intl.DurationFormat("en", { seconds: "numeric", fractionalDigits: 3 }).format({ milliseconds: 1, microseconds: 900 }));
            assert.areEqual("0.000001", new Intl.DurationFormat("en", { seconds: "numeric", fractionalDigits: 6 }).format({ microseconds: 1, nanoseconds: 900 }));

            // Pre-truncation non-zero check for minutes/seconds.
            const dfAuto = new Intl.DurationFormat("en", {
                hours: "numeric",
                minutesDisplay: "auto",
                secondsDisplay: "auto",
                fractionalDigits: 0
            });
            assert.areEqual("1", dfAuto.format({ hours: 1, minutes: 0, seconds: 0 }));
            assert.areEqual("1:00:00", dfAuto.format({ hours: 1, minutes: 0, seconds: 0, milliseconds: 1 }));
            assert.areEqual("1:00:00", dfAuto.format({ hours: 1, minutes: 0, seconds: 0, nanoseconds: 1 }));

            // Negative duration with leading zero.
            assert.areEqual("-0:00:01", new Intl.DurationFormat("en", { style: "digital", hoursDisplay: "always" }).format({ hours: 0, seconds: -1 }));
            assert.areEqual("-1 day, 01:02", new Intl.DurationFormat("en", { style: "digital", hoursDisplay: "auto" }).format({ days: -1, hours: 0, minutes: -1, seconds: -2 }));
            assert.areEqual("-0 hr, 1 sec", new Intl.DurationFormat("en", { hoursDisplay: "always" }).format({ hours: 0, seconds: -1 }));
            assert.areEqual("0 hr", new Intl.DurationFormat("en", { hoursDisplay: "always" }).format({ hours: -0 }));
        }
    },
    {
        name: "formatToParts structure across styles and units",
        body() {
            const df = new Intl.DurationFormat("en", { style: "long" });
            const p1 = df.formatToParts({ years: -1234, days: -5 });
            assert.areEqual(
                'minusSign:"-":year|integer:"1":year|group:",":year|integer:"234":year|literal:" ":year|unit:"years":year|' +
                'literal:", ":-|integer:"5":day|literal:" ":day|unit:"days":day',
                partsSignature(p1)
            );
            assert.isFalse(Object.prototype.hasOwnProperty.call(p1[6], "unit"));

            const dfDig = new Intl.DurationFormat("en", { style: "digital" });
            const p2 = dfDig.formatToParts({ days: 1, hours: 0, minutes: 2, seconds: 3, milliseconds: 450 });
            assert.areEqual(
                'integer:"1":day|literal:" ":day|unit:"day":day|literal:", ":-|' +
                'integer:"0":hour|literal:":":-|integer:"02":minute|literal:":":-|integer:"03":second|decimal:".":second|fraction:"45":second',
                partsSignature(p2)
            );
            assert.areEqual(0, df.formatToParts({ years: 0 }).length);
        }
    },
    {
        name: "Locales (en, es, de, ja, ar, fi) and numbering systems (arab, hanidec)",
        body() {
            assert.areEqual("1 a\u00f1o, 2 d\u00edas y 3 horas", new Intl.DurationFormat("es", { style: "long" }).format({ years: 1, days: 2, hours: 3 }));
            assert.areEqual("1000 d", new Intl.DurationFormat("es", { style: "short" }).format({ days: 1000 }));
            assert.areEqual("-1.000 J, 2 Mon.", new Intl.DurationFormat("de", { style: "short" }).format({ years: -1000, months: -2 }));
            assert.areEqual("1h2m3s", new Intl.DurationFormat("ja", { style: "narrow" }).format({ hours: 1, minutes: 2, seconds: 3 }));

            const ar = new Intl.DurationFormat("ar", { style: "long", numberingSystem: "arab" });
            assert.areEqual("\u0633\u0646\u062a\u0627\u0646 \u0648\u0663 \u0623\u064a\u0627\u0645", ar.format({ years: 2, days: 3 }));

            const enArab = new Intl.DurationFormat("en", { style: "digital", numberingSystem: "arab" });
            assert.areEqual("\u0661:\u0660\u0662:\u0660\u0663", enArab.format({ hours: 1, minutes: 2, seconds: 3 }));

            const enHanidec = new Intl.DurationFormat("en", { style: "digital", numberingSystem: "hanidec" });
            assert.areEqual("\u4e00:\u3007\u4e8c:\u3007\u4e09", enHanidec.format({ hours: 1, minutes: 2, seconds: 3 }));

            const fiDig = new Intl.DurationFormat("fi", { style: "digital" });
            assert.areEqual("1.02.03", fiDig.format({ hours: 1, minutes: 2, seconds: 3 }));
        }
    },
    {
        name: "Receiver branding, cross-realm prototypes, reentrancy, and GC finalization",
        body() {
            const df = new Intl.DurationFormat("en");
            const fmt = df.format;
            assert.throws(() => fmt({ hours: 1 }), TypeError);
            assert.throws(() => Intl.DurationFormat.prototype.format.call({}, { hours: 1 }), TypeError);
            assert.throws(() => Intl.DurationFormat.prototype.format.call(new Intl.ListFormat("en"), { hours: 1 }), TypeError);
            assert.throws(() => Intl.DurationFormat.prototype.formatToParts.call(Intl.DurationFormat.prototype, { hours: 1 }), TypeError);
            assert.throws(() => Intl.DurationFormat.prototype.resolvedOptions.call({}), TypeError);

            const other = WScript.LoadScript("", "samethread");
            assert.isTrue(other.Intl !== Intl);
            const newTarget = new other.Function();
            newTarget.prototype = undefined;
            const crossObj = Reflect.construct(Intl.DurationFormat, ["en"], newTarget);
            assert.isTrue(other.Intl.DurationFormat.prototype === Object.getPrototypeOf(crossObj));
            assert.areEqual("1 hr", Intl.DurationFormat.prototype.format.call(crossObj, { hours: 1 }));

            // Reentrant GC inside constructor option getters and ToDurationRecord getters.
            const reentrantCtor = new Intl.DurationFormat("en", {
                get style() { collect(); return "long"; },
                get fractionalDigits() { collect(); return 2; }
            });
            assert.areEqual("1 hour, 2 seconds", reentrantCtor.format({ hours: 1, seconds: 2 }));

            const inlineOut = new Intl.DurationFormat("en", { style: "digital" }).format({
                get hours() { collect(); return 1; },
                get seconds() { collect(); return { valueOf() { collect(); return 5; } }; }
            });
            assert.areEqual("1:00:05", inlineOut);

            // GC sweep & finalization stress.
            for (let round = 0; round < 5; round++) {
                for (let i = 0; i < 200; i++) {
                    const tmp = new Intl.DurationFormat(i % 2 ? "de" : "en", { style: i % 3 ? "digital" : "short" });
                    tmp.format({ hours: i, minutes: 2, seconds: 3 });
                    tmp.formatToParts({ days: 1, hours: 2 });
                }
                collect();
            }
            assert.areEqual("1 hr, 2 min", df.format({ hours: 1, minutes: 2 }));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
