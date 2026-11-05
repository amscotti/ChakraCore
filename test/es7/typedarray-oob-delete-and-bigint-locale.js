//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// C sweep (card reconciliation): the two bounded engine fixes with observable
// behavior. (1) %TypedArray% [[Delete]] returns true for out-of-bounds
// canonical indexes (ES2025 IsValidIntegerIndex: attached-OOB and
// shrunk-resizable views; in-range indexes keep the cant-delete behavior).
// (2) BigInt.prototype.toLocaleString groups digits in the default locale,
// mirroring Number.prototype.toLocaleString's non-Intl path. See
// doc/open-cards.md.

WScript.LoadScriptFile("..\\UnitTestFramework\\UnitTestFramework.js");

var tests = [
    {
        name: "Attached out-of-bounds canonical index delete returns true",
        body: function () {
            const ta = new Uint8Array([1, 2, 3]);
            assert.areEqual(true, delete ta[3], "delete ta[3] (OOB) is true");
            assert.areEqual(true, delete ta[100], "delete ta[100] (OOB) is true");
            assert.areEqual(true, (function () { 'use strict'; return delete ta[4]; })(), "strict delete ta[4] (OOB) is true");
            assert.areEqual(3, ta.length, "length unchanged");
        }
    },
    {
        name: "In-range index delete keeps cant-delete behavior",
        body: function () {
            const ta = new Uint8Array([7]);
            assert.areEqual(false, delete ta[0], "delete ta[0] (in-range) is false");
            assert.areEqual(7, ta[0], "value survives");
            assert.throws(function () { 'use strict'; delete ta[0]; }, TypeError, "strict in-range delete throws");
        }
    },
    {
        name: "Shrunk resizable-buffer view: former indexes delete as true",
        body: function () {
            const rab = new ArrayBuffer(8, { maxByteLength: 8 });
            const ta = new Uint8Array(rab);
            ta[0] = 1; ta[7] = 1;
            rab.resize(2);
            assert.areEqual(2, ta.length, "length tracks the resize");
            assert.areEqual(true, delete ta[2], "delete of shrunk-OOB index is true");
            assert.areEqual(true, (function () { 'use strict'; return delete ta[7]; })(), "strict delete of shrunk-OOB index is true");
            assert.areEqual(false, delete ta[1], "still-in-range index keeps cant-delete");
        }
    },
    {
        name: "Zero-length view deletes are true",
        body: function () {
            const ta = new Uint8Array(0);
            assert.areEqual(true, delete ta[0], "delete on empty view is true");
        }
    },
    {
        name: "BigInt.prototype.toLocaleString groups digits in the default locale",
        body: function () {
            if (typeof BigInt === 'undefined') { return; }
            const numberGrouped = (123456789).toLocaleString();
            const bigintGrouped = (123456789n).toLocaleString();
            assert.areEqual(numberGrouped, bigintGrouped, "BigInt groups exactly like Number in the default locale");
            assert.areEqual((-123456789n).toLocaleString(), (-123456789).toLocaleString(), "negative grouped like Number");
            const sep = numberGrouped.length > 3 ? numberGrouped[numberGrouped.length - 4] : "";
            assert.areEqual((1n).toLocaleString(), "1", "no separator for short values");
            // > bufSize (1280 chars) exercises the heap-buffer grouping path
            const big = 10n ** 1300n - 1n; // 1300 nines
            const grouped = big.toLocaleString();
            assert.areEqual(1300 + Math.floor(1300 / 3) - (((1300 % 3) === 0) ? 1 : 0), grouped.length, "grouped length accounts for separators");
            assert.areEqual(big.toString(), grouped.split(sep).join(""), "digits preserved through grouping");
        }
    },
    {
        name: "BigInt toLocaleString receiver validation unchanged",
        body: function () {
            if (typeof BigInt === 'undefined') { return; }
            assert.throws(function () { BigInt.prototype.toLocaleString.call(1); }, TypeError, "number receiver rejected");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
