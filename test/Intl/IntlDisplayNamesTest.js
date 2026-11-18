//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.DisplayNames (ICU backend): all six types, fallback, languageDisplay,
// invalid codes, reentrant coercions, cross-realm construction and GC finalization.
// Run also with: ch -CollectGarbage IntlDisplayNamesTest.js

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function collect() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

function make(type, extra) {
    var options = { type: type };
    if (extra) {
        for (var k in extra) {
            options[k] = extra[k];
        }
    }
    return new Intl.DisplayNames("en", options);
}

var tests = [
    {
        name: "Constructor, prototype and method identities and descriptors",
        body: function () {
            assert.areEqual("DisplayNames", Intl.DisplayNames.name);
            assert.areEqual(2, Intl.DisplayNames.length);
            var desc = Object.getOwnPropertyDescriptor(Intl.DisplayNames, "name");
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);
            desc = Object.getOwnPropertyDescriptor(Intl.DisplayNames, "length");
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);
            desc = Object.getOwnPropertyDescriptor(Intl, "DisplayNames");
            assert.isTrue(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);

            desc = Object.getOwnPropertyDescriptor(Intl.DisplayNames, "prototype");
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isFalse(desc.configurable);
            desc = Object.getOwnPropertyDescriptor(Intl.DisplayNames.prototype, Symbol.toStringTag);
            assert.areEqual("Intl.DisplayNames", desc.value);
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);

            assert.areEqual(1, Intl.DisplayNames.prototype.of.length);
            assert.areEqual("of", Intl.DisplayNames.prototype.of.name);
            assert.areEqual(0, Intl.DisplayNames.prototype.resolvedOptions.length);
            assert.areEqual("resolvedOptions", Intl.DisplayNames.prototype.resolvedOptions.name);
            assert.areEqual(1, Intl.DisplayNames.supportedLocalesOf.length);
            assert.areEqual("supportedLocalesOf", Intl.DisplayNames.supportedLocalesOf.name);
            ["of", "resolvedOptions"].forEach(function (name) {
                var d = Object.getOwnPropertyDescriptor(Intl.DisplayNames.prototype, name);
                assert.isTrue(d.writable, name);
                assert.isFalse(d.enumerable, name);
                assert.isTrue(d.configurable, name);
            });
            assert.areEqual(Intl.DisplayNames, Intl.DisplayNames.prototype.constructor);
            assert.areEqual(Function.prototype, Object.getPrototypeOf(Intl.DisplayNames));
            assert.areEqual("[object Intl.DisplayNames]", Object.prototype.toString.call(make("region")));
        }
    },
    {
        name: "Constructor requires new and a valid options object with a type",
        body: function () {
            assert.throws(function () { Intl.DisplayNames(); }, TypeError);
            assert.throws(function () { Intl.DisplayNames("en", { type: "region" }); }, TypeError);
            assert.throws(function () { new Intl.DisplayNames("en"); }, TypeError, "options undefined");
            assert.throws(function () { new Intl.DisplayNames("en", undefined); }, TypeError);
            assert.throws(function () { new Intl.DisplayNames("en", {}); }, TypeError, "no type");
            assert.throws(function () { new Intl.DisplayNames("en", { type: undefined }); }, TypeError);
            [null, true, false, "test", 7, Symbol()].forEach(function (bad) {
                assert.throws(function () { new Intl.DisplayNames("en", bad); }, TypeError, String(typeof bad));
            });
            assert.throws(function () { new Intl.DisplayNames(null, { type: "region" }); }, TypeError, "null locales");

            ["lang", "dayPeriod", "weekday", "", null, "Language", ["language", "region"]].forEach(function (bad) {
                assert.throws(function () { new Intl.DisplayNames("en", { type: bad }); }, RangeError, String(bad));
            });
            assert.throws(function () { new Intl.DisplayNames("en", { type: Symbol() }); }, TypeError, "symbol type");
            assert.throws(function () { new Intl.DisplayNames("en", { type: "region", style: "x" }); }, RangeError);
            assert.throws(function () { new Intl.DisplayNames("en", { type: "region", fallback: "x" }); }, RangeError);
            assert.throws(function () { new Intl.DisplayNames("en", { type: "region", fallback: null }); }, RangeError);
            assert.throws(function () { new Intl.DisplayNames("en", { type: "region", localeMatcher: "x" }); }, RangeError);
            assert.throws(function () { new Intl.DisplayNames("en", { type: "language", languageDisplay: "x" }); }, RangeError);
            assert.throws(function () { new Intl.DisplayNames("en", { type: "region", languageDisplay: "x" }); }, RangeError, "languageDisplay is always read");
            assert.throws(function () { new Intl.DisplayNames("e", { type: "region" }); }, RangeError, "invalid locale");
        }
    },
    {
        name: "Constructor step order: prototype, locales, options type check, then options in spec order",
        body: function () {
            var log = [];
            var target = new Proxy(function () {}, {
                get: function (t, key) {
                    if (key === "prototype") {
                        log.push("prototype");
                    }
                    return t[key];
                }
            });
            var locales = {
                get length() { log.push("locales.length"); return 1; },
                get 0() { log.push("locales.0"); return "en"; }
            };
            assert.throws(function () { Reflect.construct(Intl.DisplayNames, [locales], target); }, TypeError);
            assert.areEqual(["prototype", "locales.length", "locales.0"], log, "options undefined fails after locales");

            log = [];
            var options = {};
            ["localeMatcher", "style", "type", "fallback", "languageDisplay"].forEach(function (name) {
                var value = { localeMatcher: "lookup", style: "short", type: "language", fallback: "code", languageDisplay: "standard" }[name];
                Object.defineProperty(options, name, { get: function () { log.push(name); return value; } });
            });
            var dn = new Intl.DisplayNames("en", options);
            assert.areEqual(["localeMatcher", "style", "type", "fallback", "languageDisplay"], log);
            var resolved = dn.resolvedOptions();
            assert.areEqual("short", resolved.style);
            assert.areEqual("language", resolved.type);
            assert.areEqual("code", resolved.fallback);
            assert.areEqual("standard", resolved.languageDisplay);

            // A throwing style getter fires before the missing type is reported.
            assert.throws(function () {
                new Intl.DisplayNames("en", { get style() { throw new URIError("style"); } });
            }, URIError);
            // Abrupt getters and toString for every option.
            ["localeMatcher", "style", "type", "fallback", "languageDisplay"].forEach(function (name) {
                var o = { type: "language" };
                Object.defineProperty(o, name, { get: function () { throw new EvalError(name); } });
                assert.throws(function () { new Intl.DisplayNames("en", o); }, EvalError, name + " getter");
                var p = { type: "language" };
                p[name] = { toString: function () { throw new EvalError(name); } };
                assert.throws(function () { new Intl.DisplayNames("en", p); }, EvalError, name + " toString");
            });
        }
    },
    {
        name: "Prototype from newTarget (custom, default, cross-realm, subclass)",
        body: function () {
            var custom = new Function();
            custom.prototype = {};
            var obj = Reflect.construct(Intl.DisplayNames, ["en", { type: "region" }], custom);
            assert.areEqual(custom.prototype, Object.getPrototypeOf(obj));
            assert.areEqual("United States", Intl.DisplayNames.prototype.of.call(obj, "US"));

            assert.areEqual(Intl.DisplayNames.prototype, Object.getPrototypeOf(make("region")));
            assert.isTrue(Object.isExtensible(make("region")));

            var poisoned = new Proxy(new Function(), {
                get: function (t, key) {
                    if (key === "prototype") {
                        throw new RangeError("poison");
                    }
                    return t[key];
                }
            });
            assert.throws(function () { Reflect.construct(Intl.DisplayNames, [], poisoned); }, RangeError);

            class MyDisplayNames extends Intl.DisplayNames {
                constructor(locale) { super(locale, { type: "currency" }); }
            }
            var sub = new MyDisplayNames("en");
            assert.isTrue(sub instanceof MyDisplayNames);
            assert.isTrue(sub instanceof Intl.DisplayNames);
            assert.areEqual("US Dollar", sub.of("USD"));
            assert.areEqual("currency", sub.resolvedOptions().type);
        }
    },
    {
        name: "Cross-realm newTarget with a null prototype resolves the other realm's default",
        body: function () {
            if (typeof WScript.LoadScript !== "function") {
                return;
            }
            var other = WScript.LoadScript("this", "samethread");
            var C = new other.Function();
            C.prototype = null;
            var o = Reflect.construct(Intl.DisplayNames, ["en", { type: "language" }], C);
            assert.areEqual(other.Intl.DisplayNames.prototype, Object.getPrototypeOf(o));
            assert.areEqual("English", Intl.DisplayNames.prototype.of.call(o, "en"));

            // A DisplayNames created in the other realm works through this realm's methods and back.
            var fromOther = new other.Intl.DisplayNames("en", { type: "region" });
            assert.areEqual("United States", Intl.DisplayNames.prototype.of.call(fromOther, "US"));
            assert.areEqual("region", other.Intl.DisplayNames.prototype.resolvedOptions.call(make("region")).type);
            collect();
            assert.areEqual("United States", fromOther.of("US"));
        }
    },
    {
        name: "All six types return localized names (English)",
        body: function () {
            assert.areEqual("English", make("language").of("en"));
            assert.areEqual("American English", make("language").of("en-US"), "dialect default");
            assert.areEqual("American English", make("language", { languageDisplay: "dialect" }).of("en-US"));
            assert.areEqual("English (United States)", make("language", { languageDisplay: "standard" }).of("en-US"));
            assert.areEqual("Traditional Chinese", make("language").of("zh-Hant"));
            assert.areEqual("American English", make("language").of("EN-us"), "code is canonicalized before lookup");

            assert.areEqual("United States", make("region").of("US"));
            assert.areEqual("United States", make("region").of("us"));
            assert.areEqual("Latin America", make("region").of("419"));
            assert.areEqual("Latin", make("script").of("Latn"));
            assert.areEqual("Latin", make("script").of("lATN"));
            assert.areEqual("US Dollar", make("currency").of("USD"));
            assert.areEqual("US Dollar", make("currency").of("usd"));
            assert.areEqual("Japanese Yen", make("currency").of("JPY"));
            assert.areEqual("Gregorian Calendar", make("calendar").of("gregory"));
            assert.areEqual("Gregorian Calendar", make("calendar").of("GREGORY"));
            assert.areEqual("Buddhist Calendar", make("calendar").of("buddhist"));

            assert.areEqual("year", make("dateTimeField").of("year"));
            assert.areEqual("month", make("dateTimeField").of("month"));
            assert.areEqual("day of the week", make("dateTimeField").of("weekday"));
            assert.areEqual("hour", make("dateTimeField").of("hour"));
            assert.areEqual("time zone", make("dateTimeField").of("timeZoneName"));
            ["era", "year", "quarter", "month", "weekOfYear", "weekday", "day", "dayPeriod",
             "hour", "minute", "second", "timeZoneName"].forEach(function (field) {
                ["narrow", "short", "long"].forEach(function (style) {
                    var name = make("dateTimeField", { style: style }).of(field);
                    assert.areEqual("string", typeof name, field + "/" + style);
                    assert.isTrue(name.length > 0, field + "/" + style);
                });
            });
            assert.areEqual("$", make("currency", { style: "narrow" }).of("USD"));
            assert.areEqual("$", make("currency", { style: "short" }).of("USD"));
            assert.areEqual("string", typeof make("region", { style: "short" }).of("US"));
            assert.areEqual("string", typeof make("script", { style: "narrow" }).of("Latn"));
            assert.areEqual("string", typeof make("language", { style: "narrow" }).of("en"));
        }
    },
    {
        name: "Non-English locales and data locale resolution",
        body: function () {
            assert.areEqual("Deutschland", new Intl.DisplayNames("de", { type: "region" }).of("DE"));
            assert.areEqual("Allemagne", new Intl.DisplayNames("fr", { type: "region" }).of("DE"));
            var dn = new Intl.DisplayNames(["xx-invalid", "de-DE"], { type: "region" });
            assert.areEqual("de-DE", dn.resolvedOptions().locale);
            assert.areEqual("en-US", new Intl.DisplayNames("en-US-u-nu-thai", { type: "region" }).resolvedOptions().locale,
                "unicode extensions are not part of the resolved locale");
            assert.areEqual("string", typeof new Intl.DisplayNames(undefined, { type: "region" }).resolvedOptions().locale);
            assert.areEqual("string", typeof new Intl.DisplayNames([], { type: "language" }).of("en"));
            assert.areEqual("string", typeof new Intl.DisplayNames("ja", { type: "dateTimeField" }).of("year"));
        }
    },
    {
        name: "fallback: none returns undefined, fallback: code returns the canonicalized code",
        body: function () {
            var none = function (type) { return make(type, { fallback: "none" }); };
            var code = function (type) { return make(type, { fallback: "code" }); };

            assert.isUndefined(none("region").of("XX"));
            assert.areEqual("XX", code("region").of("xx"));
            assert.areEqual("XX", make("region").of("xx"), "default fallback is code");
            assert.areEqual("United States", none("region").of("US"));

            assert.isUndefined(none("script").of("Xxxx"));
            assert.areEqual("Xxxx", code("script").of("xXXX"));
            assert.areEqual("Latin", none("script").of("Latn"));

            assert.isUndefined(none("currency").of("ABC"));
            assert.areEqual("ABC", code("currency").of("abc"));
            assert.areEqual("US Dollar", none("currency").of("USD"));
            ["narrow", "short", "long"].forEach(function (style) {
                assert.isUndefined(make("currency", { fallback: "none", style: style }).of("ABC"), style);
                assert.areEqual("ABC", make("currency", { fallback: "code", style: style }).of("abc"), style);
            });

            assert.isUndefined(none("calendar").of("abcdefgh"));
            assert.areEqual("abcdefgh", code("calendar").of("ABCDEFGH"));
            assert.areEqual("123-abc-abc", code("calendar").of("123-ABC-abc"));
            assert.areEqual("Gregorian Calendar", none("calendar").of("gregory"));

            assert.isUndefined(none("language").of("xx"));
            assert.isUndefined(none("language").of("zzzzzzzz"));
            assert.areEqual("xx", code("language").of("XX"));
            assert.areEqual("zzzzzzzz", code("language").of("zzzzzzzz"));
            assert.areEqual("English", none("language").of("en"));
            assert.areEqual("American English", none("language").of("en-US"));
            assert.areEqual("Abkhazian", none("language").of("ab"));
            // Canonicalization before the code fallback: aliases are replaced.
            assert.areEqual("string", typeof code("language").of("iw"));

            // dateTimeField has data for every legal field.
            assert.areEqual("year", none("dateTimeField").of("year"));
        }
    },
    {
        name: "Invalid codes throw RangeError for every type",
        body: function () {
            var bad = {
                language: ["", "a", "abcdefghi", "en-u-hebrew", "aa-aaaa-bbbb", "aa-aaaaa-aaaaa", "aa-bb-cc", "1a",
                           "aa-1a", "aa-1aa", "@#$%@#$", "en-US-", "-en", "en--GB", "root", "abcd-GB", "abcd", "en_GB",
                           "en-x-private", "en-t-fr", "\u00e9n", " en", "en "],
                region: ["", "a", "aaa", "1111", "11", "a01", "a1", "1a", "1a1", "-111", "_111", "111-", " aa", "aa ", "a c", "\u00e9\u00e9"],
                script: ["", "abc", "abcde", "abc1", "1abc", " abcd", "abcd ", "ab-cd", "\u00e9bcd"],
                currency: ["", "US", "USDX", "US1", "1US", "U D", " USD", "USD ", "\u00fcSD"],
                calendar: ["", "00", "000000000", "-00000000", "_00000000", "00000000-", "00000000_", " abcdef", "abcdef ",
                           "abc def", "123_abc", "123-ab", "123--abc", "gregory-", "\u00e9bcdef"],
                dateTimeField: ["", "timezoneName", "timezonename", "millisecond", "seconds", " year", "year ", "Year", "YEAR",
                                "weekofyear", "dayperiod", "\u0000", "year\u0000"]
            };
            Object.keys(bad).forEach(function (type) {
                var dn = make(type);
                bad[type].forEach(function (code) {
                    assert.throws(function () { dn.of(code); }, RangeError, type + ": " + JSON.stringify(code));
                    assert.throws(function () { make(type, { fallback: "none" }).of(code); }, RangeError, type + "/none: " + JSON.stringify(code));
                });
            });

            var language = make("language");
            assert.throws(function () { language.of(); }, RangeError, "undefined -> 'undefined' is not a valid language id");
            assert.throws(function () { language.of(null); }, RangeError, "'null' is not valid");
            assert.throws(function () { language.of(Symbol()); }, TypeError);
            assert.throws(function () { language.of(1n); }, RangeError);
            assert.areEqual("string", typeof make("region").of(840), "ToString(840) is a valid numeric region code");
            assert.throws(function () { make("region").of(8400); }, RangeError, "too long for a region");
        }
    },
    {
        name: "Valid code shapes for every type produce strings",
        body: function () {
            var language = make("language");
            ["ab", "cde", "zzzzzzzz", "ab-Abcd", "ab-AB", "ab-123", "ab-abcde", "ab-1abc", "ab-1nopq", "ab-12345678",
             "ab-Abcd-AB-abcde-fghijklm", "ab-Abcd-123-6d7e-2345-a2345678", "en-US-posix", "und", "zh-Hans-CN"].forEach(function (tag) {
                assert.areEqual("string", typeof language.of(tag), tag);
            });
            assert.areEqual("string", typeof make("region").of("123"));
            ["01234567", "899", "abcdefgh", "yzz", "ABCDEFGH", "123-abc", "12345678-abcdefgh-ABCDEFGH"].forEach(function (cal) {
                assert.areEqual("string", typeof make("calendar").of(cal), cal);
            });
        }
    },
    {
        name: "of() coerces the code once, then reacquires state (reentrant coercions and GC)",
        body: function () {
            var dn = make("region");
            var calls = 0;
            var arg = {
                toString: function () {
                    calls++;
                    collect();
                    // Allocate while the call is in flight so a stale view would be overwritten.
                    var junk = [];
                    for (var i = 0; i < 2000; i++) {
                        junk.push("x" + i);
                    }
                    collect();
                    return "US";
                }
            };
            assert.areEqual("United States", dn.of(arg));
            assert.areEqual(1, calls);

            // The receiver is dropped by the caller while toString runs; the call still completes.
            var holder = { dn: make("currency") };
            var res = holder.dn.of({
                toString: function () { holder.dn = null; collect(); return "EUR"; }
            });
            collect();
            assert.areEqual("Euro", res);

            // A toString that calls of() on the same object recursively.
            var inner;
            var outer = dn.of({
                toString: function () { inner = dn.of("DE"); return "US"; }
            });
            assert.areEqual("Germany", inner);
            assert.areEqual("United States", outer);

            // Throwing and Symbol conversions propagate their own errors.
            assert.throws(function () { dn.of({ toString: function () { throw new EvalError("e"); } }); }, EvalError);
            assert.throws(function () { dn.of({ valueOf: function () { throw new EvalError("e"); }, toString: undefined }); }, EvalError);
            assert.throws(function () { dn.of(Symbol("s")); }, TypeError);
            // Non-string primitives are coerced.
            assert.areEqual("Germany", dn.of({ toString: function () { return "DE"; } }));
            assert.areEqual("string", typeof make("language").of(new String("en")));
        }
    },
    {
        name: "Receivers: of and resolvedOptions require an Intl.DisplayNames instance",
        body: function () {
            var of = Intl.DisplayNames.prototype.of;
            var resolvedOptions = Intl.DisplayNames.prototype.resolvedOptions;
            var bad = [undefined, null, true, false, 1, "en", Symbol(), {}, [], function () {}, Intl, Intl.DisplayNames,
                       Intl.DisplayNames.prototype, new Intl.Collator(), new Intl.NumberFormat(), new Intl.DateTimeFormat(),
                       Object.create(Intl.DisplayNames.prototype)];
            bad.forEach(function (receiver, i) {
                assert.throws(function () { of.call(receiver, "US"); }, TypeError, "of " + i);
                assert.throws(function () { resolvedOptions.call(receiver); }, TypeError, "resolvedOptions " + i);
            });
            // The receiver is validated before the argument is coerced.
            var called = false;
            assert.throws(function () {
                of.call({}, { toString: function () { called = true; return "US"; } });
            }, TypeError);
            assert.isFalse(called);
            assert.throws(function () { new (Intl.DisplayNames.prototype.of)(); }, TypeError, "of is not a constructor");
        }
    },
    {
        name: "resolvedOptions has the specified shape",
        body: function () {
            var dn = new Intl.DisplayNames("en-US", { type: "language" });
            var options = dn.resolvedOptions();
            var other = dn.resolvedOptions();
            assert.isFalse(options === other, "each call returns a new object");
            assert.areEqual(Object.prototype, Object.getPrototypeOf(options));
            assert.areEqual(["locale", "style", "type", "fallback", "languageDisplay"], Reflect.ownKeys(options));
            assert.areEqual("en-US", options.locale);
            assert.areEqual("long", options.style);
            assert.areEqual("language", options.type);
            assert.areEqual("code", options.fallback);
            assert.areEqual("dialect", options.languageDisplay);
            Reflect.ownKeys(options).forEach(function (key) {
                var d = Object.getOwnPropertyDescriptor(options, key);
                assert.isTrue(d.writable && d.enumerable && d.configurable, key);
            });

            ["region", "script", "currency", "calendar", "dateTimeField"].forEach(function (type) {
                var o = new Intl.DisplayNames("en-US", { type: type, style: "narrow", fallback: "none", languageDisplay: "standard" }).resolvedOptions();
                assert.areEqual(["locale", "style", "type", "fallback"], Reflect.ownKeys(o), type);
                assert.areEqual("narrow", o.style);
                assert.areEqual(type, o.type);
                assert.areEqual("none", o.fallback);
                assert.isFalse(o.hasOwnProperty("languageDisplay"));
            });
            var o = new Intl.DisplayNames("en", { type: "language", languageDisplay: "standard", style: "short", fallback: "none", localeMatcher: "lookup", chaos: 1 }).resolvedOptions();
            assert.areEqual(["locale", "style", "type", "fallback", "languageDisplay"], Reflect.ownKeys(o));
            assert.areEqual("standard", o.languageDisplay);
            assert.areEqual("short", o.style);
            assert.areEqual("none", o.fallback);

            // Mutating a returned options object does not affect the instance.
            options.type = "region";
            options.fallback = "none";
            assert.areEqual("language", dn.resolvedOptions().type);
            assert.areEqual("code", dn.resolvedOptions().fallback);
        }
    },
    {
        name: "Option values are normalized from user strings (String objects, toString)",
        body: function () {
            var dn = new Intl.DisplayNames("en", {
                type: { toString: function () { return "region"; } },
                style: new String("short"),
                fallback: { toString: function () { return "none"; } }
            });
            var o = dn.resolvedOptions();
            assert.areEqual("region", o.type);
            assert.areEqual("short", o.style);
            assert.areEqual("none", o.fallback);
            assert.isUndefined(dn.of("XX"));
        }
    },
    {
        name: "supportedLocalesOf",
        body: function () {
            var supported = Intl.DisplayNames.supportedLocalesOf(["en-US", "de"]);
            assert.isTrue(Array.isArray(supported));
            assert.areEqual(["en-US", "de"], supported);
            assert.areEqual([], Intl.DisplayNames.supportedLocalesOf([]));
            assert.areEqual([], Intl.DisplayNames.supportedLocalesOf());
            assert.areEqual(["en"], Intl.DisplayNames.supportedLocalesOf("en"));
            assert.areEqual(["en"], Intl.DisplayNames.supportedLocalesOf("en", { localeMatcher: "lookup" }));
            assert.throws(function () { Intl.DisplayNames.supportedLocalesOf("en", { localeMatcher: "bogus" }); }, RangeError);
            assert.throws(function () { Intl.DisplayNames.supportedLocalesOf("e"); }, RangeError);
            assert.throws(function () { Intl.DisplayNames.supportedLocalesOf(null); }, TypeError);
            assert.areEqual(["en"], Intl.DisplayNames.supportedLocalesOf.call(undefined, "en"));
            assert.throws(function () { new Intl.DisplayNames.supportedLocalesOf(); }, TypeError);
        }
    },
    {
        name: "GC: finalization of unreferenced instances of every type; survivors keep working",
        body: function () {
            var types = ["language", "region", "script", "currency", "calendar", "dateTimeField"];
            var codes = { language: "en-US", region: "US", script: "Latn", currency: "USD", calendar: "gregory", dateTimeField: "year" };
            var survivors = [];
            for (var round = 0; round < 3; round++) {
                for (var i = 0; i < 300; i++) {
                    types.forEach(function (type) {
                        var dn = new Intl.DisplayNames(i % 2 ? "en" : "de", { type: type, fallback: i % 3 ? "code" : "none" });
                        if (i === 299) {
                            survivors.push(dn);
                        }
                        // Half of the instances are used before being dropped.
                        if (i % 2) {
                            assert.areEqual("string", typeof dn.of(codes[type]));
                        }
                    });
                }
                collect();
            }
            collect();
            assert.areEqual(6 * 3, survivors.length);
            survivors.forEach(function (dn, i) {
                var type = types[i % 6];
                assert.areEqual(type, dn.resolvedOptions().type);
                assert.areEqual("string", typeof dn.of(codes[type]));
            });
            assert.areEqual("United States", survivors[1].of("US"));
            assert.areEqual("US Dollar", survivors[3].of("USD"));

            // Construction that fails after ICU/state setup began must not leak or crash.
            for (var j = 0; j < 200; j++) {
                assert.throws(function () { new Intl.DisplayNames("en", { type: "region", languageDisplay: "x" }); }, RangeError);
                assert.throws(function () { new Intl.DisplayNames("en", { type: "region", fallback: "x" }); }, RangeError);
                assert.throws(function () { new Intl.DisplayNames("en", { type: "dateTimeField", fallback: "x" }); }, RangeError);
            }
            collect();

            // Objects created through Reflect.construct and dropped immediately.
            for (var k = 0; k < 500; k++) {
                Reflect.construct(Intl.DisplayNames, ["en", { type: "language" }], Object);
                Reflect.construct(Intl.DisplayNames, ["en", { type: "dateTimeField" }], function () {});
            }
            collect();
            assert.areEqual("English", make("language").of("en"));
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
