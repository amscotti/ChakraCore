//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.Locale (ECMA-402 section 14 + Intl Locale Info API).
// Covers the constructor step order, option validation and canonicalization, every
// getter, maximize/minimize, the Locale-info methods, cross-realm construction,
// reentrancy and GC churn (run again with -CollectGarbage).

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function gc() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function throwsOf(ctor, fn, message) {
    assert.throws(fn, ctor, message);
}

let tests = [
    {
        name: "Constructor basics and canonicalization",
        body: function () {
            assert.areEqual("function", typeof Intl.Locale);
            assert.areEqual("Locale", Intl.Locale.name);
            assert.areEqual(1, Intl.Locale.length);
            assert.areEqual("en-US", new Intl.Locale("en-us").toString());
            assert.areEqual("it-Latn-IT", new Intl.Locale("IT-LATN-iT").toString());
            assert.areEqual("th-TH-u-nu-thai", new Intl.Locale("th-th-u-nu-thai").toString());
            assert.areEqual("de-1996", new Intl.Locale("DE-1996").toString());
            assert.areEqual("sl-1994-biske-rozaj", new Intl.Locale("sl-ROZAJ-BISKE-1994").toString());
            assert.areEqual("en-a-bar-u-baz-x-u-foo", new Intl.Locale("en-u-baz-a-bar-x-u-foo").toString());
            assert.areEqual("ro", new Intl.Locale("mo").toString());
            assert.areEqual("xtg", new Intl.Locale("cel-gaulish").toString());
            assert.areEqual("[object Intl.Locale]", Object.prototype.toString.call(new Intl.Locale("en")));
        }
    },
    {
        name: "Constructor argument validation",
        body: function () {
            assert.throws(function () { Intl.Locale("en"); }, TypeError);
            assert.throws(function () { new Intl.Locale(); }, TypeError);
            assert.throws(function () { new Intl.Locale(undefined); }, TypeError);
            assert.throws(function () { new Intl.Locale(null); }, TypeError);
            assert.throws(function () { new Intl.Locale(5); }, TypeError);
            assert.throws(function () { new Intl.Locale(true); }, TypeError);
            assert.throws(function () { new Intl.Locale(Symbol()); }, TypeError);
            assert.throws(function () { new Intl.Locale("en", null); }, TypeError);
            ["", "root", "abcd", "en-", "-en", "en--US", "X-u-foo", "da-u", "da-u-", "da-u--",
             "da-u-ca-gregory-u-ca-buddhist", "zh-min", "i-default", "no-bok", "en-US-US", "e", "en_US"].forEach(function (tag) {
                assert.throws(function () { new Intl.Locale(tag); }, RangeError, "tag " + JSON.stringify(tag));
            });
            assert.throws(function () { new Intl.Locale("en\u00e9"); }, RangeError);
        }
    },
    {
        name: "Tag coercion happens before option reads and errors propagate",
        body: function () {
            let order = [];
            let loc = new Intl.Locale({ toString() { order.push("tag"); return "en"; } }, {
                get language() { order.push("language"); },
                get script() { order.push("script"); },
                get region() { order.push("region"); },
                get variants() { order.push("variants"); },
                get calendar() { order.push("calendar"); },
                get collation() { order.push("collation"); },
                get firstDayOfWeek() { order.push("firstDayOfWeek"); },
                get hourCycle() { order.push("hourCycle"); },
                get caseFirst() { order.push("caseFirst"); },
                get numeric() { order.push("numeric"); },
                get numberingSystem() { order.push("numberingSystem"); }
            });
            assert.areEqual("en", loc.toString());
            assert.areEqual(
                ["tag", "language", "script", "region", "variants", "calendar", "collation", "firstDayOfWeek",
                 "hourCycle", "caseFirst", "numeric", "numberingSystem"].join(),
                order.join());

            function Boom() {}
            assert.throws(function () { new Intl.Locale({ toString() { throw new Boom(); } }); }, Boom);
            ["language", "script", "region", "variants", "calendar", "collation", "firstDayOfWeek",
             "hourCycle", "caseFirst", "numeric", "numberingSystem"].forEach(function (name) {
                assert.throws(function () {
                    new Intl.Locale("en", { get [name]() { throw new Boom(); } });
                }, Boom, name);
            });
        }
    },
    {
        name: "Language, script, region and variants options",
        body: function () {
            assert.areEqual("de", new Intl.Locale("en", { language: "de" }).toString());
            assert.areEqual("de-US", new Intl.Locale("en-US", { language: "de" }).toString());
            assert.areEqual("en-Cyrl", new Intl.Locale("en", { script: "cyrl" }).toString());
            assert.areEqual("en-DK", new Intl.Locale("en", { region: "dk" }).toString());
            assert.areEqual("en-150", new Intl.Locale("en", { region: "150" }).toString());
            assert.areEqual("en-spanglis", new Intl.Locale("en", { variants: "spanglis" }).toString());
            assert.areEqual("en-oxendict-spanglis", new Intl.Locale("en", { variants: "spanglis-oxendict" }).toString());
            assert.areEqual("en-fonipa-u-ca-gregory", new Intl.Locale("en-u-ca-gregory", { variants: "fonipa" }).toString());
            assert.areEqual("ru-Armn-AM", new Intl.Locale("und-Armn-SU", { language: "ru" }).toString());
            assert.areEqual("xtg", new Intl.Locale("cel", { variants: "gaulish" }).toString());
            assert.areEqual("en", new Intl.Locale("en", { script: undefined, region: undefined }).toString());

            ["", "root", "abcd", "zh-cmn", "en-US"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { language: v }); }, RangeError, "language " + v);
            });
            assert.throws(function () { new Intl.Locale("en", { language: null }); }, RangeError);
            ["", "abc", "abcde", "Latn-US"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { script: v }); }, RangeError, "script " + v);
            });
            ["", "a", "abc", "a7", "notaregion", "SA-vaidika"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { region: v }); }, RangeError, "region " + v);
            });
            ["", "-", "-spanglis", "spanglis-", "spanglis--oxendict", "abc", "fonipa-fonipa", "fonipa-valencia-Fonipa", "GB-scouse"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { variants: v }); }, RangeError, "variants " + v);
            });
        }
    },
    {
        name: "Unicode extension options",
        body: function () {
            assert.areEqual("en-u-ca-gregory", new Intl.Locale("en", { calendar: "gregory" }).toString());
            assert.areEqual("en-u-ca-islamic-civil", new Intl.Locale("en", { calendar: "islamicc" }).toString());
            assert.areEqual("en-u-ca-gregory", new Intl.Locale("en", { calendar: "GREGORY" }).toString());
            assert.areEqual("en-u-ca-abc", new Intl.Locale("en-u-ca-gregory", { calendar: "abc" }).toString());
            assert.areEqual("en-u-ca-1234abcd-abc123", new Intl.Locale("en", { calendar: "1234ABCD-abc123" }).toString());
            assert.areEqual("zh-u-co-zhuyin", new Intl.Locale("zh-u-co-unihan", { collation: "zhuyin" }).toString());
            assert.areEqual("en-u-hc-h23", new Intl.Locale("en", { hourCycle: "h23" }).toString());
            assert.areEqual("en-u-kf-upper", new Intl.Locale("en", { caseFirst: "upper" }).toString());
            assert.areEqual("en-u-kf", new Intl.Locale("en-u-kf-true").toString());
            assert.areEqual("en-u-kn", new Intl.Locale("en", { numeric: true }).toString());
            assert.areEqual("en-u-kn-false", new Intl.Locale("en", { numeric: false }).toString());
            assert.areEqual("en-u-kn", new Intl.Locale("en-u-kn-true", { numeric: undefined }).toString());
            assert.areEqual("en-u-nu-latn", new Intl.Locale("en", { numberingSystem: "LATN" }).toString());
            assert.areEqual("pt-u-attr-ca-gregory-nu-latn", new Intl.Locale("pt-u-attr-ca-gregory", { numberingSystem: "latn" }).toString());
            assert.areEqual("en-u-ca-gregory-x-priv", new Intl.Locale("en-x-priv", { calendar: "gregory" }).toString());
            assert.areEqual("en-t-fr-u-ca-gregory", new Intl.Locale("en-t-fr", { calendar: "gregory" }).toString());

            ["", "a", "ab", "abcdefghi", "abc-abcdefghi"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { calendar: v }); }, RangeError, "calendar " + v);
                assert.throws(function () { new Intl.Locale("en", { collation: v }); }, RangeError, "collation " + v);
                assert.throws(function () { new Intl.Locale("en", { numberingSystem: v }); }, RangeError, "numberingSystem " + v);
            });
            ["latn-ca", "latn-ca-gregory", "!invalid!"].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { numberingSystem: v }); }, RangeError, v);
            });
            assert.throws(function () { new Intl.Locale("en", { hourCycle: "h25" }); }, RangeError);
            assert.throws(function () { new Intl.Locale("en", { caseFirst: "x" }); }, RangeError);
        }
    },
    {
        name: "firstDayOfWeek option and getter",
        body: function () {
            let map = [["mon", "mon"], ["sun", "sun"], ["1", "mon"], ["7", "sun"], ["0", "sun"], [0, "sun"], [-0, "sun"],
                       [3, "wed"], [100, "100"], ["Montag", "montag"], [true, ""], [false, "false"], [1n, "mon"]];
            map.forEach(function (pair) {
                let loc = new Intl.Locale("en", { firstDayOfWeek: pair[0] });
                assert.areEqual(pair[1], loc.firstDayOfWeek, String(pair[0]));
            });
            ["", "m", "mo", "abc?", 8, -1, 0.5, Infinity * -1].forEach(function (v) {
                assert.throws(function () { new Intl.Locale("en", { firstDayOfWeek: v }); }, RangeError, String(v));
            });
            assert.areEqual(undefined, new Intl.Locale("en").firstDayOfWeek);
            assert.areEqual("", new Intl.Locale("en-u-fw").firstDayOfWeek);
            assert.areEqual(1, new Intl.Locale("en-u-fw-mon").getWeekInfo().firstDay);
            assert.areEqual(7, new Intl.Locale("en", { firstDayOfWeek: 0 }).getWeekInfo().firstDay);
        }
    },
    {
        name: "Getters",
        body: function () {
            let loc = new Intl.Locale("de-latn-de-fonipa-1996-u-ca-gregory-co-phonebk-hc-h23-kf-true-kn-false-nu-latn");
            assert.areEqual("de-Latn-DE-1996-fonipa-u-ca-gregory-co-phonebk-hc-h23-kf-kn-false-nu-latn", loc.toString());
            assert.areEqual("de-Latn-DE-1996-fonipa", loc.baseName);
            assert.areEqual("de", loc.language);
            assert.areEqual("Latn", loc.script);
            assert.areEqual("DE", loc.region);
            assert.areEqual("1996-fonipa", loc.variants);
            assert.areEqual("gregory", loc.calendar);
            assert.areEqual("phonebk", loc.collation);
            assert.areEqual("h23", loc.hourCycle);
            assert.areEqual("", loc.caseFirst);
            assert.areEqual(false, loc.numeric);
            assert.areEqual("latn", loc.numberingSystem);

            let und = new Intl.Locale("und");
            assert.areEqual("und", und.language);
            assert.areEqual(undefined, und.script);
            assert.areEqual(undefined, und.region);
            assert.areEqual(undefined, und.variants);
            assert.areEqual(undefined, und.calendar);
            assert.areEqual(undefined, und.caseFirst);
            assert.areEqual(false, und.numeric);
            assert.areEqual(true, new Intl.Locale("en-u-kn").numeric);
            assert.areEqual(true, new Intl.Locale("en", { numeric: "false" }).numeric);
            assert.areEqual("en-US", new Intl.Locale("en-US-u-co-emoji").baseName);
        }
    },
    {
        name: "Getter and method identity",
        body: function () {
            let proto = Intl.Locale.prototype;
            ["baseName", "calendar", "caseFirst", "collation", "firstDayOfWeek", "hourCycle", "language",
             "numberingSystem", "numeric", "region", "script", "variants"].forEach(function (name) {
                let d = Object.getOwnPropertyDescriptor(proto, name);
                assert.areEqual("function", typeof d.get, name);
                assert.areEqual(undefined, d.set, name);
                assert.areEqual("get " + name, d.get.name, name);
                assert.areEqual(0, d.get.length, name);
                assert.areEqual(false, d.enumerable, name);
                assert.areEqual(true, d.configurable, name);
                assert.throws(function () { d.get.call({}); }, TypeError, name);
                assert.throws(function () { d.get.call(undefined); }, TypeError, name);
                assert.throws(function () { d.get.call(proto); }, TypeError, name);
                assert.throws(function () { d.get.call("en"); }, TypeError, name);
            });
            ["toString", "maximize", "minimize", "getCalendars", "getCollations", "getHourCycles",
             "getNumberingSystems", "getTimeZones", "getTextInfo", "getWeekInfo"].forEach(function (name) {
                let d = Object.getOwnPropertyDescriptor(proto, name);
                assert.areEqual("function", typeof d.value, name);
                assert.areEqual(name, d.value.name, name);
                assert.areEqual(0, d.value.length, name);
                assert.areEqual(true, d.writable, name);
                assert.areEqual(false, d.enumerable, name);
                assert.areEqual(true, d.configurable, name);
                assert.throws(function () { d.value.call({}); }, TypeError, name);
                assert.throws(function () { d.value.call(proto); }, TypeError, name);
                assert.throws(function () { d.value.call(undefined); }, TypeError, name);
            });
            let tag = Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag);
            assert.areEqual("Intl.Locale", tag.value);
            assert.areEqual(false, tag.writable);
            assert.areEqual(true, tag.configurable);
            let pd = Object.getOwnPropertyDescriptor(Intl.Locale, "prototype");
            assert.areEqual(false, pd.writable);
            assert.areEqual(false, pd.enumerable);
            assert.areEqual(false, pd.configurable);
            assert.areEqual(Intl.Locale, proto.constructor);
        }
    },
    {
        name: "Locale objects as input",
        body: function () {
            let enUS = new Intl.Locale("en-US");
            let enGB = new Intl.Locale(enUS, { region: "GB" });
            assert.areEqual("en-US", enUS.toString());
            assert.areEqual("en-GB", enGB.toString());
            let unihan = new Intl.Locale("zh-u-co-unihan");
            let zhuyin = new Intl.Locale(unihan, { collation: "zhuyin" });
            assert.areEqual("zh-u-co-unihan", unihan.toString());
            assert.areEqual("zh-u-co-zhuyin", zhuyin.toString());
            assert.areEqual("en-US", new Intl.Locale(enUS).toString());
            assert.areEqual(["en-US"].join(), Intl.getCanonicalLocales(enUS).join());
            assert.areEqual(["en-US", "zh-u-co-zhuyin"].join(), Intl.getCanonicalLocales([enUS, zhuyin]).join());
        }
    },
    {
        name: "maximize and minimize",
        body: function () {
            let data = [
                ["en", "en-Latn-US", "en"],
                ["und", "en-Latn-US", "en"],
                ["zh-TW", "zh-Hant-TW", "zh-TW"],
                ["es-ES-preeuro", "es-Latn-ES-preeuro", "es-preeuro"],
                ["de-u-kf", "de-Latn-DE-u-kf", "de-u-kf"],
                ["ro-x-private", "ro-Latn-RO-x-private", "ro-x-private"]
            ];
            data.forEach(function (row) {
                let loc = new Intl.Locale(row[0]);
                assert.areEqual(row[1], loc.maximize().toString(), row[0]);
                assert.areEqual(row[2], loc.minimize().toString(), row[0]);
                assert.areEqual(row[2], loc.maximize().minimize().toString(), row[0]);
                assert.areEqual(row[1], loc.minimize().maximize().toString(), row[0]);
                assert.areEqual(Intl.Locale.prototype, Object.getPrototypeOf(loc.maximize()));
                assert.areEqual(false, loc.maximize() === loc);
            });
            assert.areEqual("abcdefgh", new Intl.Locale("abcdefgh").maximize().toString());
            assert.areEqual("en-US", new Intl.Locale("en-US").toString(), "receiver unchanged");
        }
    },
    {
        name: "Locale info: calendars, collations, hour cycles, numbering systems",
        body: function () {
            let cals = new Intl.Locale("en").getCalendars();
            assert.isTrue(Array.isArray(cals) && cals.length > 0);
            assert.areEqual("gregory", cals[0]);
            assert.areEqual(["buddhist"].join(), new Intl.Locale("th-u-ca-buddhist").getCalendars().join());
            assert.areEqual([""].join(), new Intl.Locale("en-u-ca").getCalendars().join());
            assert.isFalse(new Intl.Locale("fa-IR").getCalendars().join() === new Intl.Locale("fa-IN").getCalendars().join());
            assert.areEqual(new Intl.Locale("fa-TH").getCalendars().join(), new Intl.Locale("fa-JP-u-rg-thzzzz").getCalendars().join());
            assert.areEqual(new Intl.Locale("fa-IR").getCalendars().join(), new Intl.Locale("fa").getCalendars().join());

            ["en", "de", "zh", "sv", "ja", "und"].forEach(function (tag) {
                let c = new Intl.Locale(tag).getCollations();
                assert.isTrue(Array.isArray(c) && c.length > 0, tag);
                assert.areEqual(c.slice().sort().join(), c.join(), tag + " sorted");
                assert.isTrue(c.indexOf("standard") < 0 && c.indexOf("search") < 0, tag);
            });
            assert.areEqual(["emoji", "eor"].join(), new Intl.Locale("und").getCollations().join());
            assert.areEqual(["emoji", "eor"].join(), new Intl.Locale("abcdefgh").getCollations().join());
            assert.areEqual(["phonebk"].join(), new Intl.Locale("de", { collation: "phonebk" }).getCollations().join());
            assert.areEqual([""].join(), new Intl.Locale("en-u-co").getCollations().join());

            assert.areEqual(["h23"].join(), new Intl.Locale("en-GB").getHourCycles().join());
            assert.areEqual(["h12"].join(), new Intl.Locale("en-US").getHourCycles().join());
            assert.areEqual(["h24"].join(), new Intl.Locale("en", { hourCycle: "h24" }).getHourCycles().join());
            assert.areEqual(new Intl.Locale("en-GB").getHourCycles().join(), new Intl.Locale("en-US-u-rg-gbzzzz").getHourCycles().join());

            assert.areEqual(["latn"].join(), new Intl.Locale("en").getNumberingSystems().join());
            assert.areEqual(["arab"].join(), new Intl.Locale("ar-EG").getNumberingSystems().join());
            assert.areEqual(["latn"].join(), new Intl.Locale("abcdefgh").getNumberingSystems().join());
            assert.areEqual(["deva"].join(), new Intl.Locale("en", { numberingSystem: "deva" }).getNumberingSystems().join());
            assert.areEqual([""].join(), new Intl.Locale("en-u-nu").getNumberingSystems().join());
        }
    },
    {
        name: "Locale info: time zones, text info, week info",
        body: function () {
            assert.areEqual(undefined, new Intl.Locale("en").getTimeZones());
            let zones = new Intl.Locale("en-US").getTimeZones();
            assert.isTrue(Array.isArray(zones) && zones.length > 0);
            assert.areEqual(zones.slice().sort().join(), zones.join());
            assert.isTrue(zones.indexOf("America/New_York") >= 0);
            assert.isTrue(new Intl.Locale("fr-FR").getTimeZones().indexOf("Europe/Paris") >= 0);

            assert.areEqual("ltr", new Intl.Locale("en").getTextInfo().direction);
            assert.areEqual("rtl", new Intl.Locale("ar").getTextInfo().direction);
            assert.areEqual("rtl", new Intl.Locale("pa-PK").getTextInfo().direction);
            assert.areEqual("rtl", new Intl.Locale("und-Arab").getTextInfo().direction);
            assert.areEqual(undefined, new Intl.Locale("en-Zyyy").getTextInfo().direction);
            assert.areEqual(undefined, new Intl.Locale("en-Brai").getTextInfo().direction);
            assert.areEqual(undefined, new Intl.Locale("ar-Zxxx").getTextInfo().direction);
            assert.areEqual(undefined, new Intl.Locale("en-Aaaa").getTextInfo().direction);
            assert.areEqual(undefined, new Intl.Locale("abcdefgh").getTextInfo().direction);
            assert.areEqual("ltr", new Intl.Locale("abcdefgh-Latn").getTextInfo().direction);
            assert.areEqual(["direction"].join(), Reflect.ownKeys(new Intl.Locale("en").getTextInfo()).join());

            let week = new Intl.Locale("en-US").getWeekInfo();
            assert.areEqual(["firstDay", "weekend"].join(), Reflect.ownKeys(week).join());
            assert.areEqual(7, week.firstDay);
            assert.areEqual([6, 7].join(), week.weekend.join());
            assert.areEqual(Object.prototype, Object.getPrototypeOf(week));
            assert.areEqual(1, new Intl.Locale("en-GB").getWeekInfo().firstDay);
            assert.areEqual([5].join(), new Intl.Locale("fa-IR").getWeekInfo().weekend.join());
            assert.areEqual([4, 5].join(), new Intl.Locale("fa-AF").getWeekInfo().weekend.join());
            assert.areEqual(new Intl.Locale("fa-AF").getWeekInfo().weekend.join(), new Intl.Locale("fa-JP-u-rg-afzzzz").getWeekInfo().weekend.join());
            assert.areEqual(new Intl.Locale("fa-IN").getWeekInfo().firstDay, new Intl.Locale("fa-u-sd-inka").getWeekInfo().firstDay);
        }
    },
    {
        name: "Subclassing, newTarget and cross-realm construction",
        body: function () {
            class MyLocale extends Intl.Locale {
                constructor(tag, opts) { super(tag, opts); this.extra = 1; }
                get custom() { return this.language; }
            }
            let m = new MyLocale("fr-CA", { calendar: "gregory" });
            assert.isTrue(m instanceof Intl.Locale);
            assert.areEqual("fr-CA-u-ca-gregory", m.toString());
            assert.areEqual("fr", m.custom);
            assert.areEqual(MyLocale.prototype, Object.getPrototypeOf(m));
            assert.areEqual("fr-Latn-CA-u-ca-gregory", m.maximize().toString());

            function F() {}
            F.prototype = Array.prototype;
            let viaReflect = Reflect.construct(Intl.Locale, ["de"], F);
            assert.areEqual(Array.prototype, Object.getPrototypeOf(viaReflect));
            assert.areEqual("de", Intl.Locale.prototype.toString.call(viaReflect));

            let other = WScript.LoadScript("this", "samethread");
            let nt = new other.Function();
            [undefined, null, true, "str", Symbol(), 0].forEach(function (p) {
                nt.prototype = p;
                let loc = Reflect.construct(Intl.Locale, ["de"], nt);
                assert.areEqual(other.Intl.Locale.prototype, Object.getPrototypeOf(loc), String(typeof p));
                assert.areEqual("de", loc.toString());
            });
            let foreign = new other.Intl.Locale("ja-JP");
            assert.areEqual("ja-JP", new Intl.Locale(foreign).toString());
            assert.areEqual(Intl.Locale.prototype, Object.getPrototypeOf(new Intl.Locale(foreign)));
            assert.areEqual("ja-JP-u-ca-japanese", new Intl.Locale(foreign, { calendar: "japanese" }).toString());
            assert.areEqual("ja", Intl.Locale.prototype.__lookupGetter__("language").call(foreign));
        }
    },
    {
        name: "Reentrancy: option getters and toString run user code",
        body: function () {
            let created = [];
            let loc = new Intl.Locale({
                toString() {
                    gc();
                    created.push(new Intl.Locale("de"));
                    return "en-US";
                }
            }, {
                get language() { gc(); created.push(new Intl.Locale("fr", { region: "CA" })); return "es"; },
                get calendar() { gc(); return { toString() { gc(); return "gregory"; } }; },
                get numeric() { gc(); return 1; }
            });
            assert.areEqual("es-US-u-ca-gregory-kn", loc.toString());
            assert.areEqual("fr-CA", created[1].toString());
            assert.areEqual("de", created[0].toString());

            let opts = { calendar: "gregory" };
            let l2 = new Intl.Locale("en", opts);
            opts.calendar = "buddhist";
            assert.areEqual("gregory", l2.calendar, "state is a snapshot of the options");

            // The Locale argument can be mutated by getters; its [[Locale]] is read before options.
            let base = new Intl.Locale("en-US");
            let l3 = new Intl.Locale(base, { get region() { return "GB"; } });
            assert.areEqual("en-GB", l3.toString());
            assert.areEqual("en-US", base.toString());
        }
    },
    {
        name: "GC churn",
        body: function () {
            let keep = [];
            for (let i = 0; i < 3000; i++) {
                let loc = new Intl.Locale("en-US", { calendar: "gregory", numeric: (i & 1) === 0, hourCycle: "h23" });
                let max = loc.maximize();
                let min = max.minimize();
                if (i % 100 === 0) {
                    keep.push(max);
                    gc();
                }
                assert.areEqual("en", min.baseName, "iteration " + i);
                if (i % 7 === 0) {
                    assert.areEqual("gregory", loc.calendar);
                    assert.isTrue(loc.getCalendars().length === 1);
                    assert.areEqual("object", typeof loc.getWeekInfo());
                }
            }
            gc();
            gc();
            keep.forEach(function (loc) {
                assert.areEqual("en-Latn-US-u-ca-gregory-hc-h23", loc.toString().replace(/-kn(-false)?/, ""));
                assert.areEqual("en", loc.language);
                assert.areEqual("Latn", loc.script);
                assert.areEqual("US", loc.region);
            });
            let anon = [];
            for (let i = 0; i < 500; i++) {
                anon.push(new Intl.Locale("ja-JP", { collation: "unihan" }).toString());
            }
            gc();
            assert.areEqual("ja-JP-u-co-unihan", anon[499]);
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
