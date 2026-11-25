//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlEnumeration.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3):
//   Resource:  ICU UEnumeration*, UNumberingSystem*, ULocaleDisplayNames* handles
//              created inside IntlEnumeration::EntrySupportedValuesOf, plus local AsciiStringSet
//              storage (char pool and size_t offset table).
//   Domain:    ICU library heap for ICU handles; stack scratch (2 KiB pool / 128 offsets) with
//              NoCheckHeapNewArray/NoCheckHeapDeleteArray fallback for AsciiStringSet; Recycler GC
//              heap solely for the returned JavascriptArray* and its element JavascriptString*s.
//   Owner:     Stack RAII only (PlatformAgnostic::ICUHelpers::ScopedICUObject and AsciiStringSet);
//              no native handle, native buffer, or GC reference is retained across calls.
//   Reentrancy: JavascriptConversion::ToString(keyArg, scriptContext) executes before any native
//              allocation or ICU call. Enumeration runs in a leaf non-reentrant window and writes
//              own elements via CreateArray + DirectSetItemAt (immune to Array.prototype setters).
//   Failure:   ICU U_MEMORY_ALLOCATION_ERROR and AsciiStringSet OOM set/throw JS OutOfMemoryError;
//              all RAII locals unwind cleanly. Invalid keys throw RangeError.
//   Thread:    Owning ScriptContext thread only.

namespace Js
{
    namespace
    {
        enum class EnumerationKey : uint8
        {
            Calendar,
            Collation,
            Currency,
            NumberingSystem,
            TimeZone,
            Unit
        };

        // Sanctioned simple unit identifiers from ECMA-402 Table (sorted ascending UTF-16 order).
        const char* const kSanctionedUnits[] =
        {
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
        };

        void CheckIcuOom(UErrorCode status, ScriptContext* scriptContext)
        {
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
        }

        int UnsignedByteStrCmp(const char* a, const char* b)
        {
            while (*a != '\0' && *a == *b)
            {
                ++a;
                ++b;
            }
            return static_cast<int>(static_cast<unsigned char>(*a)) -
                   static_cast<int>(static_cast<unsigned char>(*b));
        }

        bool EqualsUtf16Literal(const char16* str, charcount_t len, const char16* literal, size_t literalLen)
        {
            return static_cast<size_t>(len) == literalLen && wmemcmp(str, literal, literalLen) == 0;
        }

        bool TryClassifyKey(JavascriptString* keyStr, EnumerationKey* outKey)
        {
            const char16* keyBuf = keyStr->GetString();
            charcount_t keyLen = keyStr->GetLength();

            if (EqualsUtf16Literal(keyBuf, keyLen, _u("calendar"), 8))
            {
                *outKey = EnumerationKey::Calendar;
                return true;
            }
            if (EqualsUtf16Literal(keyBuf, keyLen, _u("collation"), 9))
            {
                *outKey = EnumerationKey::Collation;
                return true;
            }
            if (EqualsUtf16Literal(keyBuf, keyLen, _u("currency"), 8))
            {
                *outKey = EnumerationKey::Currency;
                return true;
            }
            if (EqualsUtf16Literal(keyBuf, keyLen, _u("numberingSystem"), 15))
            {
                *outKey = EnumerationKey::NumberingSystem;
                return true;
            }
            if (EqualsUtf16Literal(keyBuf, keyLen, _u("timeZone"), 8))
            {
                *outKey = EnumerationKey::TimeZone;
                return true;
            }
            if (EqualsUtf16Literal(keyBuf, keyLen, _u("unit"), 4))
            {
                *outKey = EnumerationKey::Unit;
                return true;
            }
            return false;
        }

        // Stack-backed, NoCheckHeap-fallback set of ASCII strings with O(n log n) sort & dedup.
        class AsciiStringSet
        {
        private:
            char stackPool[2048];
            char* pool;
            size_t poolLen;
            size_t poolCap;

            size_t stackOffsets[128];
            size_t* offsets;
            size_t count;
            size_t offsetsCap;

