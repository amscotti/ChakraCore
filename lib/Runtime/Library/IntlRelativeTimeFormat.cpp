//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlRelativeTimeFormat.h"

#ifdef INTL_ICU
#include "unicode/unumberformatter.h"

// Ownership record (doc/memory-management.md section 3):
//
//   Resource:   URelativeDateTimeFormatter* from ureldatefmt_open (ICU heap; the default
//               UNumberFormat is created and owned by ICU, nfToAdopt is null).
//   Owner:      IntlRelativeTimeFormatState (Recycler finalized object, WithBarrier), published on
//               the Intl.RelativeTimeFormat instance through IntlModern::SetState (HiddenObject).
//   Release:    ureldatefmt_close in Dispose(); idempotent (pointer nulled, magic invalidated)
//               because the Recycler may call Finalize and Dispose for one object.
//   Partial failure: the handle is closed explicitly if allocating the state throws; afterwards
//               the state owns it and any later throw leaves an unpublished, finalizable state.
//   Retained JS pointers: locale/style/numeric/numberingSystem, each in a Field() barrier slot.
//   Reentrancy: format()/formatToParts() run ToNumber(value) then ToString(unit) (user code).
//               Only after both complete is the formatter re-read from the state; ICU calls never
//               run JS, and ICU result buffers are copied into JS strings before closeResult.
//   Thread/context: single script thread; ICU objects used only inside synchronous entry points.

namespace Js
{
    namespace
    {
        class IntlRelativeTimeFormatState : public IntlModern::IntlModernState
        {
        private:
            FieldNoBarrier(URelativeDateTimeFormatter*) formatter;
            Field(JavascriptString*) locale;
            Field(JavascriptString*) style;
            Field(JavascriptString*) numeric;
            Field(JavascriptString*) numberingSystem;

        public:
            IntlRelativeTimeFormatState(
                URelativeDateTimeFormatter* formatter,
                JavascriptString* locale,
                JavascriptString* style,
                JavascriptString* numeric,
                JavascriptString* numberingSystem) :
                IntlModern::IntlModernState(IntlModern::IntlModernKind::RelativeTimeFormat),
                formatter(formatter),
                locale(locale),
                style(style),
                numeric(numeric),
                numberingSystem(numberingSystem)
            {
            }

            static IntlRelativeTimeFormatState* New(
                Recycler* recycler,
                URelativeDateTimeFormatter* formatter,
                JavascriptString* locale,
                JavascriptString* style,
                JavascriptString* numeric,
                JavascriptString* numberingSystem)
            {
                return RecyclerNewWithBarrierFinalized(
                    recycler, IntlRelativeTimeFormatState, formatter, locale, style, numeric, numberingSystem);
            }

            URelativeDateTimeFormatter* GetFormatter() const { return formatter; }
            JavascriptString* GetLocale() const { return locale; }
            JavascriptString* GetStyle() const { return style; }
            JavascriptString* GetNumeric() const { return numeric; }
            JavascriptString* GetNumberingSystem() const { return numberingSystem; }

            virtual JavascriptString* GetLocaleTag() const override { return locale; }

            virtual void Dispose(bool isShutdown) override
            {
                if (formatter != nullptr)
                {
                    ureldatefmt_close(formatter);
                    formatter = nullptr;
                }
                InvalidateMagic();
            }
        };

        static const char16* const gLocaleMatcherValues[] = { _u("lookup"), _u("best fit") };
        static const char16* const gStyleValues[] = { _u("long"), _u("short"), _u("narrow") };
        static const char16* const gNumericValues[] = { _u("always"), _u("auto") };

        void ThrowIcuFailure(ScriptContext* scriptContext, UErrorCode status)
        {
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.RelativeTimeFormat: internal ICU failure"));
        }

        bool StringEquals(JavascriptString* str, const char16* literal)
        {
            return wcscmp(str->GetSz(), literal) == 0;
        }

