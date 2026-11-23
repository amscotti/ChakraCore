//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlDurationFormat.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3):
//
// Resource / creation call:   UListFormatter* from ulistfmt_openForType (EntryNewInstance); transient
//                             UResourceBundle*, UNumberFormatter*, UFormattedNumber*, UFormattedList*,
//                             and UConstrainedFieldPosition* scoped to synchronous leaf windows.
// Domain / allocator:         ICU library heap for ICU handles and formatted results; Recycler heap
//                             (RecyclerNewWithBarrierFinalized) for IntlDurationFormatState reachable
//                             via InternalPropertyIds::HiddenObject (IntlModern::SetState). Native
//                             scratch buffers use stack storage or NoCheckHeapNewArray/DeleteArray.
// Physical base / release:    UListFormatter* closed via ulistfmt_close in IntlDurationFormatState::Dispose.
//                             Transient ICU handles are owned by ScopedICUObject / ScopedUResourceBundle.
//                             Retained JavascriptString* pointers live in Field() write-barrier slots;
//                             enums, flags, ASCII config buffers, and UListFormatter* use FieldNoBarrier().
// Logical / borrowed views:   No ICU-owned pointer or Recycler character pointer survives across user code
//                             or GC allocation. Formatted ICU UChar buffers are copied into fresh
//                             Recycler strings (IntlModern::CreateUtf16String) before closing results.
// Owner / transfer point:     IntlDurationFormatState is the sole owner of UListFormatter*. A try/catch
//                             around IntlDurationFormatState::New closes listFormatter if allocation
//                             throws OOM. Any later failure before/during SetState leaves an unpublished
//                             state finalized by the Recycler.
// Borrow invalidation:        EntryNewInstance finishes all user-visible coercions (newTarget.prototype,
//                             CanonicalizeLocaleList, GetOptionsObject, option getters, ToString, and
//                             ToNumber) before ResolveLocale, ures_open, ulistfmt_openForType, and
//                             SetState. EntryFormat/EntryFormatToParts validate the receiver first,
//                             complete ToDurationRecord (property getters + ToNumber) before reacquiring
//                             the live formatter, and execute all ICU calls in a non-reentrant window.
// Thread/context:             Owning ScriptContext thread; Dispose() is idempotent (nulls listFormatter,
//                             calls InvalidateMagic()) for Recycler Finalize/Dispose and shutdown.
// Bounds / exhaustion:        Fixed-size state; bounded per-call scratch (at most 10 unit groups);
//                             ICU U_MEMORY_ALLOCATION_ERROR and native allocation failures throw JS OOM.
// Tests:                      test/Intl/IntlDurationFormatTest.js and test/intl402/DurationFormat.

namespace Js
{
    namespace
    {
        enum class DurationStyle : uint8
        {
            Long,
            Short,
            Narrow,
            Digital
        };

        enum class UnitStyle : uint8
        {
            Long,
            Short,
            Narrow,
            Numeric,
            TwoDigit,
            Fractional
        };

        enum class UnitDisplay : uint8
        {
            Auto,
            Always
        };

        enum DurationUnitIndex
        {
            Unit_Years = 0,
            Unit_Months = 1,
            Unit_Weeks = 2,
            Unit_Days = 3,
            Unit_Hours = 4,
            Unit_Minutes = 5,
            Unit_Seconds = 6,
            Unit_Milliseconds = 7,
            Unit_Microseconds = 8,
            Unit_Nanoseconds = 9,
            Unit_Count = 10
        };

        static const char16* const gLocaleMatcherValues[] = { _u("lookup"), _u("best fit") };
        static const char16* const gStyleValues[] = { _u("long"), _u("short"), _u("narrow"), _u("digital") };
        static const char16* const gDisplayValues[] = { _u("auto"), _u("always") };
        static const char16* const gDateUnitStyles[] = { _u("long"), _u("short"), _u("narrow") };
        static const char16* const gHmsUnitStyles[] = { _u("long"), _u("short"), _u("narrow"), _u("numeric"), _u("2-digit") };
        static const char16* const gSubsecUnitStyles[] = { _u("long"), _u("short"), _u("narrow"), _u("numeric") };

        struct UnitSpec
        {
            const char16* name;
            const char16* displayName;
            const char* singularName;
            const char16* const* allowedStyles;
            size_t allowedCount;
            UnitStyle digitalBase;
        };

        static const UnitSpec gUnits[Unit_Count] =
        {
            { _u("years"),        _u("yearsDisplay"),        "year",        gDateUnitStyles,   _countof(gDateUnitStyles),   UnitStyle::Short },
            { _u("months"),       _u("monthsDisplay"),       "month",       gDateUnitStyles,   _countof(gDateUnitStyles),   UnitStyle::Short },
            { _u("weeks"),        _u("weeksDisplay"),        "week",        gDateUnitStyles,   _countof(gDateUnitStyles),   UnitStyle::Short },
            { _u("days"),         _u("daysDisplay"),         "day",         gDateUnitStyles,   _countof(gDateUnitStyles),   UnitStyle::Short },
            { _u("hours"),        _u("hoursDisplay"),        "hour",        gHmsUnitStyles,    _countof(gHmsUnitStyles),    UnitStyle::Numeric },
            { _u("minutes"),      _u("minutesDisplay"),      "minute",      gHmsUnitStyles,    _countof(gHmsUnitStyles),    UnitStyle::Numeric },
            { _u("seconds"),      _u("secondsDisplay"),      "second",      gHmsUnitStyles,    _countof(gHmsUnitStyles),    UnitStyle::Numeric },
            { _u("milliseconds"), _u("millisecondsDisplay"), "millisecond", gSubsecUnitStyles, _countof(gSubsecUnitStyles), UnitStyle::Numeric },
            { _u("microseconds"), _u("microsecondsDisplay"), "microsecond", gSubsecUnitStyles, _countof(gSubsecUnitStyles), UnitStyle::Numeric },
            { _u("nanoseconds"),  _u("nanosecondsDisplay"),  "nanosecond",  gSubsecUnitStyles, _countof(gSubsecUnitStyles), UnitStyle::Numeric },
        };

        // Alphabetical order required by ECMA-402 ToDurationRecord.
        static const DurationUnitIndex gAlphabeticalUnits[Unit_Count] =
        {
            Unit_Days,
            Unit_Hours,
            Unit_Microseconds,
            Unit_Milliseconds,
            Unit_Minutes,
            Unit_Months,
            Unit_Nanoseconds,
            Unit_Seconds,
            Unit_Weeks,
            Unit_Years
        };

        static constexpr unsigned __int128 kMaxTimeDurationNs =
            static_cast<unsigned __int128>(9007199254740992ULL) * 1000000000ULL;

        static const unsigned __int128 gTimeUnitScales[7] =
        {
            static_cast<unsigned __int128>(86400ULL) * 1000000000ULL, // days
            static_cast<unsigned __int128>(3600ULL) * 1000000000ULL,  // hours
            static_cast<unsigned __int128>(60ULL) * 1000000000ULL,    // minutes
            1000000000ULL,                                            // seconds
            1000000ULL,                                               // milliseconds
            1000ULL,                                                  // microseconds
            1ULL                                                      // nanoseconds
        };

        struct DurationFormatConfig
        {
            DurationStyle style;
            UnitStyle unitStyles[Unit_Count];
            UnitDisplay unitDisplays[Unit_Count];
            int8 fractionalDigits;
            char chosenNu[32];
            char icuLocale[ULOC_FULLNAME_CAPACITY];
        };

        class IntlDurationFormatState : public IntlModern::IntlModernState
        {
        private:
            FieldNoBarrier(UListFormatter*) listFormatter;
            Field(JavascriptString*) locale;
            Field(JavascriptString*) numberingSystem;
            Field(JavascriptString*) styleStr;
            Field(JavascriptString*) timeSeparator;
            FieldNoBarrier(DurationFormatConfig) config;

        public:
            IntlDurationFormatState(
                UListFormatter* listFormatter,
                JavascriptString* locale,
                JavascriptString* numberingSystem,
                JavascriptString* styleStr,
                JavascriptString* timeSeparator,
                const DurationFormatConfig& config) :
                IntlModern::IntlModernState(IntlModern::IntlModernKind::DurationFormat),
                listFormatter(listFormatter),
                locale(locale),
                numberingSystem(numberingSystem),
                styleStr(styleStr),
                timeSeparator(timeSeparator),
                config(config)
            {
            }

