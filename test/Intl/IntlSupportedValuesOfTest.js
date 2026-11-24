//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused tests for Intl.supportedValuesOf(key): metadata, coercion/errors,
// Array.prototype taint immunity, sorting/uniqueness, all 6 keys, cross-API parity, and GC stability.
// Run also with: ch -CollectGarbage IntlSupportedValuesOfTest.js -args summary -endargs

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function collect() {
    if (typeof CollectGarbage === "function") {
        CollectGarbage();
    }
}

var ALL_KEYS = ["calendar", "collation", "currency", "numberingSystem", "timeZone", "unit"];

var EXPECTED_CALENDARS = [
    "buddhist",
    "chinese",
    "coptic",
    "dangi",
    "ethioaa",
    "ethiopic",
    "gregory",
    "hebrew",
    "indian",
    "islamic-civil",
    "islamic-tbla",
    "islamic-umalqura",
    "iso8601",
    "japanese",
    "persian",
    "roc"
];

var EXPECTED_UNITS = [
    "acre",
    "bit",
    "byte",
    "celsius",
    "centimeter",
    "day",
    "degree",
    "fahrenheit",
    "fluid-ounce",
    "foot",
    "gallon",
    "gigabit",
    "gigabyte",
    "gram",
    "hectare",
    "hour",
    "inch",
    "kilobit",
    "kilobyte",
    "kilogram",
    "kilometer",
    "liter",
    "megabit",
    "megabyte",
    "meter",
    "microsecond",
    "mile",
    "mile-scandinavian",
    "milliliter",
    "millimeter",
    "millisecond",
    "minute",
    "month",
    "nanosecond",
    "ounce",
    "percent",
    "petabyte",
    "pound",
    "second",
    "stone",
    "terabit",
    "terabyte",
    "week",
    "yard",
    "year"
];

