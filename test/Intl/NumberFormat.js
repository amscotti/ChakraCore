//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

let suppressFormatEqualityCheck = false;

function format() {
    let locale = "en-US", options, n;
    assert.isTrue(arguments.length > 0);

    if (typeof arguments[0] === "number") {
        [n] = arguments;
    } else if (typeof arguments[0] === "object" && !(arguments[0] instanceof Array)) {
        [options, n] = arguments;
    } else {
        [locale, options, n] = arguments;
    }

    const nf = new Intl.NumberFormat(locale, options);
    const format = nf.format(n);
    const localeString = n.toLocaleString(locale, options);

    assert.isTrue(format === localeString, `[locale = ${JSON.stringify(locale)}, options = ${JSON.stringify(options)}] format does not match toLocaleString`);
    if (WScript.Platform.INTL_LIBRARY === "icu" && !suppressFormatEqualityCheck) {
        assert.isTrue(format === nf.formatToParts(n).map((part) => part.value).join(""), `[locale = ${JSON.stringify(locale)}, options = ${JSON.stringify(options)}] format does not match formatToParts`);
    }

    return format;
}

const tests = [
    {
        name: "Decimal style default options",
        body: function () {
            assert.areEqual("5", format(5));
            assert.areEqual("5,000", format(5000));
            assert.areEqual("50.474", format(50.474));
        }
    },
    {
        name: "Min/max fractional digits",
        body: function () {
            // min
            assert.areEqual("5.00", format({ minimumFractionDigits: 2 }, 5));
            assert.areEqual("5.0", format({ minimumFractionDigits: 1 }, 5));

            // min and max
            assert.areEqual("5.00", format({ minimumFractionDigits: 2, maximumFractionDigits: 2 }, 5));
            assert.areEqual("5.0", format({ minimumFractionDigits: 1, maximumFractionDigits: 2 }, 5));

            // max
            assert.areEqual("5.44", format({ maximumFractionDigits: 2 }, 5.444));
            assert.areEqual("5.444", format({ maximumFractionDigits: 4 }, 5.444));
            assert.areEqual("5.45", format({ maximumFractionDigits: 2 }, 5.445));
            assert.areEqual("5.445", format({ maximumFractionDigits: 4 }, 5.445));
            assert.areEqual("5.55", format({ maximumFractionDigits: 2 }, 5.554));
            assert.areEqual("5.554", format({ maximumFractionDigits: 4 }, 5.554));
            assert.areEqual("5", format({ maximumFractionDigits: 0 }, 5.45));
            assert.areEqual("6", format({ maximumFractionDigits: 0 }, 5.5));
        }
    },
    {
        name: "Min integer digits",
        body: function () {
            assert.areEqual("5", format({ minimumIntegerDigits: 1 }, 5));
            assert.areEqual("05", format({ minimumIntegerDigits: 2 }, 5));
            assert.areEqual("0,000,000,005", format({ minimumIntegerDigits: 10 }, 5));
            assert.areEqual("500", format({ minimumIntegerDigits: 1 }, 500));
            assert.areEqual("0,000,000,500", format({ minimumIntegerDigits: 10 }, 500));
        }
    },
    {
        name: "Min/max significant digits",
        body: function () {
            // min
            assert.areEqual("5.0", format({ minimumSignificantDigits: 2 }, 5));
            assert.areEqual("500", format({ minimumSignificantDigits: 2 }, 500));
            assert.areEqual("500.0", format({ minimumSignificantDigits: 4 }, 500));

            // min and max
            assert.areEqual("5.0", format({ minimumSignificantDigits: 2, maximumSignificantDigits: 2 }, 5));
            assert.areEqual("5", format({ minimumSignificantDigits: 1, maximumSignificantDigits: 2 }, 5));

            // max
            assert.areEqual("5.44", format({ maximumSignificantDigits: 3 }, 5.444));
            assert.areEqual("5.444", format({ maximumSignificantDigits: 4 }, 5.4444));
            assert.areEqual("5.45", format({ maximumSignificantDigits: 3 }, 5.445));
            assert.areEqual("5.445", format({ maximumSignificantDigits: 4 }, 5.4445));
            assert.areEqual("5.55", format({ maximumSignificantDigits: 3 }, 5.554));
        }
    },
    {
        name: "Grouping separator",
        body: function () {
            assert.areEqual("50,000", format({ useGrouping: true }, 50000));
            assert.areEqual("50000", format({ useGrouping: false }, 50000));
            assert.areEqual("0000000005", format({ minimumIntegerDigits: 10, useGrouping: false }, 5));
            assert.areEqual("0000005000", format({ minimumIntegerDigits: 10, useGrouping: false }, 5000));
        }
    },
    {
        name: "Default style option combinations",
        body: function () {
            assert.areEqual("123", format({ minimumSignificantDigits: 3, maximumSignificantDigits: 3, minimumIntegerDigits: 5, minimumFractionDigits: 5, maximumFractionDigits: 5 }, 123.1));
            assert.areEqual("00,123.10000", format({ minimumIntegerDigits: 5, minimumFractionDigits: 5, maximumFractionDigits: 5 }, 123.1))
        }
    },
    {
        name: "Currency style",
        body: function () {
            function formatCurrency() {
                let locale = "en-US", currency = "USD", options, n;
                assert.isTrue(arguments.length > 0);

                if (typeof arguments[0] === "number") {
                    [n] = arguments;
                } else if (typeof arguments[0] === "object") {
                    [options, n] = arguments;
                } else if (arguments.length === 3) {
                    [currency, options, n] = arguments;
                } else {
                    [locale, currency, options, n] = arguments;
                }

                options = options || {};
                options.style = "currency",
                options.currency = currency;

                return format(locale, options, n)
            }

            assert.areEqual("$1.00", formatCurrency(1));
            assert.areEqual("$1.50", formatCurrency(1.50));
            assert.areEqual("$1.50", formatCurrency(1.504));
            assert.areEqual("$1.51", formatCurrency(1.505));
            assert.matches(/USD[\x20\u00a0]?1.00/, formatCurrency({ currencyDisplay: "code" }, 1), "Currency display: code");
            assert.matches(/USD[\x20\u00a0]?1.50/, formatCurrency({ currencyDisplay: "code" }, 1.504), "Currency display: code");
            assert.matches(/USD[\x20\u00a0]?1.51/, formatCurrency({ currencyDisplay: "code" }, 1.505), "Currency display: code");
            assert.areEqual("$1.00", formatCurrency({ currencyDisplay: "symbol" }, 1), "Currency display: symbol");
            assert.areEqual("$1.50", formatCurrency({ currencyDisplay: "symbol" }, 1.504), "Currency display: symbol");
            assert.areEqual("$1.51", formatCurrency({ currencyDisplay: "symbol" }, 1.505), "Currency display: symbol");
            // ICU has a proper "name" currency display, while WinGlob falls back to "code"
            if (WScript.Platform.ICU_VERSION >= 62) {
                // In ICU 62, there is a mismatch between "1.00 US dollar" and "1.00 US dollars"
                suppressFormatEqualityCheck = true;
            }
            assert.matches(/(?:USD[\x20\u00a0]?1.00|1.00 US dollars)/, formatCurrency({ currencyDisplay: "name" }, 1), "Currency display: name");
            suppressFormatEqualityCheck = false;
            assert.matches(/(?:USD[\x20\u00a0]?1.50|1.50 US dollars)/, formatCurrency({ currencyDisplay: "name" }, 1.504), "Currency display: name");
            assert.matches(/(?:USD[\x20\u00a0]?1.51|1.51 US dollars)/, formatCurrency({ currencyDisplay: "name" }, 1.505), "Currency display: name");
        }
    },
    {
        name: "Percent style",
        body: function () {
            assert.matches(/100[\x20\u00a0]?%/, format({ style: "percent" }, 1));
            assert.matches(/1[\x20\u00a0]?%/, format({ style: "percent" }, 0.01));
            assert.matches(/10,000[\x20\u00a0]?%/,format({ style: "percent" }, 100));
        }
    },
    {
        name: "Negative 0 (https://github.com/tc39/ecma402/issues/219)",
        body() {
            assert.areEqual(
                0,
                new Intl.NumberFormat(undefined, { minimumFractionDigits: -0 }).resolvedOptions().minimumFractionDigits,
                "Passing -0 for minimumFractionDigits should get normalized to 0 by DefaultNumberOption"
            );

            assert.areEqual("-0", (-0).toLocaleString(), "-0 should be treated as a negative number (toLocaleString)");
            assert.areEqual("-0", new Intl.NumberFormat().format(-0), "-0 should be treated as a negative number (NumberFormat.prototype.format)");
            if (WScript.Platform.INTL_LIBRARY === "icu") {
                assert.areEqual("-0", new Intl.NumberFormat().formatToParts(-0).map(v => v.value).join(""), "-0 should be treated as a negative number (NumberFormat.prototype.formatToParts)");
            }
        }
    },
    {
        name: "formatToParts",
        body() {
            if (WScript.Platform.INTL_LIBRARY === "winglob") {
                return;
            }

            function assertParts(locale, options, n, expectedParts) {
                const nf = new Intl.NumberFormat(locale, options);
                const resolved = nf.resolvedOptions();
                assert.areEqual(locale, resolved.locale, `This test requires ${locale} support`);
                if (options) {
                    for (const opt of Object.getOwnPropertyNames(options)) {
                        assert.areEqual(options[opt], resolved[opt], `Bad option resolution for option ${opt}`)
                    }
                }

                const actualParts = nf.formatToParts(n);
                assert.isTrue(Array.isArray(actualParts), `formatToParts(${n}) did not return an array`);

                if (WScript.Platform.ICU_VERSION < 61) {
                    // real formatToParts support was only added with ICU 61
                    assert.areEqual(1, actualParts.length, `formatToParts(${n}) stub implementation should return only one part`);
                    const literal = actualParts[0];
                    assert.areEqual("unknown", literal.type, `formatToParts(${n}) stub implementation should return an unknown part`);
                    assert.areEqual(nf.format(n), literal.value, `formatToParts(${n}) stub implementation should return one part whose value matches the fully formatted number`);
                    return;
                }

                assert.areEqual(expectedParts.length, actualParts.length, `formatToParts(${n}) returned wrong number of parts (actual: ${JSON.stringify(actualParts, null, 2)})`);
                expectedParts.forEach((part, i) => {
                    assert.areEqual(expectedParts[i].type, actualParts[i].type, `formatToParts(${n}) returned bad type for part ${i}`);
                    assert.areEqual(expectedParts[i].value, actualParts[i].value, `formatToParts(${n}) returned bad value for part ${i} (code points: ${actualParts[i].value.split("").map(char => char.charCodeAt(0)).toString()})`);
                })
            }

            assertParts("en-US", undefined, 1000, [
                { type: "integer" , value: "1" },
                { type: "group", value: "," },
                { type: "integer", value: "000" }
            ]);
            assertParts("en-US", undefined, -1000, [
                { type: "minusSign", value: "-" },
                { type: "integer" , value: "1" },
                { type: "group", value: "," },
                { type: "integer", value: "000" }
            ]);
            if (WScript.Platform.ICU_VERSION !== 62) {
                assertParts("en-US", undefined, NaN, [{ type: "nan", value: "NaN" }]);
            }
            assertParts("en-US", undefined, Infinity, [{ type: "infinity", value: "∞" }]);
            assertParts("en-US", undefined, 1000.3423, [
                { type: "integer", value: "1" },
                { type: "group", value: "," },
                { type: "integer", value: "000" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "342" }
            ]);
            assertParts("en-US", { minimumFractionDigits: 5 }, 1000.3423, [
                { type: "integer", value: "1" },
                { type: "group", value: "," },
                { type: "integer", value: "000" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "34230" }
            ]);
            assertParts("en-US", { style: "currency", currency: "CAD", currencyDisplay: "name" }, 1000.3423, [
                { type: "integer", value: "1" },
                { type: "group", value: "," },
                { type: "integer", value: "000" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "34" },
                { type: "literal", value: " " },
                { type: "currency", value: "Canadian dollars" }
            ]);
            assertParts("en-US", { style: "percent", minimumSignificantDigits: 4 }, 0.3423, [
                { type: "integer", value: "34" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "23" },
                { type: "percent", value: "%" }
            ]);

            assertParts("de-DE", { minimumFractionDigits: 5 }, 1000.3423, [
                { type: "integer", value: "1" },
                { type: "group", value: "." },
                { type: "integer", value: "000" },
                { type: "decimal", value: "," },
                { type: "fraction", value: "34230" }
            ]);
            assertParts("de-DE", { style: "currency", currency: "CAD", currencyDisplay: "name" }, 1000.3423, [
                { type: "integer", value: "1" },
                { type: "group", value: "." },
                { type: "integer", value: "000" },
                { type: "decimal", value: "," },
                { type: "fraction", value: "34" },
                { type: "literal", value: " " },
                { type: "currency", value: "Kanadische Dollar" }
            ]);
            assertParts("de-DE", { style: "percent", minimumSignificantDigits: 4 }, 0.3423, [
                { type: "integer", value: "34" },
                { type: "decimal", value: "," },
                { type: "fraction", value: "23" },
                { type: "literal", value: "\u00A0" }, // non-breaking space
                { type: "percent", value: "%" }
            ]);
        }
    },
    {
        name: "BigInt exact formatting (FormatNumericToString)",
        body() {
            if (WScript.Platform.INTL_LIBRARY === "winglob") {
                return; // Windows.Globalization has no exact BigInt path
            }

            const big = 123456789012345678901234567890n;
            const negBig = -123456789012345678901234567890n;

            function assertBigInt(locale, options, n, expected, expectedParts) {
                const nf = new Intl.NumberFormat(locale, options);
                assert.areEqual(locale, nf.resolvedOptions().locale, `This test requires ${locale} support`);

                const formatted = nf.format(n);
                assert.areEqual(expected, formatted, `format(${n}) [${locale} ${JSON.stringify(options)}]`);

                const actualParts = nf.formatToParts(n);
                assert.isTrue(Array.isArray(actualParts), `formatToParts(${n}) did not return an array`);
                assert.areEqual(expected, actualParts.map((part) => part.value).join(""), `formatToParts(${n}) join mismatch`);

                if (expectedParts !== undefined) {
                    assert.areEqual(expectedParts.length, actualParts.length, `formatToParts(${n}) returned wrong number of parts`);
                    expectedParts.forEach((part, i) => {
                        assert.areEqual(part.type, actualParts[i].type, `formatToParts(${n}) returned bad type for part ${i}`);
                        assert.areEqual(part.value, actualParts[i].value, `formatToParts(${n}) returned bad value for part ${i}`);
                    });
                }
            }

            // Exact decimal output across grouping locales (uniform, dot-group, 3-2-2).
            assertBigInt("en-US", undefined, big, "123,456,789,012,345,678,901,234,567,890");
            assertBigInt("de-DE", undefined, big, "123.456.789.012.345.678.901.234.567.890");
            assertBigInt("en-IN", undefined, big, "1,23,45,67,89,01,23,45,67,89,01,23,45,67,890");

            // Values straddling 2^53 must not lose precision; negatives keep their sign.
            assertBigInt("en-US", undefined, 9007199254740993n, "9,007,199,254,740,993");
            assertBigInt("en-US", undefined, 9007199254740992n, "9,007,199,254,740,992");
            assertBigInt("en-US", undefined, negBig, "-123,456,789,012,345,678,901,234,567,890", [
                { type: "minusSign", value: "-" },
                { type: "integer", value: "123" },
                { type: "group", value: "," },
                { type: "integer", value: "456" },
                { type: "group", value: "," },
                { type: "integer", value: "789" },
                { type: "group", value: "," },
                { type: "integer", value: "012" },
                { type: "group", value: "," },
                { type: "integer", value: "345" },
                { type: "group", value: "," },
                { type: "integer", value: "678" },
                { type: "group", value: "," },
                { type: "integer", value: "901" },
                { type: "group", value: "," },
                { type: "integer", value: "234" },
                { type: "group", value: "," },
                { type: "integer", value: "567" },
                { type: "group", value: "," },
                { type: "integer", value: "890" }
            ]);
            assertBigInt("en-US", undefined, 0n, "0", [{ type: "integer", value: "0" }]);
            assertBigInt("en-US", undefined, 5n, "5", [{ type: "integer", value: "5" }]);

            // Small BigInt values agree with their Number counterparts.
            assert.areEqual(new Intl.NumberFormat("en-US").format(5000), new Intl.NumberFormat("en-US").format(5000n), "small BigInt/Number parity");

            // Digit options.
            assertBigInt("en-US", { minimumFractionDigits: 2 }, 5n, "5.00", [
                { type: "integer", value: "5" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "00" }
            ]);
            assertBigInt("en-US", { minimumIntegerDigits: 5 }, 42n, "00,042");
            assertBigInt("en-US", { useGrouping: false }, big, "123456789012345678901234567890");

            // Percent scales by 100 exactly.
            // NOTE: this engine reports the percent-sign part as "percent" on both the
            // Number and BigInt paths (Node/spec say "percentSign"); pinned as-is.
            assertBigInt("en-US", { style: "percent" }, 5n, "500%", [
                { type: "integer", value: "500" },
                { type: "percent", value: "%" }
            ]);
            assertBigInt("en-US", { style: "percent" }, big, "12,345,678,901,234,567,890,123,456,789,000%");

            // Currency styles.
            assertBigInt("en-US", { style: "currency", currency: "USD" }, 5n, "$5.00", [
                { type: "currency", value: "$" },
                { type: "integer", value: "5" },
                { type: "decimal", value: "." },
                { type: "fraction", value: "00" }
            ]);
            assertBigInt("en-US", { style: "currency", currency: "USD" }, -5n, "-$5.00");
            assertBigInt("en-US", { style: "currency", currency: "JPY" }, 5n, "¥5");
            assertBigInt("de-DE", { style: "currency", currency: "EUR" }, 5n, "5,00\u00A0€");

            // Significant digits round exactly (half away from zero).
            assertBigInt("en-US", { maximumSignificantDigits: 3 }, 123456789n, "123,000,000");
            assertBigInt("en-US", { minimumSignificantDigits: 5 }, 123n, "123.00");
            assertBigInt("en-US", { maximumSignificantDigits: 2 }, -1250n, "-1,300");

            // Non-Latin digits map through the formatter's zero digit.
            assertBigInt("ar-EG", undefined, big, "١٢٣٬٤٥٦٬٧٨٩٬٠١٢٬٣٤٥٬٦٧٨٬٩٠١٬٢٣٤٬٥٦٧٬٨٩٠");
        }
    }
];

testRunner.runTests(tests, { verbose: false });