            bool oom;

            bool EnsurePoolAdditional(size_t addBytes)
            {
                if (oom)
                {
                    return false;
                }
                if (addBytes > static_cast<size_t>(INT32_MAX) / 2 ||
                    poolLen > static_cast<size_t>(INT32_MAX) - addBytes)
                {
                    oom = true;
                    return false;
                }
                size_t needed = poolLen + addBytes;
                if (needed <= poolCap)
                {
                    return true;
                }
                size_t newCap = poolCap;
                while (newCap < needed)
                {
                    if (newCap > static_cast<size_t>(INT32_MAX) / 2)
                    {
                        oom = true;
                        return false;
                    }
                    newCap *= 2;
                }
                char* newPool = NoCheckHeapNewArray(char, newCap);
                if (newPool == nullptr)
                {
                    oom = true;
                    return false;
                }
                if (poolLen > 0)
                {
                    js_memcpy_s(newPool, newCap, pool, poolLen);
                }
                if (pool != stackPool)
                {
                    NoCheckHeapDeleteArray(poolCap, pool);
                }
                pool = newPool;
                poolCap = newCap;
                return true;
            }

            bool EnsureOffsetSlot()
            {
                if (oom)
                {
                    return false;
                }
                if (count < offsetsCap)
                {
                    return true;
                }
                if (offsetsCap > static_cast<size_t>(INT32_MAX) / 2)
                {
                    oom = true;
                    return false;
                }
                size_t newCap = offsetsCap * 2;
                size_t* newOffsets = NoCheckHeapNewArray(size_t, newCap);
                if (newOffsets == nullptr)
                {
                    oom = true;
                    return false;
                }
                js_memcpy_s(newOffsets, newCap * sizeof(size_t), offsets, count * sizeof(size_t));
                if (offsets != stackOffsets)
                {
                    NoCheckHeapDeleteArray(offsetsCap, offsets);
                }
                offsets = newOffsets;
                offsetsCap = newCap;
                return true;
            }

            static void SiftDown(size_t* arr, size_t start, size_t end, const char* base)
            {
                size_t root = start;
                while (root * 2 + 1 <= end)
                {
                    size_t child = root * 2 + 1;
                    size_t swapIdx = root;
                    if (UnsignedByteStrCmp(base + arr[swapIdx], base + arr[child]) < 0)
                    {
                        swapIdx = child;
                    }
                    if (child + 1 <= end && UnsignedByteStrCmp(base + arr[swapIdx], base + arr[child + 1]) < 0)
                    {
                        swapIdx = child + 1;
                    }
                    if (swapIdx == root)
                    {
                        return;
                    }
                    size_t tmp = arr[root];
                    arr[root] = arr[swapIdx];
                    arr[swapIdx] = tmp;
                    root = swapIdx;
                }
            }

        public:
            AsciiStringSet() :
                pool(stackPool),
                poolLen(0),
                poolCap(sizeof(stackPool)),
                offsets(stackOffsets),
                count(0),
                offsetsCap(_countof(stackOffsets)),
                oom(false)
            {
            }

            ~AsciiStringSet()
            {
                if (pool != stackPool)
                {
                    NoCheckHeapDeleteArray(poolCap, pool);
                }
                if (offsets != stackOffsets)
                {
                    NoCheckHeapDeleteArray(offsetsCap, offsets);
                }
            }

            AsciiStringSet(const AsciiStringSet&) = delete;
            AsciiStringSet& operator=(const AsciiStringSet&) = delete;

            bool Add(const char* s, size_t len)
            {
                if (s == nullptr || len == 0 || oom)
                {
                    return false;
                }
                for (size_t i = 0; i < len; ++i)
                {
                    unsigned char ch = static_cast<unsigned char>(s[i]);
                    if (ch == 0 || ch > 0x7F)
                    {
                        return false;
                    }
                }
                if (!EnsureOffsetSlot() || !EnsurePoolAdditional(len + 1))
                {
                    return false;
                }
                size_t off = poolLen;
                js_memcpy_s(pool + off, poolCap - off, s, len);
                pool[off + len] = '\0';
                poolLen = off + len + 1;
                offsets[count++] = off;
                return true;
            }