        // Extracts the (single-subtag) value of the "nu" keyword of the unicode extension of a
        // canonical BCP 47 tag. The tag is ASCII with lowercase extension subtags.
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
                ++pos; // skip '-'
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
                // Collect the key's type subtags (length 3-8).
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

        // Base of a tag (everything before the first singleton subtag).
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

        // Finds the nu extension value of the requested locale that ResolveLocale matched, i.e. the
        // first requested tag whose extension-free base has dataLocale as a prefix.
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

        bool TryGetUnit(JavascriptString* unit, URelativeDateTimeUnit* outUnit, const char** outName)
        {
            static const struct
            {
                const char16* singular;
                const char16* plural;
                const char* name;
                URelativeDateTimeUnit icu;
            } units[] = {
                { _u("second"), _u("seconds"), "second", UDAT_REL_UNIT_SECOND },
                { _u("minute"), _u("minutes"), "minute", UDAT_REL_UNIT_MINUTE },
                { _u("hour"), _u("hours"), "hour", UDAT_REL_UNIT_HOUR },
                { _u("day"), _u("days"), "day", UDAT_REL_UNIT_DAY },
                { _u("week"), _u("weeks"), "week", UDAT_REL_UNIT_WEEK },
                { _u("month"), _u("months"), "month", UDAT_REL_UNIT_MONTH },
                { _u("quarter"), _u("quarters"), "quarter", UDAT_REL_UNIT_QUARTER },
                { _u("year"), _u("years"), "year", UDAT_REL_UNIT_YEAR },
            };
            const char16* sz = unit->GetSz();
            for (size_t i = 0; i < _countof(units); ++i)
            {
                if (wcscmp(sz, units[i].singular) == 0 || wcscmp(sz, units[i].plural) == 0)
                {
                    *outUnit = units[i].icu;
                    *outName = units[i].name;
                    return true;
                }
            }
            return false;
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

        struct NumberField
        {
            int32_t field;
            int32_t start;
            int32_t limit;
        };

        // Native scratch holder (not a GC object).
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
            T* Data() const { return data; }
            T& operator[](size_t i) const { return data[i]; }
        };

        // Validates the receiver, coerces value and unit (user code), then re-reads the formatter and
        // formats. Returns the owned ICU result via outResult; the caller closes it.
        struct FormatInputs
        {
            IntlRelativeTimeFormatState* state;
            double value;
            URelativeDateTimeUnit icuUnit;
            const char* unitName;
        };

        FormatInputs CoerceInputs(Arguments& args, ScriptContext* scriptContext, const char16* methodName)
        {
            Var undefined = scriptContext->GetLibrary()->GetUndefined();
            IntlRelativeTimeFormatState* state = IntlModern::RequireState<IntlRelativeTimeFormatState>(
                args.Info.Count > 0 ? args[0] : undefined,
                IntlModern::IntlModernKind::RelativeTimeFormat,
                scriptContext,
                methodName);

            Var valueArg = args.Info.Count > 1 ? args[1] : undefined;
            Var unitArg = args.Info.Count > 2 ? args[2] : undefined;

            // User-visible coercions, in spec order.
            double value = JavascriptConversion::ToNumber(valueArg, scriptContext);
            JavascriptString* unitString = JavascriptConversion::ToString(unitArg, scriptContext);

            if (!NumberUtilities::IsFinite(value))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.RelativeTimeFormat: value must be a finite number"));
            }

