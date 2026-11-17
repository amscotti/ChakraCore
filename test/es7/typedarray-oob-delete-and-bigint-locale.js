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

function hasIcuIntlLocaleRouting() {
    return typeof Intl !== "undefined" &&
        WScript.Platform.INTL_LIBRARY !== "winglob" &&
        (123456).toLocaleString("de-DE") === "123.456";
}

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
            const numberDefault = (123456789).toLocaleString();
            const bigintDefault = (123456789n).toLocaleString();
            assert.areEqual(numberDefault, bigintDefault, "BigInt matches Number in the default locale");
            assert.areEqual((-123456789n).toLocaleString(), (-123456789).toLocaleString(), "negative matches Number in the default locale");

            // Grouping is locale-dependent; pin en-US for the separator/length checks.
            const numberGrouped = (123456789).toLocaleString("en-US");
            const bigintGrouped = (123456789n).toLocaleString("en-US");
            assert.areEqual(numberGrouped, bigintGrouped, "BigInt groups exactly like Number in en-US");
            const sep = numberGrouped.length > 3 ? numberGrouped[numberGrouped.length - 4] : "";
            assert.areEqual((1n).toLocaleString("en-US"), "1", "no separator for short values");
            // > bufSize (1280 chars) exercises the heap-buffer grouping path
            const big = 10n ** 1300n - 1n; // 1300 nines
            const grouped = big.toLocaleString("en-US");
            assert.areEqual(1300 + Math.floor(1300 / 3) - (((1300 % 3) === 0) ? 1 : 0), grouped.length, "grouped length accounts for separators");
            assert.areEqual(big.toString(), grouped.split(sep).join(""), "digits preserved through grouping");
        }
    },
    {
        name: "BigInt toLocaleString receiver validation unchanged",
        body: function () {
            if (typeof BigInt === 'undefined') { return; }
            assert.throws(function () { BigInt.prototype.toLocaleString.call(1); }, TypeError, "number receiver rejected");
            assert.throws(function () { BigInt.prototype.toLocaleString.call(); }, TypeError, "missing receiver rejected");
            assert.throws(function () { BigInt.prototype.toLocaleString.call(undefined); }, TypeError, "undefined receiver rejected");
            assert.throws(function () { BigInt.prototype.toLocaleString.call(null); }, TypeError, "null receiver rejected");
            assert.throws(function () { BigInt.prototype.toLocaleString.call({}); }, TypeError, "object receiver rejected");
        }
    },
    {
        name: "BigInt.prototype.toLocaleString exact >2^53 formatting across locales and options",
        body: function () {
            if (typeof BigInt === 'undefined' || !hasIcuIntlLocaleRouting()) {
                return;
            }

            const aboveSafe = 9007199254740993n; // 2^53 + 1
            const hugeNeg = -123456789012345678901234567890n;
            assert.areEqual("9,007,199,254,740,993", aboveSafe.toLocaleString("en-US"), "exact >2^53 en-US");
            assert.areEqual("9.007.199.254.740.993", aboveSafe.toLocaleString("de-DE"), "exact >2^53 de-DE");
            assert.areEqual("-123,456,789,012,345,678,901,234,567,890", hugeNeg.toLocaleString("en-US"), "exact huge negative en-US");
            assert.areEqual("-123.456.789.012.345.678.901.234.567.890", hugeNeg.toLocaleString("de-DE"), "exact huge negative de-DE");

            assert.areEqual("123456", (123456n).toLocaleString("en-US", { useGrouping: false }), "useGrouping: false");
            assert.areEqual("00,042", (42n).toLocaleString("en-US", { minimumIntegerDigits: 5 }), "minimumIntegerDigits: 5");
            assert.areEqual("123,000,000", (123456789n).toLocaleString("en-US", { maximumSignificantDigits: 3 }), "maximumSignificantDigits: 3");

            assert.throws(function () { (123n).toLocaleString("invalid_locale_tag"); }, RangeError, "invalid locale throws RangeError");
            assert.throws(function () { (123n).toLocaleString("en-US", { style: "invalid" }); }, RangeError, "invalid style option throws RangeError");

            // Ensure BigInt Intl initialization did not clobber Number.prototype.toLocaleString.
            assert.areEqual("123.456", (123456).toLocaleString("de-DE"), "Number.prototype.toLocaleString remains intact");
        }
    },
    {
        name: "BigInt.prototype.toLocaleString boxed receivers and delete globalThis.BigInt resilience",
        body: function () {
            if (typeof BigInt === 'undefined' || !hasIcuIntlLocaleRouting()) {
                return;
            }

            const boxed = Object(123456n);
            assert.areEqual("123,456", boxed.toLocaleString("en-US"), "boxed BigInt en-US");
            assert.areEqual("123.456", boxed.toLocaleString("de-DE"), "boxed BigInt de-DE");
            assert.areEqual("9.007.199.254.740.993", Object(9007199254740993n).toLocaleString("de-DE"), "boxed >2^53 BigInt de-DE");

            const savedBigInt = globalThis.BigInt;
            const toLocale = BigInt.prototype.toLocaleString;
            try {
                delete globalThis.BigInt;
                assert.areEqual("123.456", (123456n).toLocaleString("de-DE"), "primitive survives deleted globalThis.BigInt");
                assert.areEqual("123.456", boxed.toLocaleString("de-DE"), "boxed BigInt survives deleted globalThis.BigInt");
                assert.throws(function () { toLocale.call(1); }, TypeError, "invalid receiver throws TypeError without globalThis.BigInt");
                assert.throws(function () { toLocale.call(); }, TypeError, "missing receiver throws TypeError without globalThis.BigInt");
            } finally {
                globalThis.BigInt = savedBigInt;
            }
        }
    },
    {
        name: "BigInt64Array and BigUint64Array toLocaleString formatting",
        body: function () {
            if (typeof BigInt === 'undefined' || typeof BigInt64Array === 'undefined' || typeof BigUint64Array === 'undefined') {
                return;
            }

            const i64 = new BigInt64Array([123456n, -789012n]);
            const u64 = new BigUint64Array([123456n, 18446744073709551615n]);

            assert.areEqual((123456n).toLocaleString() + "," + (-789012n).toLocaleString(), i64.toLocaleString(),
                "BigInt64Array.prototype.toLocaleString formats each element");
            assert.areEqual((123456n).toLocaleString() + "," + (18446744073709551615n).toLocaleString(), u64.toLocaleString(),
                "BigUint64Array.prototype.toLocaleString formats each element");

            if (hasIcuIntlLocaleRouting()) {
                assert.areEqual("123.456", BigInt.prototype.toLocaleString.call(i64[0], "de-DE"), "BigInt64Array element de-DE");
                assert.areEqual("-789.012", BigInt.prototype.toLocaleString.call(i64[1], "de-DE"), "BigInt64Array negative element de-DE");
                assert.areEqual("18.446.744.073.709.551.615", BigInt.prototype.toLocaleString.call(u64[1], "de-DE"), "BigUint64Array max element de-DE");
            }
        }
    },
    {
        name: "Warmed JIT ImplicitCall bailout on BigInt.prototype.toLocaleString",
        body: function () {
            if (typeof BigInt === 'undefined' || !hasIcuIntlLocaleRouting()) {
                return;
            }

            const state = { marker: 1, iter: 0 };
            const opts = {
                get style() {
                    if (state.iter === 190) {
                        state.marker = 999;
                    }
                    return "decimal";
                }
            };

            function callLocaleWithRead(x, s, o) {
                const before = s.marker;
                const formatted = x.toLocaleString("en-US", o);
                const after = s.marker;
                return { before: before, after: after, formatted: formatted };
            }

            for (let i = 0; i < 400; i++) {
                state.iter = i;
                const expectedBefore = i <= 190 ? 1 : 999;
                const expectedAfter = i < 190 ? 1 : 999;
                const res = callLocaleWithRead(123456n, state, opts);
                assert.areEqual("123,456", res.formatted, "warmed toLocaleString output at iter " + i);
                assert.areEqual(expectedBefore, res.before, "pre-call property read at iter " + i);
                assert.areEqual(expectedAfter, res.after, "post-call property read sees getter mutation at iter " + i);
            }

            // Exercise BigInt64Array.prototype.toLocaleString (JavascriptConversion::ToLocaleString
            // ExecuteImplicitCall path) inside a warmed helper and across a state mutation.
            const ta = new BigInt64Array([123456n, -789012n]);
            const expectedTa = (123456n).toLocaleString() + "," + (-789012n).toLocaleString();
            const savedBigIntToLocale = BigInt.prototype.toLocaleString;
            const taState = { marker: 10 };

            function callTypedArrayToLocale(arr, s) {
                const before = s.marker;
                const formatted = arr.toLocaleString();
                const after = s.marker;
                return { before: before, after: after, formatted: formatted };
            }

            try {
                for (let i = 0; i < 400; i++) {
                    if (i === 190) {
                        BigInt.prototype.toLocaleString = function () {
                            taState.marker = 777;
                            return savedBigIntToLocale.call(this, "en-US", opts);
                        };
                    } else if (i === 191) {
                        BigInt.prototype.toLocaleString = savedBigIntToLocale;
                    }
                    const res = callTypedArrayToLocale(ta, taState);
                    if (i === 190) {
                        assert.areEqual("123,456,-789,012", res.formatted, "BigInt64Array element implicit call at iter 190");
                        assert.areEqual(10, res.before, "pre-call read before iter 190 mutation");
                        assert.areEqual(777, res.after, "post-call read observes iter 190 mutation");
                    } else {
                        assert.areEqual(expectedTa, res.formatted, "BigInt64Array warmed output at iter " + i);
                        const expectedVal = i < 190 ? 10 : 777;
                        assert.areEqual(expectedVal, res.before, "BigInt64Array pre-call read at iter " + i);
                        assert.areEqual(expectedVal, res.after, "BigInt64Array post-call read at iter " + i);
                    }
                }
            } finally {
                BigInt.prototype.toLocaleString = savedBigIntToLocale;
            }

            // Exercise ToPrimitive ExecuteImplicitCall(BigInt.prototype.toLocaleString) directly
            // with Object(123456n).toString = BigInt.prototype.toLocaleString in a warmed helper.
            const primHolder = { marker: 5, trigger: false };
            const boxedPrim = Object(123456n);
            boxedPrim.valueOf = undefined;
            boxedPrim.toString = function () {
                if (primHolder.trigger) {
                    primHolder.marker = 888;
                }
                return savedBigIntToLocale.call(this, "en-US", opts);
            };
            function callToPrimitiveRead(b, s) {
                const before = s.marker;
                const out = "" + b;
                const after = s.marker;
                return { before: before, after: after, out: out };
            }
            for (let i = 0; i < 400; i++) {
                primHolder.trigger = (i === 190);
                const expectedBefore = i <= 190 ? 5 : 888;
                const expectedAfter = i < 190 ? 5 : 888;
                const res = callToPrimitiveRead(boxedPrim, primHolder);
                assert.areEqual("123,456", res.out, "ToPrimitive formatted at iter " + i);
                assert.areEqual(expectedBefore, res.before, "ToPrimitive pre-call read at iter " + i);
                assert.areEqual(expectedAfter, res.after, "ToPrimitive post-call read at iter " + i);
            }

            // Direct C++ BuiltIn EntryToLocaleString inside ExecuteImplicitCall (no JS wrapper):
            // exercises both ToPrimitive ("" + boxedDirect) and CallGetter (boxedDirect.fmt)
            // across warmed JIT iterations, plus TypeError propagation on a non-BigInt receiver.
            const boxedDirect = Object(123456n);
            boxedDirect.valueOf = undefined;
            boxedDirect.toString = savedBigIntToLocale;
            Object.defineProperty(boxedDirect, "fmt", { get: savedBigIntToLocale });
            const defaultFormatted = (123456n).toLocaleString();
            function callDirectImplicit(b) {
                return ("" + b) + "|" + b.fmt;
            }
            for (let i = 0; i < 400; i++) {
                assert.areEqual(defaultFormatted + "|" + defaultFormatted, callDirectImplicit(boxedDirect),
                    "direct EntryToLocaleString ExecuteImplicitCall output at iter " + i);
            }
            const badDirect = { valueOf: undefined, toString: savedBigIntToLocale };
            for (let i = 0; i < 200; i++) {
                assert.throws(function () { return "" + badDirect; }, TypeError,
                    "direct EntryToLocaleString ExecuteImplicitCall rejects non-BigInt at iter " + i);
            }
        }
    },
    {
        name: "Array and TypedArray toLocaleString(locales, options) forwarding and warmed JIT bailout",
        body: function () {
            const optsBoth = { minimumIntegerDigits: 4, maximumSignificantDigits: 3 };
            const optsMin4 = { minimumIntegerDigits: 4 };

            assert.areEqual("1.230", [1234.5].toLocaleString("de-DE", optsBoth), "Array de-DE both options");
            assert.areEqual("1.234,5,0.042", [1234.5, 42].toLocaleString("de-DE", optsMin4), "Array de-DE minimumIntegerDigits");
            assert.areEqual("1.234,0.042", new Int32Array([1234, 42]).toLocaleString("de-DE", optsMin4), "Int32Array de-DE minimumIntegerDigits");
            assert.areEqual("1.230", new Int32Array([1234]).toLocaleString("de-DE", optsBoth), "Int32Array de-DE both options");
            assert.areEqual("123.456,-0.007", new BigInt64Array([123456n, -7n]).toLocaleString("de-DE", optsMin4), "BigInt64Array de-DE minimumIntegerDigits");
            assert.areEqual("123.000", new BigInt64Array([123456n]).toLocaleString("de-DE", optsBoth), "BigInt64Array de-DE both options");
            assert.areEqual("", [].toLocaleString("invalid-locale"), "empty Array skips invalid locale check");
            assert.areEqual("", new Int32Array(0).toLocaleString("invalid-locale"), "empty TypedArray skips invalid locale check");

            const observed = [];
            const customArr = [
                1234.5,
                {
                    toLocaleString(loc, opt) {
                        observed.push([arguments.length, loc, opt]);
                        return loc + ":" + opt.minimumIntegerDigits + "/" + opt.maximumSignificantDigits;
                    }
                }
            ];
            assert.areEqual("1.230,de-DE:4/3", customArr.toLocaleString("de-DE", optsBoth), "Array forwards locales and options to element");
            assert.areEqual(2, observed[0][0], "Array element receives 2 arguments");
            assert.areEqual("de-DE", observed[0][1], "Array element receives locales");
            assert.areEqual(optsBoth, observed[0][2], "Array element receives options");

            const argCountDefault = [{ toLocaleString() { return String(arguments.length); } }].toLocaleString();
            assert.areEqual("2", argCountDefault, "ECMA-402 passes (undefined, undefined) when omitted");

            // Warmed JIT bailout check on Array.prototype.toLocaleString & Int32Array.prototype.toLocaleString
            const holder = { marker: 11, trigger: false };
            const jitArr = [
                {
                    toLocaleString(loc, opt) {
                        if (holder.trigger) {
                            holder.marker = 444;
                        }
                        return (12345).toLocaleString(loc, opt);
                    }
                },
                42
            ];
            const taSource = new Int32Array([12345, 42]);
            const biSource = new BigInt64Array([123456n, -7n]);

            function runLocaleWarm(arr, ta, bi, s) {
                const before = s.marker;
                const aOut = arr.toLocaleString("de-DE", optsMin4);
                const tOut = ta.toLocaleString("de-DE", optsMin4);
                const bOut = bi.toLocaleString("de-DE", optsMin4);
                const after = s.marker;
                return { before: before, after: after, aOut: aOut, tOut: tOut, bOut: bOut };
            }

            for (let i = 0; i < 400; i++) {
                holder.trigger = (i === 190);
                const res = runLocaleWarm(jitArr, taSource, biSource, holder);
                const expBefore = i <= 190 ? 11 : 444;
                const expAfter = i < 190 ? 11 : 444;
                assert.areEqual("12.345,0.042", res.aOut, "Array warmed toLocaleString output at iter " + i);
                assert.areEqual("12.345,0.042", res.tOut, "Int32Array warmed toLocaleString output at iter " + i);
                assert.areEqual("123.456,-0.007", res.bOut, "BigInt64Array warmed toLocaleString output at iter " + i);
                assert.areEqual(expBefore, res.before, "pre-call marker at iter " + i);
                assert.areEqual(expAfter, res.after, "post-call marker at iter " + i);
            }
        }
    },
    {
        name: "TypedArray descriptors, Set / Reflect.set, prototype bypass, and integrity checks",
        body: function () {
            const ta = new Int32Array([10, 20]);
            const desc = Object.getOwnPropertyDescriptor(ta, 0);
            assert.isTrue(desc.configurable, "valid index descriptor is configurable");
            assert.isTrue(desc.writable, "valid index descriptor is writable");
            assert.isTrue(desc.enumerable, "valid index descriptor is enumerable");
            assert.areEqual(10, desc.value, "valid index descriptor value");
            assert.isFalse(Reflect.defineProperty(ta, 0, { configurable: false }), "cannot make index non-configurable");

            const taNonExt = new Int32Array([1]);
            Object.preventExtensions(taNonExt);
            assert.isFalse(Object.isExtensible(taNonExt), "preventExtensions succeeds");
            assert.isFalse(Object.isSealed(taNonExt), "non-empty TypedArray is not sealed");
            assert.isFalse(Object.isFrozen(taNonExt), "non-empty TypedArray is not frozen");

            const taEmpty = new Int32Array(0);
            Object.preventExtensions(taEmpty);
            assert.isTrue(Object.isSealed(taEmpty), "empty non-extensible TypedArray is sealed");
            assert.isTrue(Object.isFrozen(taEmpty), "empty non-extensible TypedArray is frozen");

            const immTa = new Int32Array(new ArrayBuffer(8).transferToImmutable());
            assert.isFalse(Object.getOwnPropertyDescriptor(immTa, 0).configurable, "immutable view index remains non-configurable");

            let seCalls = 0;
            const sideEffect = { valueOf() { seCalls++; return 99; } };

            assert.isTrue(Reflect.set(ta, 5, sideEffect), "Reflect.set OOB same receiver returns true");
            assert.areEqual(1, seCalls, "Reflect.set OOB evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, "-0", sideEffect), "Reflect.set '-0' returns true");
            assert.areEqual(2, seCalls, "Reflect.set '-0' evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, "1.1", sideEffect), "Reflect.set '1.1' returns true");
            assert.areEqual(3, seCalls, "Reflect.set '1.1' evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, "NaN", sideEffect), "Reflect.set 'NaN' returns true");
            assert.areEqual(4, seCalls, "Reflect.set 'NaN' evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, "Infinity", sideEffect), "Reflect.set 'Infinity' returns true");
            assert.areEqual(5, seCalls, "Reflect.set 'Infinity' evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, "-1", sideEffect), "Reflect.set '-1' returns true");
            assert.areEqual(6, seCalls, "Reflect.set '-1' evaluates ToNumber(V)");
            assert.isTrue(Reflect.set(ta, new String("-0"), sideEffect), "Reflect.set new String('-0') returns true");
            assert.areEqual(7, seCalls, "Reflect.set new String('-0') evaluates ToNumber(V)");

            (function () {
                "use strict";
                const before = seCalls;
                ta[5] = sideEffect;
                ta["-0"] = sideEffect;
                ta["1.1"] = sideEffect;
                ta["NaN"] = sideEffect;
                ta[new String("-0")] = sideEffect;
                assert.areEqual(5, seCalls - before, "strict mode canonical numeric writes run side effects without throwing");
            })();

            // BigInt64Array OOB / canonical numeric index value conversion
            const bi = new BigInt64Array([1n, 2n]);
            let biCalls = 0;
            const biSideEffect = { valueOf() { biCalls++; return 7n; } };
            assert.isTrue(Reflect.set(bi, 5, biSideEffect), "BigInt64Array OOB returns true after ToBigInt");
            assert.areEqual(1, biCalls, "BigInt64Array OOB converts value");
            assert.isTrue(Reflect.set(bi, "-0", biSideEffect), "BigInt64Array '-0' returns true after ToBigInt");
            assert.areEqual(2, biCalls, "BigInt64Array '-0' converts value");
            assert.throws(function () { Reflect.set(bi, 5, 1); }, TypeError, "BigInt64Array OOB number throws TypeError");

            // ES2025 10.4.5.5 [[Set]] step 1.b.ii: distinct receiver
            const recvOob = {};
            const beforeDistinct = seCalls;
            assert.isTrue(Reflect.set(ta, 5, sideEffect, recvOob), "distinct receiver OOB returns true");
            assert.isTrue(Reflect.set(ta, "-0", sideEffect, recvOob), "distinct receiver '-0' returns true");
            assert.isTrue(Reflect.set(ta, "1.1", sideEffect, recvOob), "distinct receiver '1.1' returns true");
            assert.isTrue(Reflect.set(bi, 5, 1, recvOob), "distinct receiver OOB BigInt skips ToBigInt");
            assert.areEqual(beforeDistinct, seCalls, "distinct receiver invalid index skips ToNumber(V)");
            assert.isFalse(Object.prototype.hasOwnProperty.call(recvOob, "5"), "distinct receiver OOB does not create property");
            assert.isFalse(Object.prototype.hasOwnProperty.call(recvOob, "-0"), "distinct receiver '-0' does not create property");

            const recvValid = {};
            assert.isTrue(Reflect.set(ta, 0, "hello", recvValid), "distinct receiver valid index defines property on receiver");
            assert.areEqual("hello", recvValid[0], "distinct receiver gets raw value");
            assert.areEqual(10, ta[0], "TypedArray target unmodified when receiver differs");

            const recvAcc = {};
            Object.defineProperty(recvAcc, 0, { set() { throw new Error("setter must not run"); } });
            assert.isFalse(Reflect.set(ta, 0, 123, recvAcc), "distinct receiver with accessor returns false");
            assert.isFalse(Reflect.set(ta, 0, 123, 42), "distinct primitive receiver returns false");

            // Prototype indexed setters must never be triggered by TypedArray [[Set]]
            let protoSetCalls = 0;
            try {
                Object.defineProperty(Int32Array.prototype, 0, { set() { protoSetCalls++; }, configurable: true });
                Object.defineProperty(Int32Array.prototype, 5, { set() { protoSetCalls++; }, configurable: true });
                ta[0] = 55;
                ta[5] = 66;
                assert.isTrue(Reflect.set(ta, 0, 56));
                assert.isTrue(Reflect.set(ta, 5, 67));
                assert.areEqual(0, protoSetCalls, "TypedArray [[Set]] bypasses prototype setters");
                assert.areEqual(56, ta[0], "in-bounds write updated element directly");
            } finally {
                delete Int32Array.prototype[0];
                delete Int32Array.prototype[5];
            }
        }
    },
    {
        name: "Array.prototype.unshift prototype setter and Array.prototype.join length capture",
        body: function () {
            let unshiftSetterLog = "";
            try {
                Object.defineProperty(Array.prototype, 0, {
                    set(v) { unshiftSetterLog += "set0=" + v + ";"; },
                    configurable: true
                });
                const a = [];
                assert.areEqual(1, a.unshift(42), "unshift returns new length");
                assert.areEqual("set0=42;", unshiftSetterLog, "unshift triggers Array.prototype[0] setter");
                assert.isFalse(Object.prototype.hasOwnProperty.call(a, 0), "setter intercepted property creation");
            } finally {
                delete Array.prototype[0];
            }

            try {
                Object.defineProperty(Array.prototype, 0, {
                    get() { return 99; },
                    configurable: true
                });
                assert.throws(function () { [].unshift(42); }, TypeError, "unshift throws on getter-only prototype[0]");
            } finally {
                delete Array.prototype[0];
            }

            try {
                Object.defineProperty(Array.prototype, 0, {
                    value: 99,
                    writable: false,
                    configurable: true
                });
                assert.throws(function () { [].unshift(42); }, TypeError, "unshift throws on non-writable prototype[0]");
            } finally {
                delete Array.prototype[0];
            }

            const j0 = [1, 2, 3];
            assert.areEqual("..", j0.join({ toString() { j0.length = 0; return "."; } }), "join captures length before ToString(separator) when shrunk to 0");
            const j1 = [1, 2, 3];
            assert.areEqual("1..", j1.join({ toString() { j1.length = 1; return "."; } }), "join captures length before ToString(separator) when shrunk to 1");
            const j2 = [];
            assert.areEqual("", j2.join({ toString() { j2.push(8, 9); return "."; } }), "join captures length before ToString(separator) when grown");
        }
    }
];

testRunner.run(tests, { verbose: WScript.Arguments[0] != "summary" });