var tests = [
    {
        name: "Function identity, property descriptors, prototype, and non-constructibility",
        body: function () {
            assert.areEqual("function", typeof Intl.supportedValuesOf);
            assert.isFalse(Object.prototype.hasOwnProperty.call(Intl.supportedValuesOf, "prototype"));
            assert.isTrue(Object.isExtensible(Intl.supportedValuesOf));
            assert.areEqual(Function.prototype, Object.getPrototypeOf(Intl.supportedValuesOf));

            var desc = Object.getOwnPropertyDescriptor(Intl, "supportedValuesOf");
            assert.isTrue(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);

            desc = Object.getOwnPropertyDescriptor(Intl.supportedValuesOf, "length");
            assert.areEqual(1, desc.value);
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);

            desc = Object.getOwnPropertyDescriptor(Intl.supportedValuesOf, "name");
            assert.areEqual("supportedValuesOf", desc.value);
            assert.isFalse(desc.writable);
            assert.isFalse(desc.enumerable);
            assert.isTrue(desc.configurable);

            assert.throws(function () { new Intl.supportedValuesOf("unit"); }, TypeError);
            assert.throws(function () { Reflect.construct(Intl.supportedValuesOf, ["unit"]); }, TypeError);

            var detached = Intl.supportedValuesOf;
            assert.areEqual(EXPECTED_UNITS, detached("unit"));
            assert.areEqual(EXPECTED_UNITS, detached.call(null, "unit"));
        }
    },
    {
        name: "Key ToString coercion, reentrancy, Symbol TypeError, and invalid key RangeError",
        body: function () {
            var expectedCalendars = Intl.supportedValuesOf("calendar");

            assert.areEqual(expectedCalendars, Intl.supportedValuesOf(new String("calendar")));
            assert.areEqual(expectedCalendars, Intl.supportedValuesOf({ toString: function () { return "calendar"; } }));
            assert.areEqual(expectedCalendars, Intl.supportedValuesOf({ toString: null, valueOf: function () { return "calendar"; } }));

            // Concatenated / rope string key must work identically.
            var part = "numbering";
            assert.areEqual(
                Intl.supportedValuesOf("numberingSystem"),
                Intl.supportedValuesOf(part + "System")
            );

            // Reentrant GC inside user ToString coercion must remain safe.
            var gcKey = {
                toString: function () {
                    collect();
                    return "calendar";
                }
            };
            assert.areEqual(expectedCalendars, Intl.supportedValuesOf(gcKey));

            // User exceptions from ToString / valueOf propagate unchanged.
            function CustomError() {}
            assert.throws(function () {
                Intl.supportedValuesOf({ toString: function () { throw new CustomError(); } });
            }, CustomError);
            assert.throws(function () {
                Intl.supportedValuesOf({ toString: null, valueOf: function () { throw new CustomError(); } });
            }, CustomError);

            // Symbol and ToPrimitive returning Symbol throw TypeError.
            assert.throws(function () { Intl.supportedValuesOf(Symbol()); }, TypeError);
            assert.throws(function () {
                Intl.supportedValuesOf({ toString: function () { return Symbol(); } });
            }, TypeError);
            assert.throws(function () {
                Intl.supportedValuesOf({ toString: null, valueOf: function () { return Symbol(); } });
            }, TypeError);

            // Missing / invalid keys throw RangeError.
            assert.throws(function () { Intl.supportedValuesOf(); }, RangeError);
            var invalidKeys = [
                "",
                "hourCycle",
                "locale",
                "language",
                "script",
                "region",
                "calendars",
                "collations",
                "currencies",
                "numberingSystems",
                "timeZones",
                "units",
                "CALENDAR",
                "Collation",
                "Currency",
                "numberingsystem",
                "timezone",
                "UNIT",
                "calendar\0",
                "unit\0",
                "calendar ",
                " calendar",
                undefined,
                null,
                false,
                true,
                NaN,
                0,
                Math.PI,
                123n,
                {},
                []
            ];
            for (var i = 0; i < invalidKeys.length; i++) {
                (function (k) {
                    assert.throws(function () { Intl.supportedValuesOf(k); }, RangeError);
                })(invalidKeys[i]);
            }
        }
    },
    {
        name: "Freshness, mutability, Array.prototype taint immunity, sorting, and uniqueness",
        body: function () {
            for (var k = 0; k < ALL_KEYS.length; k++) {
                var key = ALL_KEYS[k];
                var first = Intl.supportedValuesOf(key);
                var second = Intl.supportedValuesOf(key);

                assert.isTrue(Array.isArray(first), key);
                assert.areEqual(Array.prototype, Object.getPrototypeOf(first), key);
                assert.isTrue(first !== second, key + " returns fresh array");
                assert.isTrue(first.length > 0, key + " non-empty");
                assert.areEqual(first.slice().sort(), first, key + " sorted");
                assert.areEqual(new Set(first).size, first.length, key + " unique");

                var elemDesc = Object.getOwnPropertyDescriptor(first, 0);
                assert.isTrue(elemDesc.writable, key);
                assert.isTrue(elemDesc.enumerable, key);
                assert.isTrue(elemDesc.configurable, key);

                // Mutating returned array does not affect subsequent calls.
                first[0] = "mutated";
                first.push("extra");
                first.length = 0;
                assert.areEqual(second, Intl.supportedValuesOf(key), key + " unaffected by caller mutation");
            }

            // Tainting Array.prototype indices must not fire setters or alter results.
            var indices = [0, 1, 5, 15];
            try {
                for (var idx = 0; idx < indices.length; idx++) {
                    Object.defineProperty(Array.prototype, indices[idx], {
                        configurable: true,
                        get: function () { throw new Error("Array.prototype getter invoked"); },
                        set: function () { throw new Error("Array.prototype setter invoked"); }
                    });
                }
                for (var m = 0; m < ALL_KEYS.length; m++) {
                    var res = Intl.supportedValuesOf(ALL_KEYS[m]);
                    assert.isTrue(res.length > 0);
                    assert.areEqual("string", typeof res[0]);
                }
            } finally {
                for (var idx2 = 0; idx2 < indices.length; idx2++) {
                    delete Array.prototype[indices[idx2]];
                }
            }
        }
    },
    {
        name: "calendar enumeration and cross-API consistency",
        body: function () {
            var calendars = Intl.supportedValuesOf("calendar");
            assert.areEqual(EXPECTED_CALENDARS, calendars);
            assert.isFalse(calendars.indexOf("islamic") !== -1);
            assert.isFalse(calendars.indexOf("islamic-rgsa") !== -1);
            assert.isFalse(calendars.indexOf("unknown") !== -1);

            var dn = new Intl.DisplayNames("en", { type: "calendar", fallback: "none" });
            var typeRE = /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/;
            for (var i = 0; i < calendars.length; i++) {
                var cal = calendars[i];
                assert.isTrue(typeRE.test(cal), cal);
                assert.areEqual(cal, new Intl.Locale("und", { calendar: cal }).calendar, cal);
                assert.areEqual("string", typeof dn.of(cal), cal + " DisplayNames");
            }
        }
    },
    {
        name: "collation enumeration and Intl.Locale consistency",
        body: function () {
            var collations = Intl.supportedValuesOf("collation");
            assert.isFalse(collations.indexOf("standard") !== -1);
            assert.isFalse(collations.indexOf("search") !== -1);
            assert.isFalse(collations.indexOf("unknown") !== -1);

            var typeRE = /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/;
            for (var i = 0; i < collations.length; i++) {
                var col = collations[i];
                assert.isTrue(typeRE.test(col), col);
                assert.areEqual(col, new Intl.Locale("und", { collation: col }).collation, col);
            }
        }
    },
    {
        name: "currency enumeration and DisplayNames / NumberFormat parity",
        body: function () {
            var currencies = Intl.supportedValuesOf("currency");
            var dn = new Intl.DisplayNames("en", { type: "currency", fallback: "none" });
            var codeRE = /^[A-Z]{3}$/;

            assert.isTrue(currencies.indexOf("USD") !== -1);
            assert.isTrue(currencies.indexOf("EUR") !== -1);
            assert.isTrue(currencies.indexOf("JPY") !== -1);

            for (var i = 0; i < currencies.length; i++) {
                var cur = currencies[i];
                assert.isTrue(codeRE.test(cur), cur);
                assert.areEqual("string", typeof dn.of(cur), cur + " DisplayNames");
                assert.areEqual(
                    cur,
                    new Intl.NumberFormat("en", { style: "currency", currency: cur }).resolvedOptions().currency,
                    cur + " NumberFormat"
                );
            }

            // Historical currencies whose CLDR display data varies across ICU versions
            // (e.g., ICU 74/78 define "CNX" while ICU 79 removed it): assert the
            // cross-API iff bijection rather than hard-coding presence or absence.
            var historical = ["CNX", "LSM"];
            for (var h = 0; h < historical.length; h++) {
                var hist = historical[h];
                var inCurrencies = currencies.indexOf(hist) !== -1;
                var displayName = dn.of(hist);
                assert.areEqual(inCurrencies, typeof displayName === "string", hist + " parity");
                if (inCurrencies) {
                    assert.isTrue(displayName.length > 0, hist + " non-empty DisplayNames");
                    assert.areEqual(
                        hist,
                        new Intl.NumberFormat("en", { style: "currency", currency: hist }).resolvedOptions().currency,
                        hist + " NumberFormat"
                    );
                } else {
                    assert.areEqual(undefined, displayName, hist + " undefined DisplayNames");
                }
            }

            // Verify permanently unassigned ISO 4217 codes are absent and rejected by DisplayNames.
            var unassigned = ["AAA", "QZZ", "ZZZ"];
            for (var j = 0; j < unassigned.length; j++) {
                assert.areEqual(undefined, dn.of(unassigned[j]), unassigned[j]);
                assert.isFalse(currencies.indexOf(unassigned[j]) !== -1, unassigned[j]);
            }
        }
    },
    {
        name: "numberingSystem enumeration and Locale / RelativeTimeFormat consistency",
        body: function () {
            var systems = Intl.supportedValuesOf("numberingSystem");
            assert.isTrue(systems.indexOf("latn") !== -1);
            assert.isTrue(systems.indexOf("arab") !== -1);
            assert.isTrue(systems.indexOf("hanidec") !== -1);
            assert.isFalse(systems.indexOf("finance") !== -1);
            assert.isFalse(systems.indexOf("native") !== -1);
            assert.isFalse(systems.indexOf("traditio") !== -1);
            assert.isFalse(systems.indexOf("unknown") !== -1);

            var typeRE = /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/;
            for (var i = 0; i < systems.length; i++) {
                var nu = systems[i];
                assert.isTrue(typeRE.test(nu), nu);
                assert.areEqual(nu, new Intl.Locale("und", { numberingSystem: nu }).numberingSystem, nu);
                assert.areEqual(
                    nu,
                    new Intl.RelativeTimeFormat("en", { numberingSystem: nu }).resolvedOptions().numberingSystem,
                    nu + " RelativeTimeFormat"
                );
            }
        }
    },
    {
        name: "timeZone enumeration and DateTimeFormat consistency",
        body: function () {
            var zones = Intl.supportedValuesOf("timeZone");
            assert.isTrue(zones.indexOf("UTC") !== -1);
            assert.isTrue(zones.indexOf("America/New_York") !== -1);
            assert.isTrue(zones.indexOf("Etc/GMT+1") !== -1);
            assert.isTrue(zones.indexOf("Etc/GMT-14") !== -1);
            assert.isFalse(zones.indexOf("Etc/UTC") !== -1);
            assert.isFalse(zones.indexOf("Etc/GMT") !== -1);
            assert.isFalse(zones.indexOf("Etc/Unknown") !== -1);
            assert.isFalse(zones.indexOf("Factory") !== -1);

            for (var i = 0; i < zones.length; i++) {
                var tz = zones[i];
                assert.areEqual(
                    tz,
                    new Intl.DateTimeFormat("en", { timeZone: tz }).resolvedOptions().timeZone,
                    tz + " DateTimeFormat"
                );
            }
        }
    },
    {
        name: "unit enumeration exact sanctioned list",
        body: function () {
            var units = Intl.supportedValuesOf("unit");
            assert.areEqual(EXPECTED_UNITS, units);
            for (var i = 0; i < units.length; i++) {
                assert.areEqual(-1, units[i].indexOf("-per-"), units[i]);
            }
        }
    },
    {
        name: "GC stability across repeated enumeration and failure paths",
        body: function () {
            var baseline = {};
            for (var k = 0; k < ALL_KEYS.length; k++) {
                baseline[ALL_KEYS[k]] = Intl.supportedValuesOf(ALL_KEYS[k]);
            }

            for (var round = 0; round < 5; round++) {
                for (var i = 0; i < ALL_KEYS.length; i++) {
                    var key = ALL_KEYS[i];
                    var current = Intl.supportedValuesOf(key);
                    assert.areEqual(baseline[key], current, key + " round " + round);
                }
                assert.throws(function () { Intl.supportedValuesOf("invalid"); }, RangeError);
                assert.throws(function () { Intl.supportedValuesOf(Symbol()); }, TypeError);
                collect();
            }
        }
    }
];

testRunner.runTests(tests, { verbose: WScript.Arguments[0] != "summary" });