            FormatInputs inputs;
            inputs.state = state;
            inputs.value = value;
            inputs.icuUnit = UDAT_REL_UNIT_SECOND;
            inputs.unitName = nullptr;
            if (!TryGetUnit(unitString, &inputs.icuUnit, &inputs.unitName))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.RelativeTimeFormat: invalid unit"));
            }
            return inputs;
        }

        UFormattedRelativeDateTime* FormatRelative(
            const FormatInputs& inputs,
            const UChar** outChars,
            int32_t* outLen,
            ScriptContext* scriptContext)
        {
            IntlRelativeTimeFormatState* state = inputs.state;
            URelativeDateTimeFormatter* formatter = state->IsValidState() ? state->GetFormatter() : nullptr;
            if (formatter == nullptr)
            {
                IntlModern::ThrowInvalidReceiverTypeError(scriptContext, _u("Intl.RelativeTimeFormat"));
            }

            UErrorCode status = U_ZERO_ERROR;
            UFormattedRelativeDateTime* result = ureldatefmt_openResult(&status);
            if (ICU_FAILURE(status) || result == nullptr)
            {
                ThrowIcuFailure(scriptContext, status);
            }
            if (StringEquals(state->GetNumeric(), _u("auto")))
            {
                ureldatefmt_formatToResult(formatter, inputs.value, inputs.icuUnit, result, &status);
            }
            else
            {
                ureldatefmt_formatNumericToResult(formatter, inputs.value, inputs.icuUnit, result, &status);
            }
            if (ICU_FAILURE(status))
            {
                ureldatefmt_closeResult(result);
                ThrowIcuFailure(scriptContext, status);
            }
            const UFormattedValue* value = ureldatefmt_resultAsValue(result, &status);
            const UChar* chars = ICU_FAILURE(status) ? nullptr : ufmtval_getString(value, outLen, &status);
            if (ICU_FAILURE(status) || chars == nullptr)
            {
                ureldatefmt_closeResult(result);
                ThrowIcuFailure(scriptContext, status);
            }
            *outChars = chars;
            return result;
        }

        // CLDR's minimumGroupingDigits is honored by ICU's NumberFormatter (Intl.NumberFormat) but not
        // by a classic UNumberFormat, so "1000" would otherwise format as "1 000" for pl/es/pt-PT.
        // Probe the locale's value through the public number formatter: the first of 1000, 10000,
        // 100000 that gets a grouping separator gives the minimum. Returns 0 if undetermined.
        int32_t ProbeMinimumGroupingDigits(const char* icuLocale)
        {
            typedef PlatformAgnostic::ICUHelpers::ScopedICUObject<UNumberFormatter*, unumf_close> ScopedUNumberFormatter;
            typedef PlatformAgnostic::ICUHelpers::ScopedICUObject<UFormattedNumber*, unumf_closeResult> ScopedUFormattedNumber;

            static const UChar emptySkeleton[] = { 0 };
            UErrorCode status = U_ZERO_ERROR;
            ScopedUNumberFormatter numberFormatter(unumf_openForSkeletonAndLocale(emptySkeleton, 0, icuLocale, &status));
            ScopedUFormattedNumber number(unumf_openResult(&status));
            PlatformAgnostic::ICUHelpers::ScopedUConstrainedFieldPosition pos(ucfpos_open(&status));
            if (ICU_FAILURE(status))
            {
                return 0;
            }

            static const int64_t probes[] = { 1000, 10000, 100000 };
            for (int32_t i = 0; i < static_cast<int32_t>(_countof(probes)); ++i)
            {
                unumf_formatInt(numberFormatter, probes[i], number, &status);
                const UFormattedValue* value = ICU_FAILURE(status) ? nullptr : unumf_resultAsValue(number, &status);
                if (ICU_FAILURE(status) || value == nullptr)
                {
                    return 0;
                }
                ucfpos_reset(pos, &status);
                ucfpos_constrainField(pos, UFIELD_CATEGORY_NUMBER, UNUM_GROUPING_SEPARATOR_FIELD, &status);
                bool grouped = !ICU_FAILURE(status) && ufmtval_nextPosition(value, pos, &status);
                if (ICU_FAILURE(status))
                {
                    return 0;
                }
                if (grouped)
                {
                    return i + 1;
                }
            }
            return 0;
        }

        // Opens the formatter with an explicit decimal UNumberFormat carrying the locale's
        // minimum grouping digits. Once status is validated non-error, ICU's
        // RelativeDateTimeFormatter::init deletes nfToAdopt on internal failures while
        // early wrapper OOM does not, so caller-side unum_close would double-free.
        URelativeDateTimeFormatter* OpenFormatter(
            const char* icuLocale,
            UDateRelativeDateTimeFormatterStyle style,
            UErrorCode* status)
        {
            UNumberFormat* numberFormat = unum_open(UNUM_DECIMAL, nullptr, 0, icuLocale, nullptr, status);
            if (ICU_FAILURE(*status) || numberFormat == nullptr)
            {
                if (numberFormat != nullptr)
                {
                    unum_close(numberFormat);
                }
                return nullptr;
            }
            *status = U_ZERO_ERROR;
            int32_t minGrouping = ProbeMinimumGroupingDigits(icuLocale);
            if (minGrouping > 1)
            {
                unum_setAttribute(numberFormat, UNUM_MINIMUM_GROUPING_DIGITS, minGrouping);
            }
            return ureldatefmt_open(icuLocale, numberFormat, style, UDISPCTX_CAPITALIZATION_NONE, status);
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
    } // namespace

    void IntlRelativeTimeFormat::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("RelativeTimeFormat"),
            &EntryInfo::NewInstance,
            0,
            _u("Intl.RelativeTimeFormat"),
            nullptr,
            &ctor);

        library->SetIntlRelativeTimeFormatPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::format, &EntryInfo::Format, 2);
        library->AddFunctionToLibraryObject(proto, PropertyIds::formatToParts, &EntryInfo::FormatToParts, 2);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlRelativeTimeFormat::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.RelativeTimeFormat"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.RelativeTimeFormat"));
        }

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefined;
        Var optionsArg = args.Info.Count > 2 ? args[2] : undefined;

        DynamicObject* rtf = IntlModern::OrdinaryCreateFromConstructor(
            newTarget, scriptContext, &JavascriptLibrary::GetIntlRelativeTimeFormatPrototype);

        JavascriptArray* requestedLocales = IntlModern::CanonicalizeLocaleList(locales, scriptContext);
        RecyclableObject* options = IntlModern::CoerceOptionsToObject(optionsArg, scriptContext);

        // Option reads in spec order: localeMatcher, numberingSystem, style, numeric.
        JavascriptString* matcher = IntlModern::GetStringOption(
            options, _u("localeMatcher"), gLocaleMatcherValues, _countof(gLocaleMatcherValues), _u("best fit"), scriptContext);

        JavascriptString* numberingSystemOption = IntlModern::GetStringOption(
            options, _u("numberingSystem"), nullptr, 0, nullptr, scriptContext);
        char optionNu[32] = { 0 };
        bool hasOptionNu = false;
        if (numberingSystemOption != nullptr)
        {
            IntlModern::ScopedAsciiString ascii;
            if (!ascii.Init(numberingSystemOption))
            {
                if (ascii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.RelativeTimeFormat: invalid numberingSystem"));
            }
            if (!IntlModern::IsUnicodeTypeNonterminal(ascii.Data(), ascii.Length()))
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Intl.RelativeTimeFormat: invalid numberingSystem"));
            }
            if (ascii.Length() < sizeof(optionNu))
            {
                for (size_t i = 0; i < ascii.Length(); ++i)
                {
                    optionNu[i] = IntlModern::ToAsciiLower(ascii.Data()[i]);
                }
                optionNu[ascii.Length()] = '\0';
                hasOptionNu = true;
            }
            // else: structurally valid but longer than any numbering system name -> unsupported.
        }

        JavascriptString* style = IntlModern::GetStringOption(
            options, _u("style"), gStyleValues, _countof(gStyleValues), _u("long"), scriptContext);
        JavascriptString* numeric = IntlModern::GetStringOption(
            options, _u("numeric"), gNumericValues, _countof(gNumericValues), _u("always"), scriptContext);

        // ResolveLocale with relevantExtensionKeys = ["nu"]. The shared helper supplies the matched
        // data locale and the default / extension numbering system; the option-vs-extension
        // reconciliation below follows ECMA-402 ResolveLocale (the extension is reflected in the
        // resolved locale only if it supplies the final value).
        static const char16* const nuKey[] = { _u("nu") };
        IntlModern::ResolvedLocale resolved = IntlModern::ResolveLocale(
            IntlModern::IntlModernKind::RelativeTimeFormat,
            requestedLocales,
            matcher->GetSz(),
            nuKey,
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

        char chosenNu[32] = { 0 };
        bool keepExtension = false;
        if (optionNuSupported)
        {
            js_memcpy_s(chosenNu, sizeof(chosenNu), optionNu, strlen(optionNu) + 1);
            keepExtension = requestedNuSupported && strcmp(requestedNu, optionNu) == 0;
        }
        else if (requestedNuSupported)
        {
            js_memcpy_s(chosenNu, sizeof(chosenNu), requestedNu, strlen(requestedNu) + 1);
            keepExtension = true;
        }

        JavascriptString* numberingSystem = resolved.numberingSystem;
        if (chosenNu[0] != '\0')
        {
            numberingSystem = IntlModern::CreateAsciiString(chosenNu, scriptContext);
        }
        else
        {
            IntlModern::ScopedAsciiString nuAscii;
            if (numberingSystem == nullptr || !nuAscii.Init(numberingSystem) || nuAscii.Length() >= sizeof(chosenNu))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            js_memcpy_s(chosenNu, sizeof(chosenNu), nuAscii.Data(), nuAscii.Length() + 1);
        }

        char localeTag[ULOC_FULLNAME_CAPACITY + 48] = { 0 };
        JavascriptString* locale = resolved.dataLocale;
        if (keepExtension)
        {
            snprintf(localeTag, sizeof(localeTag), "%s-u-nu-%s", dataAscii.Data(), chosenNu);
            locale = IntlModern::CreateAsciiString(localeTag, scriptContext);
        }

        // ICU always receives an explicit numbering system so the formatter agrees with
        // resolvedOptions().numberingSystem.
        snprintf(localeTag, sizeof(localeTag), "%s-u-nu-%s", dataAscii.Data(), chosenNu);
        char icuLocale[ULOC_FULLNAME_CAPACITY] = { 0 };
        ToIcuLocaleId(localeTag, icuLocale, static_cast<int32_t>(sizeof(icuLocale)), scriptContext);

        UDateRelativeDateTimeFormatterStyle icuStyle = UDAT_STYLE_LONG;
        if (StringEquals(style, _u("short")))
        {
            icuStyle = UDAT_STYLE_SHORT;
        }
        else if (StringEquals(style, _u("narrow")))
        {
            icuStyle = UDAT_STYLE_NARROW;
        }

        UErrorCode status = U_ZERO_ERROR;
        URelativeDateTimeFormatter* formatter = OpenFormatter(icuLocale, icuStyle, &status);
        if (ICU_FAILURE(status) || formatter == nullptr)
        {
            if (formatter != nullptr)
            {
                ureldatefmt_close(formatter);
            }
            ThrowIcuFailure(scriptContext, status);
        }

        // Ownership of `formatter` transfers to the state; close it explicitly if the state
        // allocation throws.
        IntlRelativeTimeFormatState* state = nullptr;
        try
        {
            state = IntlRelativeTimeFormatState::New(
                scriptContext->GetRecycler(), formatter, locale, style, numeric, numberingSystem);
        }
        catch (...)
        {
            ureldatefmt_close(formatter);
            throw;
        }

        if (!IntlModern::SetState(rtf, state))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DefineProperty_NotExtensible, _u("Intl.RelativeTimeFormat"));
        }
        return rtf;
    }

    Var IntlRelativeTimeFormat::EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.RelativeTimeFormat.supportedLocalesOf"));
        Assert(!(callInfo.Flags & CallFlags_New));

        Var undefined = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefined;
        Var options = args.Info.Count > 2 ? args[2] : undefined;
        return IntlModern::SupportedLocales(IntlModern::IntlModernKind::RelativeTimeFormat, locales, options, scriptContext);
    }

    Var IntlRelativeTimeFormat::EntryFormat(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.RelativeTimeFormat.prototype.format"));
        Assert(!(callInfo.Flags & CallFlags_New));

        FormatInputs inputs = CoerceInputs(args, scriptContext, _u("Intl.RelativeTimeFormat.prototype.format"));

        const UChar* chars = nullptr;
        int32_t len = 0;
        UFormattedRelativeDateTime* result = FormatRelative(inputs, &chars, &len, scriptContext);
        PlatformAgnostic::ICUHelpers::ScopedUFormattedRelativeDateTime scopedResult(result);
        return IntlModern::CreateUtf16String(chars, len, scriptContext);
    }

    Var IntlRelativeTimeFormat::EntryFormatToParts(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.RelativeTimeFormat.prototype.formatToParts"));
        Assert(!(callInfo.Flags & CallFlags_New));

        FormatInputs inputs = CoerceInputs(args, scriptContext, _u("Intl.RelativeTimeFormat.prototype.formatToParts"));

        const UChar* chars = nullptr;
        int32_t len = 0;
        UFormattedRelativeDateTime* result = FormatRelative(inputs, &chars, &len, scriptContext);
        PlatformAgnostic::ICUHelpers::ScopedUFormattedRelativeDateTime scopedResult(result);

        UErrorCode status = U_ZERO_ERROR;
        const UFormattedValue* value = ureldatefmt_resultAsValue(result, &status);
        PlatformAgnostic::ICUHelpers::ScopedUConstrainedFieldPosition pos(ucfpos_open(&status));
        if (ICU_FAILURE(status) || value == nullptr)
        {
            ThrowIcuFailure(scriptContext, status);
        }
        ucfpos_constrainCategory(pos, UFIELD_CATEGORY_NUMBER, &status);
        if (ICU_FAILURE(status))
        {
            ThrowIcuFailure(scriptContext, status);
        }

        // Pass 1: count the number fields. Pass 2: record them.
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

        ScopedNativeArray<NumberField> fields;
        ScopedNativeArray<int32_t> owner;
        if (!fields.Init(fieldCount) || !owner.Init(static_cast<size_t>(len)))
        {
            JavascriptError::ThrowOutOfMemoryError(scriptContext);
        }
        size_t recorded = 0;
        while (recorded < fieldCount && ufmtval_nextPosition(value, pos, &status))
        {
            NumberField f;
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

        // The innermost number field owns each character (an integer field contains its grouping
        // separators); characters outside every number field are literal text without a unit.
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

        JavascriptLibrary* library = scriptContext->GetLibrary();
        JavascriptArray* parts = library->CreateArray(0);
        uint32 partCount = 0;
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
            if (runOwner >= 0 && strcmp(typeName, "literal") != 0)
            {
                IntlModern::SetDataProperty(part, _u("unit"), IntlModern::CreateAsciiString(inputs.unitName, scriptContext), scriptContext);
            }
            parts->DirectSetItemAt(partCount++, static_cast<Var>(part));
            runStart = runEnd;
        }
        return parts;
    }

    Var IntlRelativeTimeFormat::EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        AUTO_TAG_NATIVE_LIBRARY_ENTRY(function, callInfo, _u("Intl.RelativeTimeFormat.prototype.resolvedOptions"));
        Assert(!(callInfo.Flags & CallFlags_New));

        IntlRelativeTimeFormatState* state = IntlModern::RequireState<IntlRelativeTimeFormatState>(
            args.Info.Count > 0 ? args[0] : scriptContext->GetLibrary()->GetUndefined(),
            IntlModern::IntlModernKind::RelativeTimeFormat,
            scriptContext,
            _u("Intl.RelativeTimeFormat.prototype.resolvedOptions"));

        DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
        IntlModern::SetDataProperty(result, _u("locale"), state->GetLocale(), scriptContext);
        IntlModern::SetDataProperty(result, _u("style"), state->GetStyle(), scriptContext);
        IntlModern::SetDataProperty(result, _u("numeric"), state->GetNumeric(), scriptContext);
        IntlModern::SetDataProperty(result, _u("numberingSystem"), state->GetNumberingSystem(), scriptContext);
        return result;
    }
} // namespace Js

#endif // INTL_ICU
