//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

// Regression test for long BCP-47 language tags (> ULOC_FULLNAME_CAPACITY / > 256 chars)
// across legacy ICU-backed Intl APIs and Phase-4 Intl APIs.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

function makeSubtagList(prefix, count) {
    const parts = [];
    for (let i = 0; i < count; i++) {
        parts.push(prefix + String(i).padStart(3, "0"));
    }
    return parts.join("-");
}

function makeLongVariantTag(base, count) {
    return base + "-" + makeSubtagList("var", count);
}

const tag177 = makeLongVariantTag("en", 25); // 2 + 25 * 7 = 177 chars
const tag282 = makeLongVariantTag("en", 40); // 2 + 40 * 7 = 282 chars
const tagOver256Search = makeLongVariantTag("de-DE", 20) + "-u-kn-true-x-" + makeSubtagList("priv", 25);
const tagOver300Ext = makeLongVariantTag("de-DE", 25) + "-u-kn-true-co-phonebk-x-" + makeSubtagList("priv", 25);

assert.areEqual(177, tag177.length, "Expected 177-char tag");
assert.areEqual(282, tag282.length, "Expected 282-char tag");
assert.isTrue(tagOver256Search.length > 256, "Expected >256-char search tag");
assert.isTrue(tagOver300Ext.length > 300, "Expected >300-char extended tag");

const tests = [
    {
        name: "Intl.getCanonicalLocales with >157 and >256 char BCP-47 tags",
        body() {
            const c177 = Intl.getCanonicalLocales(tag177);
            assert.areEqual(1, c177.length);
            assert.isTrue(c177[0].startsWith("en-"));
            assert.areEqual(tag177.length, c177[0].length);

            const c282 = Intl.getCanonicalLocales(tag282);
            assert.areEqual(1, c282.length);
            assert.areEqual(tag282.length, c282[0].length);

            const cSearch = Intl.getCanonicalLocales(tagOver256Search);
            assert.areEqual(1, cSearch.length);
            assert.isTrue(cSearch[0].startsWith("de-DE-"));

            const cExt = Intl.getCanonicalLocales(tagOver300Ext);
            assert.areEqual(1, cExt.length);
            assert.isTrue(cExt[0].startsWith("de-DE-"));

            // Structurally invalid long tags must throw RangeError cleanly.
            assert.throws(() => Intl.getCanonicalLocales(tag177 + "--bad"), RangeError);
            assert.throws(() => Intl.getCanonicalLocales(tag177 + "-u-ca-gregory-u-ca-buddhist"), RangeError);

            // Legacy normalizeLanguageTag handles >157-char invalid tags without buffer overflow.
            assert.throws(() => new Intl.Collator(tag177 + "-tesTER-TESter"), RangeError);
            assert.throws(() => new Intl.Collator(tag177 + "-var000"), RangeError);
            assert.throws(() => new Intl.Collator(tag177 + "-u-ca-gregory-u-ca-buddhist"), RangeError);

            // Tags exceeding ICU's internal variant limit in uloc_toLanguageTag must reject
            // with RangeError (never abort via ICU_ASSERT).
            try {
                const c = new Intl.Collator(tag282);
                assert.areEqual(-1, c.compare("a", "b"));
            } catch (e) {
                assert.isTrue(e instanceof RangeError, "Expected RangeError or success, got " + e);
            }
        }
    },
    {
        name: "Legacy Intl constructors (Collator, NumberFormat, DateTimeFormat, PluralRules)",
        body() {
            for (const tag of [tag177, tagOver256Search, tagOver300Ext]) {
                const coll = new Intl.Collator(tag);
                assert.areEqual(-1, coll.compare("a", "b"));
                assert.areEqual("string", typeof coll.resolvedOptions().locale);
                assert.isTrue(Array.isArray(Intl.Collator.supportedLocalesOf([tag])));

                // Exercises uloc_setKeywordValue("collation", "search", ...) on >157 and >256-char localeID.
                const searchColl = new Intl.Collator(tag, { usage: "search" });
                assert.areEqual("search", searchColl.resolvedOptions().usage);
                assert.areEqual(-1, searchColl.compare("a", "b"));
                assert.areEqual(-1, "a".localeCompare("b", tag, { usage: "search" }));

                const nf = new Intl.NumberFormat(tag);
                assert.areEqual("string", typeof nf.format(1234.5));
                assert.isTrue(Array.isArray(nf.formatToParts(1234.5)));
                assert.areEqual("string", typeof (1234.5).toLocaleString(tag));
                assert.isTrue(Array.isArray(Intl.NumberFormat.supportedLocalesOf([tag])));

                // Exercises both udat_open and EntryIntl_GetPatternForSkeleton (uloc_getBaseName).
                const dtf = new Intl.DateTimeFormat(tag, {
                    timeZone: "UTC",
                    year: "numeric",
                    month: "short",
                    day: "numeric"
                });
                const sampleDate = new Date(0);
                assert.areEqual("string", typeof dtf.format(sampleDate));
                assert.isTrue(Array.isArray(dtf.formatToParts(sampleDate)));
                assert.areEqual("string", typeof sampleDate.toLocaleString(tag, { timeZone: "UTC" }));
                assert.isTrue(Array.isArray(Intl.DateTimeFormat.supportedLocalesOf([tag])));

                const pr = new Intl.PluralRules(tag);
                assert.areEqual("string", typeof pr.select(1));
                assert.areEqual("string", typeof pr.select(2));
                assert.isTrue(Array.isArray(pr.resolvedOptions().pluralCategories));
                assert.isTrue(Array.isArray(Intl.PluralRules.supportedLocalesOf([tag])));
            }
        }
    },
    {
        name: "Phase-4 Intl constructors with >157 and >256 char BCP-47 tags",
        body() {
            for (const tag of [tag177, tag282, tagOver256Search, tagOver300Ext]) {
                const rtf = new Intl.RelativeTimeFormat(tag);
                assert.areEqual("string", typeof rtf.format(-1, "day"));
                assert.isTrue(Array.isArray(rtf.formatToParts(-1, "day")));

                const lf = new Intl.ListFormat(tag);
                assert.areEqual("string", typeof lf.format(["one", "two"]));
                assert.isTrue(Array.isArray(lf.formatToParts(["one", "two"])));

                const loc = new Intl.Locale(tag);
                assert.areEqual("string", typeof loc.toString());
                assert.areEqual("string", typeof loc.maximize().toString());
                assert.areEqual("string", typeof loc.minimize().toString());

                const dn = new Intl.DisplayNames(tag, { type: "region" });
                assert.areEqual("string", typeof dn.of("US"));

                const seg = new Intl.Segmenter(tag);
                const segs = Array.from(seg.segment("Hello world"));
                assert.isTrue(segs.length > 0);
            }

            if (typeof CollectGarbage === "function") {
                CollectGarbage();
            }
        }
    }
];

testRunner.runTests(tests, { verbose: !WScript.Arguments.includes("summary") });