            static IntlDurationFormatState* New(
                Recycler* recycler,
                UListFormatter* listFormatter,
                JavascriptString* locale,
                JavascriptString* numberingSystem,
                JavascriptString* styleStr,
                JavascriptString* timeSeparator,
                const DurationFormatConfig& config)
            {
                return RecyclerNewWithBarrierFinalized(
                    recycler,
                    IntlDurationFormatState,
                    listFormatter,
                    locale,
                    numberingSystem,
                    styleStr,
                    timeSeparator,
                    config);
            }

            UListFormatter* GetListFormatter() const { return listFormatter; }
            JavascriptString* GetLocale() const { return locale; }
            JavascriptString* GetNumberingSystem() const { return numberingSystem; }
            JavascriptString* GetStyleStr() const { return styleStr; }
            JavascriptString* GetTimeSeparator() const { return timeSeparator; }
            const DurationFormatConfig& GetConfig() const { return config; }

            virtual JavascriptString* GetLocaleTag() const override { return locale; }

            virtual void Dispose(bool isShutdown) override
            {
                if (listFormatter != nullptr)
                {
                    ulistfmt_close(listFormatter);
                    listFormatter = nullptr;
                }
                InvalidateMagic();
            }
        };

        void ThrowIcuFailure(ScriptContext* scriptContext, UErrorCode status)
        {
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: internal ICU failure"));
        }

        bool StringEquals(JavascriptString* str, const char16* literal)
        {
            return PAL_wcscmp(str->GetSz(), literal) == 0;
        }

        DurationStyle ParseDurationStyle(JavascriptString* str)
        {
            if (StringEquals(str, _u("long"))) return DurationStyle::Long;
            if (StringEquals(str, _u("narrow"))) return DurationStyle::Narrow;
            if (StringEquals(str, _u("digital"))) return DurationStyle::Digital;
            return DurationStyle::Short;
        }

        UnitStyle BaseStyleToUnitStyle(DurationStyle style)
        {
            switch (style)
            {
            case DurationStyle::Long: return UnitStyle::Long;
            case DurationStyle::Narrow: return UnitStyle::Narrow;
            default: return UnitStyle::Short;
            }
        }

        UnitStyle ParseUnitStyle(JavascriptString* str)
        {
            if (StringEquals(str, _u("long"))) return UnitStyle::Long;
            if (StringEquals(str, _u("narrow"))) return UnitStyle::Narrow;
            if (StringEquals(str, _u("numeric"))) return UnitStyle::Numeric;
            if (StringEquals(str, _u("2-digit"))) return UnitStyle::TwoDigit;
            return UnitStyle::Short;
        }

        const char* UnitStyleToAscii(UnitStyle style)
        {
            switch (style)
            {
            case UnitStyle::Long: return "long";
            case UnitStyle::Short: return "short";
            case UnitStyle::Narrow: return "narrow";
            case UnitStyle::TwoDigit: return "2-digit";
            case UnitStyle::Numeric:
            case UnitStyle::Fractional:
            default:
                return "numeric";
            }
        }

        UnitDisplay ParseUnitDisplay(JavascriptString* str)
        {
            return StringEquals(str, _u("always")) ? UnitDisplay::Always : UnitDisplay::Auto;
        }

        const char* UnitDisplayToAscii(UnitDisplay display)
        {
            return display == UnitDisplay::Always ? "always" : "auto";
        }

        const char16* UnitDisplayToWide(UnitDisplay display)
        {
            return display == UnitDisplay::Always ? _u("always") : _u("auto");
        }

        bool IsNumericLike(UnitStyle style)
        {
            return style == UnitStyle::Fractional || style == UnitStyle::Numeric || style == UnitStyle::TwoDigit;
        }

        // Extracts the single-subtag value of the "nu" unicode extension keyword from a canonical tag.
        bool ExtractNuValue(const char* tag, size_t len, char* out, size_t cap)
        {
            size_t pos = 0;
            bool inU = false;
            bool seenFirst = false;
            while (pos < len)
            {
                size_t start = pos;
                while (pos < len && tag[pos] != '-')
                {
                    ++pos;
                }
                size_t sublen = pos - start;
                ++pos;
                if (!seenFirst)
                {
                    seenFirst = true;
                    continue;
                }
                if (sublen == 1)
                {
                    char sig = IntlModern::ToAsciiLower(tag[start]);
                    if (sig == 'x')
                    {
                        return false;
                    }
                    inU = (sig == 'u');
                    continue;
                }
                if (!inU || sublen != 2)
                {
                    continue;
                }
                bool isNu = IntlModern::ToAsciiLower(tag[start]) == 'n' && IntlModern::ToAsciiLower(tag[start + 1]) == 'u';
                size_t valStart = 0;
                size_t valLen = 0;
                size_t valCount = 0;
                while (pos < len)
                {
                    size_t s2 = pos;
                    size_t p2 = pos;
                    while (p2 < len && tag[p2] != '-')
                    {
                        ++p2;
                    }
                    size_t l2 = p2 - s2;
                    if (l2 < 3)
                    {
                        break;
                    }
                    if (valCount == 0)
                    {
                        valStart = s2;
                        valLen = l2;
                    }
                    ++valCount;
                    pos = p2 + 1;
                }
                if (isNu)
                {
                    if (valCount == 1 && valLen < cap)
                    {
                        for (size_t i = 0; i < valLen; ++i)
                        {
                            out[i] = IntlModern::ToAsciiLower(tag[valStart + i]);
                        }
                        out[valLen] = '\0';
                        return true;
                    }
                    return false;
                }
            }
            return false;
        }

        size_t BaseTagLength(const char* tag, size_t len)
        {
            size_t pos = 0;
            bool first = true;
            while (pos < len)
            {
                size_t start = pos;
                while (pos < len && tag[pos] != '-')
                {
                    ++pos;
                }
                if (!first && pos - start == 1)
                {
                    return start - 1;
                }
                first = false;
                ++pos;
            }
            return len;
        }

        bool FindRequestedNu(JavascriptArray* requested, const char* dataLocale, size_t dataLen, char* out, size_t cap)
        {
            uint32 count = requested->GetLength();
            for (uint32 i = 0; i < count; ++i)
            {
                Var item = requested->DirectGetItem(i);
                if (!VarIs<JavascriptString>(item))
                {
                    continue;
                }
                IntlModern::ScopedAsciiString ascii;
                if (!ascii.Init(VarTo<JavascriptString>(item)))
                {
                    continue;
                }
                size_t baseLen = BaseTagLength(ascii.Data(), ascii.Length());
                if (baseLen < dataLen || strncmp(ascii.Data(), dataLocale, dataLen) != 0)
                {
                    continue;
                }
                if (baseLen > dataLen && ascii.Data()[dataLen] != '-')
                {
                    continue;
                }
                return ExtractNuValue(ascii.Data(), ascii.Length(), out, cap);
            }
            return false;
        }