            void FinishSortAndDeduplicate()
            {
                if (oom || count <= 1)
                {
                    return;
                }
                const char* base = pool;
                size_t start = (count - 2) / 2;
                for (;;)
                {
                    SiftDown(offsets, start, count - 1, base);
                    if (start == 0)
                    {
                        break;
                    }
                    --start;
                }
                size_t end = count - 1;
                while (end > 0)
                {
                    size_t tmp = offsets[end];
                    offsets[end] = offsets[0];
                    offsets[0] = tmp;
                    --end;
                    SiftDown(offsets, 0, end, base);
                }
                size_t writeIdx = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    if (writeIdx == 0 || UnsignedByteStrCmp(base + offsets[writeIdx - 1], base + offsets[i]) != 0)
                    {
                        offsets[writeIdx++] = offsets[i];
                    }
                }
                count = writeIdx;
            }

            JavascriptArray* ToJsArray(ScriptContext* scriptContext) const
            {
                if (oom || count > static_cast<size_t>(UINT32_MAX))
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                JavascriptArray* result = scriptContext->GetLibrary()->CreateArray(static_cast<uint32>(count));
                for (size_t i = 0; i < count; ++i)
                {
                    const char* s = pool + offsets[i];
                    JavascriptString* item = IntlModern::CreateAsciiString(s, strlen(s), scriptContext);
                    result->DirectSetItemAt(static_cast<uint32>(i), item);
                }
                return result;
            }
        };

        bool CopyAscii(const char* src, int32_t srcLen, char* dst, size_t dstCap, bool toLower)
        {
            if (src == nullptr || srcLen <= 0 || static_cast<size_t>(srcLen) >= dstCap)
            {
                return false;
            }
            for (int32_t i = 0; i < srcLen; ++i)
            {
                unsigned char ch = static_cast<unsigned char>(src[i]);
                if (ch == 0 || ch > 0x7F)
                {
                    return false;
                }
                char c = static_cast<char>(ch);
                dst[i] = toLower ? IntlModern::ToAsciiLower(c) : c;
            }
            dst[srcLen] = '\0';
            return true;
        }

