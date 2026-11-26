//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Focused regression tests for Intl hardening (#546):
// 1. FormatBigIntWithNumberFormat with non-contiguous ("hanidec") and surrogate-pair ("mathbold") digits.
// 2. Intl.Locale / Intl.DurationFormat / Intl.RelativeTimeFormat bounds, brand rejection, and error paths.
// 3. Multi-realm FinalizableICUObject teardown and WScript.LoadScript error context restoration.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

const tests = [
    {
        name: "BigInt NumberFormat hanidec digits and formatToParts parity with Number",
        body: function () {
            const nf = new Intl.NumberFormat("en-US-u-nu-hanidec");
            assert.areEqual(nf.format(0), nf.format(0n), "hanidec 0");
            assert.areEqual("\u3007", nf.format(0n), "hanidec 0 literal");
            assert.areEqual(nf.format(1234567890), nf.format(1234567890n), "hanidec 1234567890");
            assert.areEqual(
                "\u4e00,\u4e8c\u4e09\u56db,\u4e94\u516d\u4e03,\u516b\u4e5d\u3007",
                nf.format(1234567890n),
                "hanidec exact glyphs"
            );
            assert.areEqual(nf.format(-9876543210), nf.format(-9876543210n), "hanidec negative");
            assert.areEqual(
                nf.formatToParts(1234567890),
                nf.formatToParts(1234567890n),
                "hanidec formatToParts parity"
            );

            const big = 123456789012345678901234567890n;
            const formattedBig = nf.format(big);
            assert.areEqual(
                "\u4e00\u4e8c\u4e09,\u56db\u4e94\u516d,\u4e03\u516b\u4e5d,\u3007\u4e00\u4e8c,\u4e09\u56db\u4e94,\u516d\u4e03\u516b,\u4e5d\u3007\u4e00,\u4e8c\u4e09\u56db,\u4e94\u516d\u4e03,\u516b\u4e5d\u3007",
                formattedBig,
                "hanidec large BigInt"
            );

            const nfFrac = new Intl.NumberFormat("en-US-u-nu-hanidec", { minimumFractionDigits: 2 });
            assert.areEqual(nfFrac.format(42), nfFrac.format(42n), "hanidec minFrac");

            const nfPct = new Intl.NumberFormat("en-US-u-nu-hanidec", { style: "percent" });
            assert.areEqual(nfPct.format(5), nfPct.format(5n), "hanidec percent");

            const nfSig = new Intl.NumberFormat("en-US-u-nu-hanidec", { minimumSignificantDigits: 3, maximumSignificantDigits: 4 });
            assert.areEqual(nfSig.format(19999), nfSig.format(19999n), "hanidec sig digits rounding");
        }
    },
    {
        name: "BigInt NumberFormat surrogate-pair (mathbold) digits and formatToParts parity",
        body: function () {
            const nf = new Intl.NumberFormat("en-US-u-nu-mathbold");
            assert.areEqual(nf.format(0), nf.format(0n), "mathbold 0");
            assert.areEqual(nf.format(1203), nf.format(1203n), "mathbold 1203");
            assert.areEqual(nf.format(-987654), nf.format(-987654n), "mathbold negative");
            assert.areEqual(
                nf.formatToParts(1203),
                nf.formatToParts(1203n),
                "mathbold formatToParts parity"
            );

            const nfFrac = new Intl.NumberFormat("en-US-u-nu-mathbold", { minimumFractionDigits: 2 });
            assert.areEqual(nfFrac.format(7), nfFrac.format(7n), "mathbold minFrac");
            assert.areEqual(
                nfFrac.formatToParts(7),
                nfFrac.formatToParts(7n),
                "mathbold minFrac formatToParts parity"
            );
        }
    },
    {
        name: "IntlModernShared brand checks across legacy Intl objects and cross-realm Intl.Locale",
        body: function () {
            const nf = new Intl.NumberFormat("en-US");
            const coll = new Intl.Collator("en-US");
            const dtf = new Intl.DateTimeFormat("en-US");
            assert.throws(() => Intl.Locale.prototype.maximize.call(nf), TypeError, "Locale.maximize rejects legacy NumberFormat");
            assert.throws(() => Intl.Locale.prototype.maximize.call(coll), TypeError, "Locale.maximize rejects legacy Collator");
            assert.throws(() => Intl.Locale.prototype.minimize.call(dtf), TypeError, "Locale.minimize rejects legacy DateTimeFormat");
            assert.throws(() => Intl.DurationFormat.prototype.format.call(nf, { seconds: 1 }), TypeError, "DurationFormat rejects legacy NumberFormat");
            if (typeof JSON.rawJSON === "function") {
                assert.throws(() => Intl.Locale.prototype.maximize.call(JSON.rawJSON("1")), TypeError, "Locale rejects JSON.rawJSON");
            }

            const other = WScript.LoadScript("var loc = new Intl.Locale('en', { region: 'US' });", "samethread");
            assert.areEqual("en-US", other.loc.toString(), "cross-realm Locale property");
            assert.areEqual("en-Latn-US", Intl.Locale.prototype.maximize.call(other.loc).toString(), "cross-realm Locale.maximize");
        }
    },
    {
        name: "Intl.Locale and Intl.DurationFormat long/complex locale resolution and RelativeTimeFormat error paths",
        body: function () {
            const df = new Intl.DurationFormat("en-Latn-US-u-ca-gregory-nu-arab", { style: "digital" });
            assert.areEqual("string", typeof df.format({ hours: 1, minutes: 2, seconds: 3 }));

            const dfDeep = new Intl.DurationFormat("zh-Hans-CN-variant1-variant2-u-nu-latn", { style: "long" });
            assert.areEqual("string", typeof dfDeep.format({ days: 2 }));

            const l1 = new Intl.Locale("en-u-rg-uszzzz").maximize();
            assert.areEqual("string", typeof l1.toString());

            const rtfEs = new Intl.RelativeTimeFormat("es-ES", { style: "short" });
            assert.areEqual("string", typeof rtfEs.format(1000, "day"));
            assert.throws(() => new Intl.RelativeTimeFormat("en-US", { style: "invalid-style" }), RangeError, "RTF rejects invalid style");
        }
    },
    {
        name: "Multi-realm FinalizableICUObject creation and WScript.LoadScript error context restoration",
        body: function () {
            this.__hostTag = "main-context";

            let caughtRuntime = null;
            try {
                WScript.LoadScript("throw new Error('boom');", "samethread");
            } catch (e) {
                caughtRuntime = e;
            }
            assert.isTrue(caughtRuntime !== null && caughtRuntime.name === "Error", "samethread runtime throw caught");
            assert.areEqual("main-context", this.__hostTag, "context restored after samethread throw");

            let caughtSyntax = null;
            try {
                WScript.LoadScript("function bad( {", "samethread");
            } catch (e) {
                caughtSyntax = e;
            }
            assert.isTrue(caughtSyntax !== null && caughtSyntax.name === "SyntaxError", "samethread syntax error caught");
            assert.areEqual("main-context", this.__hostTag, "context restored after samethread syntax error");

            assert.throws(() => WScript.LoadScript("throw new Error('self-boom');", "self"), Error, "self runtime throw caught");
            assert.areEqual("main-context", this.__hostTag, "context restored after self throw");

            for (let i = 0; i < 4; i++) {
                WScript.LoadScript(`
                    new Intl.NumberFormat("en-US-u-nu-hanidec").format(123456n);
                    new Intl.DateTimeFormat("en-US").format(new Date(0));
                    new Intl.Collator("en-US").compare("a", "b");
                    new Intl.PluralRules("en-US").select(2);
                    new Intl.RelativeTimeFormat("pl-PL").format(-2, "day");
                    new Intl.DurationFormat("en-US").format({ seconds: 5 });
                `, "samethread");
            }
            if (typeof CollectGarbage === "function") {
                CollectGarbage();
            }
        }
    }
];

testRunner.runTests(tests, { verbose: !WScript.Arguments.includes("summary") });