        void ToIcuLocaleId(const char* bcp47, char* out, int32_t cap, ScriptContext* scriptContext)
        {
            UErrorCode status = U_ZERO_ERROR;
            int32_t parsed = 0;
            uloc_forLanguageTag(bcp47, out, cap, &parsed, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
        }

        bool TryLookupTimeSeparator(const char* icuBaseLocale, const char* nu, ScriptContext* scriptContext, JavascriptString** outSep)
        {
            char cur[ULOC_FULLNAME_CAPACITY] = { 0 };
            size_t baseLen = strlen(icuBaseLocale);
            if (baseLen >= sizeof(cur))
            {
                baseLen = sizeof(cur) - 1;
            }
            js_memcpy_s(cur, sizeof(cur), icuBaseLocale, baseLen);
            cur[baseLen] = '\0';

            for (;;)
            {
                const char* openLoc = (cur[0] == '\0') ? "root" : cur;
                UErrorCode status = U_ZERO_ERROR;
                PlatformAgnostic::ICUHelpers::ScopedUResourceBundle rb(ures_open(nullptr, openLoc, &status));
                if (status == U_MEMORY_ALLOCATION_ERROR)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                if (U_SUCCESS(status) && rb != nullptr)
                {
                    PlatformAgnostic::ICUHelpers::ScopedUResourceBundle ne(ures_getByKey(rb, "NumberElements", nullptr, &status));
                    if (status == U_MEMORY_ALLOCATION_ERROR)
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }
                    if (U_SUCCESS(status) && ne != nullptr)
                    {
                        PlatformAgnostic::ICUHelpers::ScopedUResourceBundle num(ures_getByKey(ne, nu, nullptr, &status));
                        if (status == U_MEMORY_ALLOCATION_ERROR)
                        {
                            JavascriptError::ThrowOutOfMemoryError(scriptContext);
                        }
                        if (U_SUCCESS(status) && num != nullptr)
                        {
                            PlatformAgnostic::ICUHelpers::ScopedUResourceBundle sym(ures_getByKey(num, "symbols", nullptr, &status));
                            if (status == U_MEMORY_ALLOCATION_ERROR)
                            {
                                JavascriptError::ThrowOutOfMemoryError(scriptContext);
                            }
                            if (U_SUCCESS(status) && sym != nullptr)
                            {
                                int32_t sepLen = 0;
                                const UChar* sep = ures_getStringByKey(sym, "timeSeparator", &sepLen, &status);
                                if (status == U_MEMORY_ALLOCATION_ERROR)
                                {
                                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                                }
                                if (U_SUCCESS(status) && sep != nullptr && sepLen > 0)
                                {
                                    *outSep = IntlModern::CreateUtf16String(sep, sepLen, scriptContext);
                                    return true;
                                }
                            }
                        }
                    }
                }

                if (cur[0] == '\0')
                {
                    break;
                }
                UErrorCode pStatus = U_ZERO_ERROR;
                char parent[ULOC_FULLNAME_CAPACITY] = { 0 };
                uloc_getParent(cur, parent, static_cast<int32_t>(sizeof(parent)), &pStatus);
                if (U_FAILURE(pStatus) || strcmp(parent, cur) == 0)
                {
                    break;
                }
                size_t plen = strlen(parent);
                js_memcpy_s(cur, sizeof(cur), parent, plen + 1);
            }
            return false;
        }

        JavascriptString* ResolveTimeSeparator(const char* icuBaseLocale, const char* nu, ScriptContext* scriptContext)
        {
            JavascriptString* sep = nullptr;
            if (TryLookupTimeSeparator(icuBaseLocale, nu, scriptContext, &sep))
            {
                return sep;
            }
            if (strcmp(nu, "latn") != 0 && TryLookupTimeSeparator(icuBaseLocale, "latn", scriptContext, &sep))
            {
                return sep;
            }
            return IntlModern::CreateAsciiString(":", 1, scriptContext);
        }

        struct DurationRecord
        {
            unsigned __int128 absUnits[Unit_Count];
            bool isNegative;
        };

        // Extracts the exact non-negative integer value from a finite integral double with 0 <= x < 1e26.
        unsigned __int128 ExtractExactInteger128(double x)
        {
            if (x == 0.0)
            {
                return 0;
            }
            uint64_t bits = 0;
            js_memcpy_s(&bits, sizeof(bits), &x, sizeof(x));
            int exp = static_cast<int>((bits >> 52) & 0x7ffU) - 1023;
            uint64_t mant = (bits & ((1ULL << 52) - 1ULL)) | (1ULL << 52);
            if (exp >= 52)
            {
                return static_cast<unsigned __int128>(mant) << (exp - 52);
            }
            return static_cast<unsigned __int128>(mant >> (52 - exp));
        }

        void ValidateDurationRecordRanges(const unsigned __int128 absUnits[Unit_Count], bool negativeSign, ScriptContext* scriptContext)
        {
            static constexpr unsigned __int128 kDateLimit = 4294967296ULL; // 2^32
            if (absUnits[Unit_Years] >= kDateLimit ||
                absUnits[Unit_Months] >= kDateLimit ||
                absUnits[Unit_Weeks] >= kDateLimit)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration date value out of range"));
            }