        bool IsExcludedKeywordValue(const char* val, const char* const* excluded, size_t excludedCount)
        {
            for (size_t i = 0; i < excludedCount; ++i)
            {
                if (strcmp(val, excluded[i]) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        // Maps an ICU keyword value via uloc_toUnicodeLocaleType, lowercases, validates nonterminal syntax,
        // and filters excluded canonical types.
        bool TryNormalizeUnicodeType(
            const char* bcpKey,
            const char* rawVal,
            int32_t rawLen,
            const char* const* excluded,
            size_t excludedCount,
            char* outBuf,
            size_t outCap,
            size_t* outLen)
        {
            char rawCopy[64];
            if (!CopyAscii(rawVal, rawLen, rawCopy, sizeof(rawCopy), false))
            {
                return false;
            }
            const char* mapped = uloc_toUnicodeLocaleType(bcpKey, rawCopy);
            const char* source = (mapped != nullptr && mapped[0] != '\0') ? mapped : rawCopy;
            size_t sourceLen = strlen(source);
            if (sourceLen > static_cast<size_t>(INT32_MAX) ||
                !CopyAscii(source, static_cast<int32_t>(sourceLen), outBuf, outCap, true))
            {
                return false;
            }
            if (!IntlModern::IsUnicodeTypeNonterminal(outBuf, sourceLen))
            {
                return false;
            }
            if (IsExcludedKeywordValue(outBuf, excluded, excludedCount))
            {
                return false;
            }
            *outLen = sourceLen;
            return true;
        }

        bool IsCalendarAcceptedByDisplayNames(
            ULocaleDisplayNames* ldn,
            const char* calendarType,
            ScriptContext* scriptContext)
        {
            if (ldn == nullptr)
            {
                return true;
            }
            const char* legacyType = uloc_toLegacyType("calendar", calendarType);
            if (legacyType == nullptr || legacyType[0] == '\0')
            {
                legacyType = calendarType;
            }
            UChar dispBuf[256];
            UErrorCode dispStatus = U_ZERO_ERROR;
            int32_t dispLen = uldn_keyValueDisplayName(
                ldn,
                "calendar",
                legacyType,
                dispBuf,
                static_cast<int32_t>(_countof(dispBuf)),
                &dispStatus);
            CheckIcuOom(dispStatus, scriptContext);
            return (U_SUCCESS(dispStatus) || dispStatus == U_BUFFER_OVERFLOW_ERROR) && dispLen > 0;
        }

        void CollectAvailableCalendars(AsciiStringSet& set, ScriptContext* scriptContext)
        {
            static const char* const kExcludedCalendars[] = { "islamic", "islamic-rgsa", "unknown" };

            UErrorCode ldnStatus = U_ZERO_ERROR;
            UDisplayContext ldnContexts[4] =
            {
                UDISPCTX_STANDARD_NAMES,
                UDISPCTX_CAPITALIZATION_NONE,
                UDISPCTX_LENGTH_FULL,
                UDISPCTX_NO_SUBSTITUTE
            };
            PlatformAgnostic::ICUHelpers::ScopedULocaleDisplayNames ldn(
                uldn_openForContext("en", ldnContexts, static_cast<int32_t>(_countof(ldnContexts)), &ldnStatus));
            CheckIcuOom(ldnStatus, scriptContext);

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration values(
                ucal_getKeywordValuesForLocale("calendar", "und", FALSE, &status));
            CheckIcuOom(status, scriptContext);

            if (U_SUCCESS(status) && values != nullptr)
            {
                for (;;)
                {
                    int32_t elemLen = 0;
                    const char* elem = uenum_next(values, &elemLen, &status);
                    CheckIcuOom(status, scriptContext);
                    if (elem == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    char normalized[64];
                    size_t normLen = 0;
                    if (!TryNormalizeUnicodeType(
                            "ca",
                            elem,
                            elemLen,
                            kExcludedCalendars,
                            _countof(kExcludedCalendars),
                            normalized,
                            sizeof(normalized),
                            &normLen))
                    {
                        continue;
                    }
                    if (!IsCalendarAcceptedByDisplayNames(U_SUCCESS(ldnStatus) ? static_cast<ULocaleDisplayNames*>(ldn) : nullptr, normalized, scriptContext))
                    {
                        continue;
                    }
                    set.Add(normalized, normLen);
                }
            }
        }

        void CollectAvailableCollations(AsciiStringSet& set, ScriptContext* scriptContext)
        {
            static const char* const kExcludedCollations[] = { "standard", "search", "unknown" };

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration values(
                ucol_getKeywordValues("collation", &status));
            CheckIcuOom(status, scriptContext);

            if (U_SUCCESS(status) && values != nullptr)
            {
                for (;;)
                {
                    int32_t elemLen = 0;
                    const char* elem = uenum_next(values, &elemLen, &status);
                    CheckIcuOom(status, scriptContext);
                    if (elem == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    char normalized[64];
                    size_t normLen = 0;
                    if (TryNormalizeUnicodeType(
                            "co",
                            elem,
                            elemLen,
                            kExcludedCollations,
                            _countof(kExcludedCollations),
                            normalized,
                            sizeof(normalized),
                            &normLen))
                    {
                        set.Add(normalized, normLen);
                    }
                }
            }
        }

        // Matches IntlDisplayNames::LookupCurrencyName for locale "en", default style "long", fallback "none".
        bool HasEnCurrencyLongName(const char* upperCode, ScriptContext* scriptContext)
        {
            UChar currency[4] =
            {
                static_cast<UChar>(static_cast<unsigned char>(upperCode[0])),
                static_cast<UChar>(static_cast<unsigned char>(upperCode[1])),
                static_cast<UChar>(static_cast<unsigned char>(upperCode[2])),
                0
            };
            UErrorCode status = U_ZERO_ERROR;
            UBool isChoice = FALSE;
            int32_t nameLen = 0;
            const UChar* name = ucurr_getName(currency, "en", UCURR_LONG_NAME, &isChoice, &nameLen, &status);
            CheckIcuOom(status, scriptContext);
            if (U_FAILURE(status) || name == nullptr || nameLen <= 0)
            {
                return false;
            }
            bool isCodeEcho = (status == U_USING_DEFAULT_WARNING && nameLen == 3 &&
                name[0] == currency[0] && name[1] == currency[1] && name[2] == currency[2]);
            return !isCodeEcho;
        }

        void TryAddCurrencyCandidate(const char* code, int32_t len, AsciiStringSet& set, ScriptContext* scriptContext)
        {
            if (code == nullptr || len != 3)
            {
                return;
            }
            char upper[4];
            for (int32_t i = 0; i < 3; ++i)
            {
                if (!IntlModern::IsAsciiAlpha(code[i]))
                {
                    return;
                }
                upper[i] = IntlModern::ToAsciiUpper(code[i]);
            }
            upper[3] = '\0';
            if (HasEnCurrencyLongName(upper, scriptContext))
            {
                set.Add(upper, 3);
            }
        }

        void CollectAvailableCurrencies(AsciiStringSet& set, ScriptContext* scriptContext)
        {
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration isoCurrencies(
                ucurr_openISOCurrencies(UCURR_COMMON | UCURR_NON_DEPRECATED, &status));
            CheckIcuOom(status, scriptContext);

            if (U_SUCCESS(status) && isoCurrencies != nullptr)
            {
                for (;;)
                {
                    int32_t elemLen = 0;
                    const char* elem = uenum_next(isoCurrencies, &elemLen, &status);
                    CheckIcuOom(status, scriptContext);
                    if (elem == nullptr || U_FAILURE(status))
                    {
                        break;
                    }
                    TryAddCurrencyCandidate(elem, elemLen, set, scriptContext);
                }
            }
        }

        void CollectAvailableNumberingSystems(AsciiStringSet& set, ScriptContext* scriptContext)
        {
            static const char* const kExcludedNumberingSystems[] =
            {
                "finance", "native", "traditio", "unknown"
            };

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration names(
                unumsys_openAvailableNames(&status));
            CheckIcuOom(status, scriptContext);

            if (U_SUCCESS(status) && names != nullptr)
            {
                for (;;)
                {
                    int32_t elemLen = 0;
                    const char* elem = uenum_next(names, &elemLen, &status);
                    CheckIcuOom(status, scriptContext);
                    if (elem == nullptr || U_FAILURE(status))
                    {
                        break;
                    }

                    char lower[64];
                    if (!CopyAscii(elem, elemLen, lower, sizeof(lower), true))
                    {
                        continue;
                    }
                    size_t lowerLen = static_cast<size_t>(elemLen);
                    if (!IntlModern::IsUnicodeTypeNonterminal(lower, lowerLen))
                    {
                        continue;
                    }
                    const char* canonical = uloc_toUnicodeLocaleType("nu", lower);
                    if (canonical != nullptr && strcmp(canonical, lower) != 0)
                    {
                        continue;
                    }
                    if (IsExcludedKeywordValue(lower, kExcludedNumberingSystems, _countof(kExcludedNumberingSystems)))
                    {
                        continue;
                    }

                    UErrorCode nsErr = U_ZERO_ERROR;
                    PlatformAgnostic::ICUHelpers::ScopedUNumberingSystem ns(
                        unumsys_openByName(lower, &nsErr));
                    CheckIcuOom(nsErr, scriptContext);
                    if (U_SUCCESS(nsErr) && ns != nullptr &&
                        !unumsys_isAlgorithmic(ns) && unumsys_getRadix(ns) == 10)
                    {
                        set.Add(lower, lowerLen);
                    }
                }
            }
        }

        const char* CanonicalizeUtcAlias(const char* tz, size_t len, size_t* outLen)
        {
            static const char* const kUtcAliases[] =
            {
                "Etc/UTC",
                "Etc/GMT",
                "Etc/UCT",
                "GMT",
                "GMT0",
                "GMT+0",
                "GMT-0",
                "UCT",
                "Universal",
                "Zulu"
            };
            for (size_t i = 0; i < _countof(kUtcAliases); ++i)
            {
                if (strcmp(tz, kUtcAliases[i]) == 0)
                {
                    *outLen = 3;
                    return "UTC";
                }
            }
            *outLen = len;
            return tz;
        }

        void CollectAvailableTimeZones(AsciiStringSet& set, ScriptContext* scriptContext)
        {
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUEnumeration zones(
                ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL, nullptr, nullptr, &status));
            CheckIcuOom(status, scriptContext);

            if (U_SUCCESS(status) && zones != nullptr)
            {
                for (;;)
                {
                    int32_t elemLen = 0;
                    const char* elem = uenum_next(zones, &elemLen, &status);
                    CheckIcuOom(status, scriptContext);
                    if (elem == nullptr || U_FAILURE(status))
                    {
                        break;
                    }

                    char tzBuf[128];
                    if (!CopyAscii(elem, elemLen, tzBuf, sizeof(tzBuf), false))
                    {
                        continue;
                    }
                    if (strcmp(tzBuf, "Etc/Unknown") == 0 || strcmp(tzBuf, "Factory") == 0)
                    {
                        continue;
                    }

                    size_t canonLen = 0;
                    const char* canon = CanonicalizeUtcAlias(tzBuf, static_cast<size_t>(elemLen), &canonLen);
                    set.Add(canon, canonLen);
                }
            }

            set.Add("UTC", 3);
        }

        JavascriptArray* CreateArrayFromAsciiList(
            const char* const* items,
            size_t count,
            ScriptContext* scriptContext)
        {
            Assert(count <= static_cast<size_t>(UINT32_MAX));
            JavascriptArray* result = scriptContext->GetLibrary()->CreateArray(static_cast<uint32>(count));
            for (size_t i = 0; i < count; ++i)
            {
                Assert(i == 0 || UnsignedByteStrCmp(items[i - 1], items[i]) < 0);
                JavascriptString* item = IntlModern::CreateAsciiString(items[i], strlen(items[i]), scriptContext);
                result->DirectSetItemAt(static_cast<uint32>(i), item);
            }
            return result;
        }
    } // namespace

    void IntlEnumeration::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        IntlModern::AddIntlMethod(
            library,
            intlObject,
            _u("supportedValuesOf"),
            &EntryInfo::SupportedValuesOf,
            1);
    }

    Var IntlEnumeration::EntrySupportedValuesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.supportedValuesOf"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var keyArg = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptString* keyStr = JavascriptConversion::ToString(keyArg, scriptContext);

        EnumerationKey key = EnumerationKey::Calendar;
        if (!TryClassifyKey(keyStr, &key))
        {
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid key for Intl.supportedValuesOf"));
        }

        if (key == EnumerationKey::Unit)
        {
            return CreateArrayFromAsciiList(kSanctionedUnits, _countof(kSanctionedUnits), scriptContext);
        }

        AsciiStringSet set;
        switch (key)
        {
        case EnumerationKey::Calendar:
            CollectAvailableCalendars(set, scriptContext);
            break;
        case EnumerationKey::Collation:
            CollectAvailableCollations(set, scriptContext);
            break;
        case EnumerationKey::Currency:
            CollectAvailableCurrencies(set, scriptContext);
            break;
        case EnumerationKey::NumberingSystem:
            CollectAvailableNumberingSystems(set, scriptContext);
            break;
        case EnumerationKey::TimeZone:
            CollectAvailableTimeZones(set, scriptContext);
            break;
        case EnumerationKey::Unit:
            break;
        }

        set.FinishSortAndDeduplicate();
        return set.ToJsArray(scriptContext);
    }
} // namespace Js

#endif // INTL_ICU