            unsigned __int128 totalTimeNs = 0;
            for (int i = Unit_Days; i <= Unit_Nanoseconds; ++i)
            {
                unsigned __int128 mag = absUnits[i];
                unsigned __int128 scale = gTimeUnitScales[i - Unit_Days];
                if (mag > (kMaxTimeDurationNs - 1) / scale)
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration time value out of range"));
                }
                unsigned __int128 term = mag * scale;
                if (term >= kMaxTimeDurationNs - totalTimeNs)
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration time value out of range"));
                }
                totalTimeNs += term;
            }
            (void)negativeSign;
        }

        double ToIntegerIfIntegral(Var val, ScriptContext* scriptContext)
        {
            double d = JavascriptConversion::ToNumber(val, scriptContext);
            if (!NumberUtilities::IsFinite(d) || floor(d) != d)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration field must be a finite integer"));
            }
            return (d == 0.0) ? 0.0 : d;
        }

        bool ParseAsciiDigits128(const char16* s, charcount_t len, size_t& pos, unsigned __int128* outVal)
        {
            if (pos >= len || s[pos] < _u('0') || s[pos] > _u('9'))
            {
                return false;
            }
            unsigned __int128 val = 0;
            while (pos < len && s[pos] >= _u('0') && s[pos] <= _u('9'))
            {
                unsigned digit = static_cast<unsigned>(s[pos] - _u('0'));
                if (val > (kMaxTimeDurationNs - digit) / 10)
                {
                    return false;
                }
                val = val * 10 + digit;
                ++pos;
            }
            *outVal = val;
            return true;
        }

        void DistributeSubsecondsNs(uint64_t nsTotal, unsigned __int128 absUnits[Unit_Count], int startUnit)
        {
            if (startUnit <= Unit_Minutes)
            {
                absUnits[Unit_Minutes] = nsTotal / 60000000000ULL;
                nsTotal %= 60000000000ULL;
            }
            if (startUnit <= Unit_Seconds)
            {
                absUnits[Unit_Seconds] = nsTotal / 1000000000ULL;
                nsTotal %= 1000000000ULL;
            }
            if (startUnit <= Unit_Milliseconds)
            {
                absUnits[Unit_Milliseconds] = nsTotal / 1000000ULL;
                nsTotal %= 1000000ULL;
            }
            if (startUnit <= Unit_Microseconds)
            {
                absUnits[Unit_Microseconds] = nsTotal / 1000ULL;
                nsTotal %= 1000ULL;
            }
            absUnits[Unit_Nanoseconds] = nsTotal;
        }

        DurationRecord ParseIsoDurationString(JavascriptString* str, ScriptContext* scriptContext)
        {
            const char16* s = str->GetString();
            charcount_t len = str->GetLength();
            size_t pos = 0;
            bool negative = false;

            if (pos < len)
            {
                if (s[pos] == _u('+'))
                {
                    ++pos;
                }
                else if (s[pos] == _u('-') || s[pos] == 0x2212)
                {
                    negative = true;
                    ++pos;
                }
            }

            if (pos >= len || (s[pos] != _u('P') && s[pos] != _u('p')))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
            }
            ++pos;

            DurationRecord record = {};
            bool hasAnyComponent = false;
            int dateStage = 0;

            while (pos < len && s[pos] != _u('T') && s[pos] != _u('t'))
            {
                unsigned __int128 val = 0;
                if (!ParseAsciiDigits128(s, len, pos, &val) || pos >= len)
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                }
                char16 des = s[pos];
                if ((des == _u('Y') || des == _u('y')) && dateStage < 1)
                {
                    dateStage = 1;
                    record.absUnits[Unit_Years] = val;
                }
                else if ((des == _u('M') || des == _u('m')) && dateStage < 2)
                {
                    dateStage = 2;
                    record.absUnits[Unit_Months] = val;
                }
                else if ((des == _u('W') || des == _u('w')) && dateStage < 3)
                {
                    dateStage = 3;
                    record.absUnits[Unit_Weeks] = val;
                }
                else if ((des == _u('D') || des == _u('d')) && dateStage < 4)
                {
                    dateStage = 4;
                    record.absUnits[Unit_Days] = val;
                }
                else
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                }
                hasAnyComponent = true;
                ++pos;
            }

            if (pos < len)
            {
                // Must be 'T' or 't'.
                ++pos;
                bool hasTimeComponent = false;
                bool sawFraction = false;
                int timeStage = 0;

                while (pos < len)
                {
                    if (sawFraction)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                    }
                    unsigned __int128 val = 0;
                    if (!ParseAsciiDigits128(s, len, pos, &val))
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                    }

                    bool hasFrac = false;
                    uint64_t frac9 = 0;
                    if (pos < len && (s[pos] == _u('.') || s[pos] == _u(',')))
                    {
                        hasFrac = true;
                        sawFraction = true;
                        ++pos;
                        size_t fracCount = 0;
                        while (pos < len && s[pos] >= _u('0') && s[pos] <= _u('9'))
                        {
                            if (fracCount == 9)
                            {
                                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                            }
                            frac9 = frac9 * 10ULL + static_cast<uint64_t>(s[pos] - _u('0'));
                            ++fracCount;
                            ++pos;
                        }
                        if (fracCount == 0)
                        {
                            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                        }
                        while (fracCount < 9)
                        {
                            frac9 *= 10ULL;
                            ++fracCount;
                        }
                    }

                    if (pos >= len)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                    }
                    char16 des = s[pos];
                    if ((des == _u('H') || des == _u('h')) && timeStage < 1)
                    {
                        timeStage = 1;
                        record.absUnits[Unit_Hours] = val;
                        if (hasFrac)
                        {
                            DistributeSubsecondsNs(frac9 * 3600ULL, record.absUnits, Unit_Minutes);
                        }
                    }
                    else if ((des == _u('M') || des == _u('m')) && timeStage < 2)
                    {
                        timeStage = 2;
                        record.absUnits[Unit_Minutes] = val;
                        if (hasFrac)
                        {
                            DistributeSubsecondsNs(frac9 * 60ULL, record.absUnits, Unit_Seconds);
                        }
                    }
                    else if ((des == _u('S') || des == _u('s')) && timeStage < 3)
                    {
                        timeStage = 3;
                        record.absUnits[Unit_Seconds] = val;
                        if (hasFrac)
                        {
                            DistributeSubsecondsNs(frac9, record.absUnits, Unit_Milliseconds);
                        }
                    }
                    else
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                    }

                    hasTimeComponent = true;
                    hasAnyComponent = true;
                    ++pos;
                }

                if (!hasTimeComponent)
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
                }
            }

            if (!hasAnyComponent)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid duration string"));
            }

            bool anyNonZero = false;
            for (int i = 0; i < Unit_Count; ++i)
            {
                if (record.absUnits[i] != 0)
                {
                    anyNonZero = true;
                    break;
                }
            }
            record.isNegative = negative && anyNonZero;
            ValidateDurationRecordRanges(record.absUnits, record.isNegative, scriptContext);
            return record;
        }

        DurationRecord ToDurationRecord(Var durationArg, ScriptContext* scriptContext)
        {
            if (VarIs<JavascriptString>(durationArg))
            {
                return ParseIsoDurationString(VarTo<JavascriptString>(durationArg), scriptContext);
            }
            if (!JavascriptOperators::IsObject(durationArg))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }

            RecyclableObject* obj = VarTo<RecyclableObject>(durationArg);
            double values[Unit_Count] = {};
            bool anyPresent = false;

            for (int i = 0; i < Unit_Count; ++i)
            {
                DurationUnitIndex unit = gAlphabeticalUnits[i];
                const char16* propName = gUnits[unit].name;
                PropertyId propId = scriptContext->GetOrAddPropertyIdTracked(propName, static_cast<charcount_t>(wcslen(propName)));
                Var val = JavascriptOperators::GetProperty(obj, propId, scriptContext);
                if (!JavascriptOperators::IsUndefined(val))
                {
                    anyPresent = true;
                    values[unit] = ToIntegerIfIntegral(val, scriptContext);
                }
            }

            if (!anyPresent)
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
            }

            int sign = 0;
            for (int i = 0; i < Unit_Count; ++i)
            {
                if (values[i] > 0.0)
                {
                    if (sign < 0)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration fields cannot have mixed signs"));
                    }
                    sign = 1;
                }
                else if (values[i] < 0.0)
                {
                    if (sign > 0)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration fields cannot have mixed signs"));
                    }
                    sign = -1;
                }
            }

            DurationRecord record = {};
            record.isNegative = (sign < 0);

            for (int i = 0; i < Unit_Count; ++i)
            {
                double mag = fabs(values[i]);
                if (i <= Unit_Weeks)
                {
                    if (mag >= 4294967296.0)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration date value out of range"));
                    }
                }
                else
                {
                    if (mag >= 1e26)
                    {
                        IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: duration time value out of range"));
                    }
                }
                record.absUnits[i] = ExtractExactInteger128(mag);
            }

            ValidateDurationRecordRanges(record.absUnits, record.isNegative, scriptContext);
            return record;
        }

        // Fixed-capacity UTF-16 buffer for group strings and skeletons; falls back to NoCheckHeap if needed.
        class UCharBuffer
        {
        private:
            UChar stackBuf[128];
            UChar* buf;
            int32_t len;
            int32_t cap;
            bool oom;

        public:
            UCharBuffer() : buf(stackBuf), len(0), cap(static_cast<int32_t>(_countof(stackBuf))), oom(false)
            {
                stackBuf[0] = 0;
            }

            ~UCharBuffer()
            {
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(static_cast<size_t>(cap), buf);
                }
            }

            UCharBuffer(const UCharBuffer&) = delete;
            UCharBuffer& operator=(const UCharBuffer&) = delete;

            bool Append(const UChar* s, int32_t n)
            {
                if (n <= 0)
                {
                    return !oom;
                }
                if (oom)
                {
                    return false;
                }
                if (len > INT32_MAX - n - 1)
                {
                    oom = true;
                    return false;
                }
                int32_t needed = len + n + 1;
                if (needed > cap)
                {
                    int32_t newCap = cap;
                    while (newCap < needed)
                    {
                        if (newCap > INT32_MAX / 2)
                        {
                            newCap = needed;
                            break;
                        }
                        newCap *= 2;
                    }
                    UChar* newBuf = NoCheckHeapNewArray(UChar, static_cast<size_t>(newCap));
                    if (newBuf == nullptr)
                    {
                        oom = true;
                        return false;
                    }
                    js_memcpy_s(newBuf, static_cast<size_t>(newCap) * sizeof(UChar), buf, static_cast<size_t>(len + 1) * sizeof(UChar));
                    if (buf != stackBuf)
                    {
                        NoCheckHeapDeleteArray(static_cast<size_t>(cap), buf);
                    }
                    buf = newBuf;
                    cap = newCap;
                }
                js_memcpy_s(buf + len, static_cast<size_t>(cap - len) * sizeof(UChar), s, static_cast<size_t>(n) * sizeof(UChar));
                len += n;
                buf[len] = 0;
                return true;
            }

            bool AppendAscii(const char* ascii)
            {
                if (ascii == nullptr)
                {
                    return !oom;
                }
                UChar tmp[128];
                size_t total = strlen(ascii);
                size_t offset = 0;
                while (offset < total)
                {
                    size_t chunk = total - offset;
                    if (chunk > _countof(tmp))
                    {
                        chunk = _countof(tmp);
                    }
                    for (size_t i = 0; i < chunk; ++i)
                    {
                        tmp[i] = static_cast<UChar>(static_cast<unsigned char>(ascii[offset + i]));
                    }
                    if (!Append(tmp, static_cast<int32_t>(chunk)))
                    {
                        return false;
                    }
                    offset += chunk;
                }
                return true;
            }

            const UChar* Data() const { return buf; }
            int32_t Length() const { return len; }
            bool IsEmpty() const { return len == 0; }
            bool IsOom() const { return oom; }
        };

        template <typename T>
        class ScopedNativeArray
        {
        private:
            T* data;
            size_t count;

        public:
            ScopedNativeArray() : data(nullptr), count(0) {}
            ~ScopedNativeArray()
            {
                if (data != nullptr)
                {
                    NoCheckHeapDeleteArray(count, data);
                }
            }
            ScopedNativeArray(const ScopedNativeArray&) = delete;
            ScopedNativeArray& operator=(const ScopedNativeArray&) = delete;

            bool Init(size_t n)
            {
                count = n;
                if (n == 0)
                {
                    return true;
                }
                data = NoCheckHeapNewArray(T, n);
                return data != nullptr;
            }
            T& operator[](size_t i) const { return data[i]; }
        };

        void FormatUInt128Decimal(unsigned __int128 value, char* buf, size_t cap, size_t* outLen)
        {
            char rev[48];
            size_t rlen = 0;
            if (value == 0)
            {
                rev[rlen++] = '0';
            }
            else
            {
                while (value > 0)
                {
                    rev[rlen++] = static_cast<char>('0' + (value % 10));
                    value /= 10;
                }
            }
            Assert(rlen < cap);
            for (size_t i = 0; i < rlen; ++i)
            {
                buf[i] = rev[rlen - 1 - i];
            }
            buf[rlen] = '\0';
            *outLen = rlen;
        }

        // Formats a unit value (or combined subseconds value) into an exact ASCII decimal string.
        // Returns true if the exact pre-rounding value is non-zero.
        bool BuildDecimalValue(
            const DurationRecord& record,
            int unitIndex,
            bool combineSubseconds,
            bool forceNegativeZero,
            char* outBuf,
            size_t outCap)
        {
            unsigned __int128 totalSub = 0;
            unsigned __int128 divisor = 1;
            int fracWidth = 0;

            if (combineSubseconds)
            {
                if (unitIndex == Unit_Seconds)
                {
                    fracWidth = 9;
                    divisor = 1000000000ULL;
                    totalSub = record.absUnits[Unit_Seconds] * 1000000000ULL +
                               record.absUnits[Unit_Milliseconds] * 1000000ULL +
                               record.absUnits[Unit_Microseconds] * 1000ULL +
                               record.absUnits[Unit_Nanoseconds];
                }
                else if (unitIndex == Unit_Milliseconds)
                {
                    fracWidth = 6;
                    divisor = 1000000ULL;
                    totalSub = record.absUnits[Unit_Milliseconds] * 1000000ULL +
                               record.absUnits[Unit_Microseconds] * 1000ULL +
                               record.absUnits[Unit_Nanoseconds];
                }
                else
                {
                    Assert(unitIndex == Unit_Microseconds);
                    fracWidth = 3;
                    divisor = 1000ULL;
                    totalSub = record.absUnits[Unit_Microseconds] * 1000ULL +
                               record.absUnits[Unit_Nanoseconds];
                }
            }
            else
            {
                totalSub = record.absUnits[unitIndex];
            }

            bool isNonZero = (totalSub != 0);
            size_t pos = 0;
            if ((isNonZero && record.isNegative) || (!isNonZero && forceNegativeZero))
            {
                outBuf[pos++] = '-';
            }

            if (!combineSubseconds || (totalSub % divisor) == 0)
            {
                unsigned __int128 whole = combineSubseconds ? (totalSub / divisor) : totalSub;
                size_t wlen = 0;
                FormatUInt128Decimal(whole, outBuf + pos, outCap - pos, &wlen);
                return isNonZero;
            }

            unsigned __int128 q = totalSub / divisor;
            uint64_t r = static_cast<uint64_t>(totalSub % divisor);
            size_t qlen = 0;
            FormatUInt128Decimal(q, outBuf + pos, outCap - pos, &qlen);
            pos += qlen;
            outBuf[pos++] = '.';

            for (int d = fracWidth - 1; d >= 0; --d)
            {
                outBuf[pos + d] = static_cast<char>('0' + (r % 10ULL));
                r /= 10ULL;
            }
            pos += static_cast<size_t>(fracWidth);
            outBuf[pos] = '\0';
            return isNonZero;
        }

        void BuildUnitSkeleton(
            const DurationFormatConfig& config,
            int unitIndex,
            bool suppressSign,
            bool hasFractionalPrecision,
            UCharBuffer& skel)
        {
            UnitStyle style = config.unitStyles[unitIndex];
            skel.AppendAscii("numbering-system/");
            skel.AppendAscii(config.chosenNu);
            if (suppressSign)
            {
                skel.AppendAscii(" sign-never");
            }
            if (style == UnitStyle::Numeric || style == UnitStyle::TwoDigit)
            {
                skel.AppendAscii(" group-off");
                if (style == UnitStyle::TwoDigit)
                {
                    skel.AppendAscii(" 00");
                }
            }
            else
            {
                skel.AppendAscii(" measure-unit/duration-");
                skel.AppendAscii(gUnits[unitIndex].singularName);
                if (style == UnitStyle::Long)
                {
                    skel.AppendAscii(" unit-width-full-name");
                }
                else if (style == UnitStyle::Narrow)
                {
                    skel.AppendAscii(" unit-width-narrow");
                }
                else
                {
                    skel.AppendAscii(" unit-width-short");
                }
            }

            if (hasFractionalPrecision)
            {
                skel.AppendAscii(" rounding-mode-down ");
                if (config.fractionalDigits < 0)
                {
                    skel.AppendAscii(".#########");
                }
                else if (config.fractionalDigits == 0)
                {
                    skel.AppendAscii("precision-integer");
                }
                else
                {
                    char fracBuf[16];
                    fracBuf[0] = '.';
                    for (int i = 0; i < config.fractionalDigits; ++i)
                    {
                        fracBuf[1 + i] = '0';
                    }
                    fracBuf[1 + config.fractionalDigits] = '\0';
                    skel.AppendAscii(fracBuf);
                }
            }
        }

        const char* NumberPartType(int32_t field, UChar firstChar)
        {
            switch (field)
            {
            case UNUM_INTEGER_FIELD: return "integer";
            case UNUM_FRACTION_FIELD: return "fraction";
            case UNUM_DECIMAL_SEPARATOR_FIELD: return "decimal";
            case UNUM_GROUPING_SEPARATOR_FIELD: return "group";
            case UNUM_SIGN_FIELD: return firstChar == u'+' ? "plusSign" : "minusSign";
            case UNUM_PERCENT_FIELD: return "percentSign";
            case UNUM_PERMILL_FIELD: return "perMille";
            case UNUM_EXPONENT_SYMBOL_FIELD: return "exponentSeparator";
            case UNUM_EXPONENT_SIGN_FIELD: return "exponentMinusSign";
            case UNUM_EXPONENT_FIELD: return "exponentInteger";
            case UNUM_COMPACT_FIELD: return "compact";
            case UNUM_CURRENCY_FIELD: return "currency";
            case UNUM_MEASURE_UNIT_FIELD: return "unit";
            default: return "literal";
            }
        }

        struct NumberFieldSpan
        {
            int32_t field;
            int32_t start;
            int32_t limit;
        };

        void AppendNumberParts(
            const UFormattedValue* value,
            const UChar* chars,
            int32_t len,
            const char* unitSingular,
            JavascriptArray* outGroupParts,
            uint32* inOutPartCount,
            ScriptContext* scriptContext)
        {
            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUConstrainedFieldPosition pos(ucfpos_open(&status));
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
            ucfpos_constrainCategory(pos, UFIELD_CATEGORY_NUMBER, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }

            size_t fieldCount = 0;
            while (ufmtval_nextPosition(value, pos, &status))
            {
                ++fieldCount;
            }
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }

            ucfpos_reset(pos, &status);
            ucfpos_constrainCategory(pos, UFIELD_CATEGORY_NUMBER, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }

            ScopedNativeArray<NumberFieldSpan> fields;
            ScopedNativeArray<int32_t> owner;
            if (!fields.Init(fieldCount) || !owner.Init(static_cast<size_t>(len)))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }

            size_t recorded = 0;
            while (recorded < fieldCount && ufmtval_nextPosition(value, pos, &status))
            {
                NumberFieldSpan f;
                f.field = ucfpos_getField(pos, &status);
                ucfpos_getIndexes(pos, &f.start, &f.limit, &status);
                if (ICU_FAILURE(status))
                {
                    ThrowIcuFailure(scriptContext, status);
                }
                if (f.start >= 0 && f.limit > f.start && f.limit <= len)
                {
                    fields[recorded++] = f;
                }
            }
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }

            for (int32_t i = 0; i < len; ++i)
            {
                owner[i] = -1;
            }
            for (size_t k = 0; k < recorded; ++k)
            {
                for (int32_t i = fields[k].start; i < fields[k].limit; ++i)
                {
                    int32_t cur = owner[i];
                    if (cur < 0 || (fields[k].limit - fields[k].start) < (fields[cur].limit - fields[cur].start))
                    {
                        owner[i] = static_cast<int32_t>(k);
                    }
                }
            }

            JavascriptString* unitStr = IntlModern::CreateAsciiString(unitSingular, scriptContext);
            int32_t runStart = 0;
            while (runStart < len)
            {
                int32_t runOwner = owner[runStart];
                int32_t runEnd = runStart + 1;
                while (runEnd < len && owner[runEnd] == runOwner)
                {
                    ++runEnd;
                }

                const char* typeName = "literal";
                if (runOwner >= 0)
                {
                    typeName = NumberPartType(fields[runOwner].field, chars[runStart]);
                }

                DynamicObject* part = IntlModern::CreatePlainDataObject(scriptContext);
                IntlModern::SetDataProperty(part, _u("type"), IntlModern::CreateAsciiString(typeName, scriptContext), scriptContext);
                IntlModern::SetDataProperty(part, _u("value"), IntlModern::CreateUtf16String(chars + runStart, runEnd - runStart, scriptContext), scriptContext);
                IntlModern::SetDataProperty(part, _u("unit"), unitStr, scriptContext);
                outGroupParts->DirectSetItemAt((*inOutPartCount)++, static_cast<Var>(part));
                runStart = runEnd;
            }
        }

        UListFormatter* GetLiveListFormatter(IntlDurationFormatState* state, ScriptContext* scriptContext)
        {
            UListFormatter* lf = state->IsValidState() ? state->GetListFormatter() : nullptr;
            if (lf == nullptr)
            {
                IntlModern::ThrowInvalidReceiverTypeError(scriptContext, _u("Intl.DurationFormat"));
            }
            return lf;
        }

        Var FormatDurationImpl(
            IntlDurationFormatState* state,
            const DurationRecord& record,
            bool wantParts,
            ScriptContext* scriptContext)
        {
            UListFormatter* listFormatter = GetLiveListFormatter(state, scriptContext);
            const DurationFormatConfig& config = state->GetConfig();
            JavascriptString* timeSepStr = state->GetTimeSeparator();
            const UChar* timeSepRaw = reinterpret_cast<const UChar*>(timeSepStr->GetString());
            int32_t timeSepLen = static_cast<int32_t>(timeSepStr->GetLength());
            UCharBuffer timeSepNative;
            if (!timeSepNative.Append(timeSepRaw, timeSepLen))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }

            UErrorCode status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUFormattedNumber formattedNumber(unumf_openResult(&status));
            if (ICU_FAILURE(status) || formattedNumber == nullptr)
            {
                ThrowIcuFailure(scriptContext, status);
            }

            UCharBuffer groupStrings[Unit_Count];
            JavascriptArray* groupParts[Unit_Count] = {};
            uint32 groupPartCounts[Unit_Count] = {};
            int32_t groupCount = 0;

            bool needSeparator = false;
            bool displayNegativeSign = true;

            for (int u = 0; u < Unit_Count; ++u)
            {
                UnitStyle style = config.unitStyles[u];
                UnitDisplay display = config.unitDisplays[u];
                bool combineSubseconds = false;
                bool done = false;

                if (u == Unit_Seconds || u == Unit_Milliseconds || u == Unit_Microseconds)
                {
                    if (config.unitStyles[u + 1] == UnitStyle::Fractional)
                    {
                        combineSubseconds = true;
                        done = true;
                    }
                }

                bool displayRequired = false;
                if (u == Unit_Minutes && needSeparator)
                {
                    displayRequired =
                        config.unitDisplays[Unit_Seconds] == UnitDisplay::Always ||
                        record.absUnits[Unit_Seconds] != 0 ||
                        record.absUnits[Unit_Milliseconds] != 0 ||
                        record.absUnits[Unit_Microseconds] != 0 ||
                        record.absUnits[Unit_Nanoseconds] != 0;
                }

                char decBuf[80];
                bool isNonZero = BuildDecimalValue(
                    record,
                    u,
                    combineSubseconds,
                    displayNegativeSign && record.isNegative,
                    decBuf,
                    sizeof(decBuf));

                if (isNonZero || display != UnitDisplay::Auto || displayRequired)
                {
                    bool suppressSign = !displayNegativeSign;
                    if (displayNegativeSign)
                    {
                        displayNegativeSign = false;
                    }

                    UCharBuffer skel;
                    BuildUnitSkeleton(config, u, suppressSign, combineSubseconds, skel);
                    if (skel.IsOom())
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }

                    status = U_ZERO_ERROR;
                    PlatformAgnostic::ICUHelpers::ScopedUNumberFormatter nf(
                        unumf_openForSkeletonAndLocale(skel.Data(), skel.Length(), config.icuLocale, &status));
                    if (ICU_FAILURE(status) || nf == nullptr)
                    {
                        ThrowIcuFailure(scriptContext, status);
                    }

                    unumf_formatDecimal(nf, decBuf, -1, formattedNumber, &status);
                    if (ICU_FAILURE(status))
                    {
                        ThrowIcuFailure(scriptContext, status);
                    }
                    const UFormattedValue* numValue = unumf_resultAsValue(formattedNumber, &status);
                    int32_t numLen = 0;
                    const UChar* numChars = ICU_FAILURE(status) || numValue == nullptr
                        ? nullptr
                        : ufmtval_getString(numValue, &numLen, &status);
                    if (ICU_FAILURE(status) || numChars == nullptr)
                    {
                        ThrowIcuFailure(scriptContext, status);
                    }

                    int32_t gIdx = 0;
                    if (!needSeparator)
                    {
                        gIdx = groupCount++;
                        if (wantParts)
                        {
                            groupParts[gIdx] = scriptContext->GetLibrary()->CreateArray(0);
                        }
                    }
                    else
                    {
                        gIdx = groupCount - 1;
                        if (!groupStrings[gIdx].Append(timeSepNative.Data(), timeSepNative.Length()))
                        {
                            JavascriptError::ThrowOutOfMemoryError(scriptContext);
                        }
                        if (wantParts)
                        {
                            DynamicObject* sepPart = IntlModern::CreatePlainDataObject(scriptContext);
                            IntlModern::SetDataProperty(sepPart, _u("type"), IntlModern::CreateAsciiString("literal", scriptContext), scriptContext);
                            IntlModern::SetDataProperty(sepPart, _u("value"), timeSepStr, scriptContext);
                            groupParts[gIdx]->DirectSetItemAt(groupPartCounts[gIdx]++, static_cast<Var>(sepPart));
                        }
                    }

                    if (!groupStrings[gIdx].Append(numChars, numLen))
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }
                    if (wantParts)
                    {
                        AppendNumberParts(
                            numValue,
                            numChars,
                            numLen,
                            gUnits[u].singularName,
                            groupParts[gIdx],
                            &groupPartCounts[gIdx],
                            scriptContext);
                    }

                    if (!needSeparator && (style == UnitStyle::TwoDigit || style == UnitStyle::Numeric))
                    {
                        needSeparator = true;
                    }
                }

                if (done)
                {
                    break;
                }
            }

            if (groupCount == 0)
            {
                return wantParts
                    ? static_cast<Var>(scriptContext->GetLibrary()->CreateArray(0))
                    : static_cast<Var>(scriptContext->GetLibrary()->GetEmptyString());
            }

            const UChar* listInputs[Unit_Count] = {};
            int32_t listInputLens[Unit_Count] = {};
            for (int32_t g = 0; g < groupCount; ++g)
            {
                listInputs[g] = groupStrings[g].Data();
                listInputLens[g] = groupStrings[g].Length();
            }

            status = U_ZERO_ERROR;
            PlatformAgnostic::ICUHelpers::ScopedUFormattedList formattedList(ulistfmt_openResult(&status));
            if (ICU_FAILURE(status) || formattedList == nullptr)
            {
                ThrowIcuFailure(scriptContext, status);
            }
            ulistfmt_formatStringsToResult(listFormatter, listInputs, listInputLens, groupCount, formattedList, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
            const UFormattedValue* listValue = ulistfmt_resultAsValue(formattedList, &status);
            int32_t listLen = 0;
            const UChar* listChars = ICU_FAILURE(status) || listValue == nullptr
                ? nullptr
                : ufmtval_getString(listValue, &listLen, &status);
            if (ICU_FAILURE(status) || listChars == nullptr)
            {
                ThrowIcuFailure(scriptContext, status);
            }

            if (!wantParts)
            {
                return IntlModern::CreateUtf16String(listChars, listLen, scriptContext);
            }

            PlatformAgnostic::ICUHelpers::ScopedUConstrainedFieldPosition pos(ucfpos_open(&status));
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
            ucfpos_constrainCategory(pos, UFIELD_CATEGORY_LIST, &status);
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }

            JavascriptArray* outParts = scriptContext->GetLibrary()->CreateArray(0);
            uint32 outCount = 0;
            int32_t cursor = 0;
            int32_t nextGroup = 0;

            auto emitLiteral = [&](int32_t start, int32_t limit)
            {
                if (limit <= start)
                {
                    return;
                }
                DynamicObject* part = IntlModern::CreatePlainDataObject(scriptContext);
                IntlModern::SetDataProperty(part, _u("type"), IntlModern::CreateAsciiString("literal", scriptContext), scriptContext);
                IntlModern::SetDataProperty(part, _u("value"), IntlModern::CreateUtf16String(listChars + start, limit - start, scriptContext), scriptContext);
                outParts->DirectSetItemAt(outCount++, static_cast<Var>(part));
            };

            while (ufmtval_nextPosition(listValue, pos, &status))
            {
                int32_t field = ucfpos_getField(pos, &status);
                int32_t start = 0;
                int32_t limit = 0;
                ucfpos_getIndexes(pos, &start, &limit, &status);
                if (ICU_FAILURE(status))
                {
                    ThrowIcuFailure(scriptContext, status);
                }
                if (start < cursor || limit < start || limit > listLen)
                {
                    continue;
                }
                if (start > cursor)
                {
                    emitLiteral(cursor, start);
                }
                if (field == ULISTFMT_ELEMENT_FIELD && nextGroup < groupCount)
                {
                    uint32 len = groupPartCounts[nextGroup];
                    for (uint32 k = 0; k < len; ++k)
                    {
                        outParts->DirectSetItemAt(outCount++, groupParts[nextGroup]->DirectGetItem(k));
                    }
                    ++nextGroup;
                }
                else
                {
                    emitLiteral(start, limit);
                }
                cursor = limit;
            }
            if (ICU_FAILURE(status))
            {
                ThrowIcuFailure(scriptContext, status);
            }
            if (cursor < listLen)
            {
                emitLiteral(cursor, listLen);
            }

            return outParts;
        }
    } // namespace

    void IntlDurationFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("DurationFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.DurationFormat"),
            nullptr,
            &ctor);

        library->SetIntlDurationFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlDurationFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DurationFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DurationFormat"));
        }

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefined;
        Var optionsArg = args.Info.Count > 2 ? args[2] : undefined;

        // 1. OrdinaryCreateFromConstructor (runs newTarget.prototype).
        DynamicObject* durationFormat = IntlModern::OrdinaryCreateFromConstructor(
            newTarget, scriptContext, &JavascriptLibrary::GetIntlDurationFormatPrototype);

        // 2-3. CanonicalizeLocaleList, GetOptionsObject.
        JavascriptArray* requestedLocales = IntlModern::CanonicalizeLocaleList(locales, scriptContext);
        RecyclableObject* options = IntlModern::GetOptionsObject(optionsArg, scriptContext);

        // 4. Option reads in exact ECMA-402 spec order.
        JavascriptString* matcher = IntlModern::GetStringOption(
            options, _u("localeMatcher"), gLocaleMatcherValues, _countof(gLocaleMatcherValues), _u("best fit"), scriptContext);

        JavascriptString* nuOptionStr = IntlModern::GetStringOption(
            options, _u("numberingSystem"), nullptr, 0, nullptr, scriptContext);
        char optionNu[32] = { 0 };
        bool hasOptionNu = false;
        if (nuOptionStr != nullptr)
        {
            IntlModern::ScopedAsciiString ascii;
            if (!ascii.Init(nuOptionStr))
            {
                if (ascii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid numberingSystem"));
            }
            if (!IntlModern::IsUnicodeTypeNonterminal(ascii.Data(), ascii.Length()))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: invalid numberingSystem"));
            }
            bool allLower = ascii.Length() < sizeof(optionNu);
            for (size_t i = 0; allLower && i < ascii.Length(); ++i)
            {
                char ch = ascii.Data()[i];
                if (ch >= 'A' && ch <= 'Z')
                {
                    allLower = false;
                }
                else
                {
                    optionNu[i] = ch;
                }
            }
            if (allLower)
            {
                optionNu[ascii.Length()] = '\0';
                hasOptionNu = true;
            }
        }

        JavascriptString* styleStr = IntlModern::GetStringOption(
            options, _u("style"), gStyleValues, _countof(gStyleValues), _u("short"), scriptContext);
        DurationStyle baseStyle = ParseDurationStyle(styleStr);

        DurationFormatConfig config = {};
        config.style = baseStyle;

        UnitStyle prevStyle = UnitStyle::Long;
        bool hasPrevStyle = false;

        for (int i = 0; i < Unit_Count; ++i)
        {
            JavascriptString* unitOpt = IntlModern::GetStringOption(
                options, gUnits[i].name, gUnits[i].allowedStyles, gUnits[i].allowedCount, nullptr, scriptContext);

            UnitDisplay displayDefault = UnitDisplay::Always;
            UnitStyle unitStyle = UnitStyle::Short;

            if (unitOpt == nullptr)
            {
                if (baseStyle == DurationStyle::Digital)
                {
                    if (i < Unit_Hours || i > Unit_Seconds)
                    {
                        displayDefault = UnitDisplay::Auto;
                    }
                    unitStyle = gUnits[i].digitalBase;
                }
                else if (hasPrevStyle && IsNumericLike(prevStyle))
                {
                    if (i != Unit_Minutes && i != Unit_Seconds)
                    {
                        displayDefault = UnitDisplay::Auto;
                    }
                    unitStyle = UnitStyle::Numeric;
                }
                else
                {
                    displayDefault = UnitDisplay::Auto;
                    unitStyle = BaseStyleToUnitStyle(baseStyle);
                }
            }
            else
            {
                unitStyle = ParseUnitStyle(unitOpt);
            }

            if (unitStyle == UnitStyle::Numeric && i >= Unit_Milliseconds)
            {
                unitStyle = UnitStyle::Fractional;
                displayDefault = UnitDisplay::Auto;
            }

            JavascriptString* displayOpt = IntlModern::GetStringOption(
                options, gUnits[i].displayName, gDisplayValues, _countof(gDisplayValues), UnitDisplayToWide(displayDefault), scriptContext);
            UnitDisplay unitDisplay = ParseUnitDisplay(displayOpt);

            if (unitDisplay == UnitDisplay::Always && unitStyle == UnitStyle::Fractional)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: display cannot be always with numeric sub-second unit"));
            }
            if (hasPrevStyle && prevStyle == UnitStyle::Fractional && unitStyle != UnitStyle::Fractional)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: conflicting unit styles"));
            }
            if (hasPrevStyle && (prevStyle == UnitStyle::Numeric || prevStyle == UnitStyle::TwoDigit))
            {
                if (!IsNumericLike(unitStyle))
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: conflicting unit styles"));
                }
                if (i == Unit_Minutes || i == Unit_Seconds)
                {
                    unitStyle = UnitStyle::TwoDigit;
                }
            }

            config.unitStyles[i] = unitStyle;
            config.unitDisplays[i] = unitDisplay;
            prevStyle = unitStyle;
            hasPrevStyle = true;
        }

        config.fractionalDigits = -1;
        if (options != nullptr)
        {
            PropertyId fdPropId = scriptContext->GetOrAddPropertyIdTracked(_u("fractionalDigits"), 16);
            Var fdVal = JavascriptOperators::GetProperty(options, fdPropId, scriptContext);
            if (!JavascriptOperators::IsUndefined(fdVal))
            {
                double d = JavascriptConversion::ToNumber(fdVal, scriptContext);
                if (NumberUtilities::IsNan(d) || d < 0.0 || d > 9.0)
                {
                    IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.DurationFormat: fractionalDigits out of range"));
                }
                config.fractionalDigits = static_cast<int8>(floor(d));
            }
        }

        // ResolveLocale and reconcile numberingSystem option vs -u-nu- extension.
        static const char16* const kNuKey[] = { _u("nu") };
        IntlModern::ResolvedLocale resolved = IntlModern::ResolveLocale(
            IntlModern::IntlModernKind::DurationFormat,
            requestedLocales,
            matcher->GetSz(),
            kNuKey,
            1,
            nullptr,
            scriptContext);

        IntlModern::ScopedAsciiString dataAscii;
        if (!dataAscii.Init(resolved.dataLocale))
        {
            JavascriptError::ThrowOutOfMemoryError(scriptContext);
        }

        char requestedNu[32] = { 0 };
        bool hasRequestedNu = FindRequestedNu(requestedLocales, dataAscii.Data(), dataAscii.Length(), requestedNu, sizeof(requestedNu));
        bool requestedNuSupported = hasRequestedNu && IntlModern::IsSupportedNumberingSystem(requestedNu, strlen(requestedNu));
        bool optionNuSupported = hasOptionNu && IntlModern::IsSupportedNumberingSystem(optionNu, strlen(optionNu));

        bool keepExtension = false;
        if (optionNuSupported)
        {
            js_memcpy_s(config.chosenNu, sizeof(config.chosenNu), optionNu, strlen(optionNu) + 1);
            keepExtension = requestedNuSupported && strcmp(requestedNu, optionNu) == 0;
        }
        else if (requestedNuSupported)
        {
            js_memcpy_s(config.chosenNu, sizeof(config.chosenNu), requestedNu, strlen(requestedNu) + 1);
            keepExtension = true;
        }

        JavascriptString* numberingSystem = resolved.numberingSystem;
        if (config.chosenNu[0] != '\0')
        {
            numberingSystem = IntlModern::CreateAsciiString(config.chosenNu, scriptContext);
        }
        else
        {
            IntlModern::ScopedAsciiString nuAscii;
            if (numberingSystem == nullptr || !nuAscii.Init(numberingSystem) || nuAscii.Length() >= sizeof(config.chosenNu))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            js_memcpy_s(config.chosenNu, sizeof(config.chosenNu), nuAscii.Data(), nuAscii.Length() + 1);
        }

        char localeTag[ULOC_FULLNAME_CAPACITY + 48] = { 0 };
        JavascriptString* locale = resolved.dataLocale;
        if (keepExtension)
        {
            snprintf(localeTag, sizeof(localeTag), "%s-u-nu-%s", dataAscii.Data(), config.chosenNu);
            locale = IntlModern::CreateAsciiString(localeTag, scriptContext);
        }

        char icuBaseLocale[ULOC_FULLNAME_CAPACITY] = { 0 };
        ToIcuLocaleId(dataAscii.Data(), icuBaseLocale, static_cast<int32_t>(sizeof(icuBaseLocale)), scriptContext);

        snprintf(localeTag, sizeof(localeTag), "%s-u-nu-%s", dataAscii.Data(), config.chosenNu);
        ToIcuLocaleId(localeTag, config.icuLocale, static_cast<int32_t>(sizeof(config.icuLocale)), scriptContext);

        JavascriptString* timeSeparator = ResolveTimeSeparator(icuBaseLocale, config.chosenNu, scriptContext);

        UListFormatterWidth icuWidth = ULISTFMT_WIDTH_SHORT;
        if (baseStyle == DurationStyle::Long)
        {
            icuWidth = ULISTFMT_WIDTH_WIDE;
        }
        else if (baseStyle == DurationStyle::Narrow)
        {
            icuWidth = ULISTFMT_WIDTH_NARROW;
        }

        UErrorCode status = U_ZERO_ERROR;
        UListFormatter* listFormatter = ulistfmt_openForType(config.icuLocale, ULISTFMT_TYPE_UNITS, icuWidth, &status);
        if (ICU_FAILURE(status) || listFormatter == nullptr)
        {
            if (listFormatter != nullptr)
            {
                ulistfmt_close(listFormatter);
            }
            ThrowIcuFailure(scriptContext, status);
        }

        // Ownership of `listFormatter` transfers to the state; close it explicitly if state allocation throws.
        IntlDurationFormatState* state = nullptr;
        try
        {
            state = IntlDurationFormatState::New(
                scriptContext->GetRecycler(),
                listFormatter,
                locale,
                numberingSystem,
                styleStr,
                timeSeparator,
                config);
        }
        catch (...)
        {
            ulistfmt_close(listFormatter);
            throw;
        }

        if (!IntlModern::SetState(durationFormat, state))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotExtensible, _u("Intl.DurationFormat"));
        }
        return durationFormat;
    }

    Var IntlDurationFormat::EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.DurationFormat.supportedLocalesOf"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefined;
        Var options = args.Info.Count > 2 ? args[2] : undefined;
        return IntlModern::SupportedLocales(IntlModern::IntlModernKind::DurationFormat, locales, options, scriptContext);
    }

    Var IntlDurationFormat::EntryFormat(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.DurationFormat.prototype.format"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        IntlDurationFormatState* state = IntlModern::RequireState<IntlDurationFormatState>(
            args.Info.Count > 0 ? args[0] : undefined,
            IntlModern::IntlModernKind::DurationFormat,
            scriptContext,
            _u("Intl.DurationFormat.prototype.format"));

        Var durationArg = args.Info.Count > 1 ? args[1] : undefined;
        DurationRecord record = ToDurationRecord(durationArg, scriptContext);
        return FormatDurationImpl(state, record, false, scriptContext);
    }

    Var IntlDurationFormat::EntryFormatToParts(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.DurationFormat.prototype.formatToParts"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        IntlDurationFormatState* state = IntlModern::RequireState<IntlDurationFormatState>(
            args.Info.Count > 0 ? args[0] : undefined,
            IntlModern::IntlModernKind::DurationFormat,
            scriptContext,
            _u("Intl.DurationFormat.prototype.formatToParts"));

        Var durationArg = args.Info.Count > 1 ? args[1] : undefined;
        DurationRecord record = ToDurationRecord(durationArg, scriptContext);
        return FormatDurationImpl(state, record, true, scriptContext);
    }

    Var IntlDurationFormat::EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.DurationFormat.prototype.resolvedOptions"));
        Assert(!(callInfo.Flags & CallFlags_New));

        IntlDurationFormatState* state = IntlModern::RequireState<IntlDurationFormatState>(
            args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(),
            IntlModern::IntlModernKind::DurationFormat,
            scriptContext,
            _u("Intl.DurationFormat.prototype.resolvedOptions"));

        const DurationFormatConfig& config = state->GetConfig();
        DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
        IntlModern::SetDataProperty(result, _u("locale"), state->GetLocale(), scriptContext);
        IntlModern::SetDataProperty(result, _u("numberingSystem"), state->GetNumberingSystem(), scriptContext);
        IntlModern::SetDataProperty(result, _u("style"), state->GetStyleStr(), scriptContext);

        for (int i = 0; i < Unit_Count; ++i)
        {
            IntlModern::SetDataProperty(
                result,
                gUnits[i].name,
                IntlModern::CreateAsciiString(UnitStyleToAscii(config.unitStyles[i]), scriptContext),
                scriptContext);
            IntlModern::SetDataProperty(
                result,
                gUnits[i].displayName,
                IntlModern::CreateAsciiString(UnitDisplayToAscii(config.unitDisplays[i]), scriptContext),
                scriptContext);
        }

        if (config.fractionalDigits >= 0)
        {
            IntlModern::SetDataProperty(
                result,
                _u("fractionalDigits"),
                TaggedInt::ToVarUnchecked(config.fractionalDigits),
                scriptContext);
        }

        return result;
    }
} // namespace Js

#endif // INTL_ICU
