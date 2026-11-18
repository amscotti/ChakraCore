//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"
#include "Library/IntlModernShared.h"
#include "Library/IntlDisplayNames.h"

#ifdef INTL_ICU

// Ownership record (doc/memory-management.md section 3):
//
// Resource / creation call:   ULocaleDisplayNames* (uldn_openForContext) or
//                             UDateTimePatternGenerator* (udatpg_open, type dateTimeField only),
//                             created in IntlDisplayNames::EntryNewInstance after every user-visible
//                             coercion.
// Domain:                     ICU library heap; JS-visible state is a Recycler finalized object
//                             (RecyclerNewWithBarrierFinalized) reachable through the HiddenObject
//                             internal property of the Intl.DisplayNames instance.
// Owner / transfer point:     IntlDisplayNamesState is the sole owner. A handle is stored into the
//                             state immediately after the ICU call returns, with no throwing call in
//                             between; an unpublished state is simply finalized if construction fails.
// Release authority:          IntlDisplayNamesState::Dispose (reached from Finalize on the Recycler
//                             sweep or at shutdown). Idempotent: nulls each pointer and invalidates
//                             the magic so a stale lookup can no longer return the state.
// Borrow invalidation:        No ICU-owned buffer or Recycler character pointer is kept across a call
//                             that can run user code. In `of` the argument is coerced first, its ASCII
//                             bytes are then copied into a ScopedAsciiString, and every ICU call is
//                             made in that leaf window; results are copied into a fresh Recycler
//                             string before returning.
// Reentrancy:                 The prototype getter of newTarget, CanonicalizeLocaleList, the option
//                             getters and ToString of the code can run arbitrary JS (and GC). The state
//                             is created and published only after the last constructor coercion; `of`
//                             re-fetches the state from the receiver after ToString.
// Thread/context:             The owning ScriptContext's thread only; no ICU handle is shared.
// Failure/OOM:                Each ICU status is checked; allocation failure becomes a JS OOM error,
//                             "no data" becomes the fallback result.
// Tests:                      test/Intl/IntlDisplayNamesTest.js and test/intl402/DisplayNames.

namespace Js
{
    namespace
    {
        enum class DisplayNamesType : uint8
        {
            Language,
            Region,
            Script,
            Currency,
            Calendar,
            DateTimeField
        };

        enum class DisplayNamesStyle : uint8
        {
            Narrow,
            Short,
            Long
        };

        enum class DisplayNamesFallback : uint8
        {
            Code,
            None
        };

        enum class DisplayNamesLanguageDisplay : uint8
        {
            Dialect,
            Standard
        };

        // The order of these tables matches the enums above.
        const char16* const localeMatcherValues[] = { _u("lookup"), _u("best fit") };
        const char16* const styleValues[] = { _u("narrow"), _u("short"), _u("long") };
        const char16* const typeValues[] =
        {
            _u("language"), _u("region"), _u("script"), _u("currency"), _u("calendar"), _u("dateTimeField")
        };
        const char16* const fallbackValues[] = { _u("code"), _u("none") };
        const char16* const languageDisplayValues[] = { _u("dialect"), _u("standard") };

        struct DateTimeFieldEntry
        {
            const char* name;
            size_t nameLen;
            UDateTimePatternField field;
        };

        const DateTimeFieldEntry dateTimeFieldTable[] =
        {
            { "era", 3, UDATPG_ERA_FIELD },
            { "year", 4, UDATPG_YEAR_FIELD },
            { "quarter", 7, UDATPG_QUARTER_FIELD },
            { "month", 5, UDATPG_MONTH_FIELD },
            { "weekOfYear", 10, UDATPG_WEEK_OF_YEAR_FIELD },
            { "weekday", 7, UDATPG_WEEKDAY_FIELD },
            { "day", 3, UDATPG_DAY_FIELD },
            { "dayPeriod", 9, UDATPG_DAYPERIOD_FIELD },
            { "hour", 4, UDATPG_HOUR_FIELD },
            { "minute", 6, UDATPG_MINUTE_FIELD },
            { "second", 6, UDATPG_SECOND_FIELD },
            { "timeZoneName", 12, UDATPG_ZONE_FIELD }
        };

        bool FindDateTimeField(const char* name, size_t nameLen, UDateTimePatternField* outField)
        {
            for (size_t i = 0; i < _countof(dateTimeFieldTable); ++i)
            {
                if (dateTimeFieldTable[i].nameLen == nameLen && memcmp(dateTimeFieldTable[i].name, name, nameLen) == 0)
                {
                    if (outField != nullptr)
                    {
                        *outField = dateTimeFieldTable[i].field;
                    }
                    return true;
                }
            }
            return false;
        }

        // Matches an already validated option string against its allowed values. GetStringOption has
        // verified membership, so a miss can only mean a table/enum mismatch.
        size_t IndexOfValue(JavascriptString* value, const char16* const* values, size_t count)
        {
            Assert(value != nullptr);
            const char16* sz = value->GetSz();
            size_t len = value->GetLength();
            for (size_t i = 0; i < count; ++i)
            {
                if (PAL_wcslen(values[i]) == len && wmemcmp(sz, values[i], len) == 0)
                {
                    return i;
                }
            }
            AssertMsg(false, "Validated Intl.DisplayNames option value is not in its table");
            return 0;
        }

        // Growable NUL-terminated byte buffer: stack storage first, NoCheckHeap storage if larger.
        // Never aliases a Recycler string.
        class AsciiBuffer
        {
        private:
            char stackBuf[256];
            char* buf;
            size_t capacity;

        public:
            AsciiBuffer() : buf(stackBuf), capacity(sizeof(stackBuf))
            {
                stackBuf[0] = '\0';
            }

            ~AsciiBuffer()
            {
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(capacity, buf);
                }
            }

            AsciiBuffer(const AsciiBuffer&) = delete;
            AsciiBuffer& operator=(const AsciiBuffer&) = delete;

            char* Data() { return buf; }
            const char* Data() const { return buf; }

            // Ensures room for `count` bytes (including the terminator). Contents are not preserved.
            bool Reserve(size_t count)
            {
                if (count <= capacity)
                {
                    return true;
                }
                char* newBuf = NoCheckHeapNewArray(char, count);
                if (newBuf == nullptr)
                {
                    return false;
                }
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(capacity, buf);
                }
                buf = newBuf;
                capacity = count;
                buf[0] = '\0';
                return true;
            }
        };

        // UTF-16 result buffer used for ICU calls that fill caller storage. Owns its (optional) heap
        // block; never aliases a Recycler string.
        class UCharBuffer
        {
        private:
            UChar stackBuf[256];
            UChar* buf;
            int32_t capacity;
            int32_t length;

        public:
            UCharBuffer() : buf(stackBuf), capacity(static_cast<int32_t>(_countof(stackBuf))), length(0)
            {
                stackBuf[0] = 0;
            }

            ~UCharBuffer()
            {
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(static_cast<size_t>(capacity), buf);
                }
            }

            UCharBuffer(const UCharBuffer&) = delete;
            UCharBuffer& operator=(const UCharBuffer&) = delete;

            UChar* Data() { return buf; }
            int32_t Capacity() const { return capacity; }
            int32_t Length() const { return length; }
            void SetLength(int32_t newLength) { length = newLength; }

            // Contents are not preserved.
            bool Grow(int32_t newCapacity)
            {
                if (newCapacity <= capacity)
                {
                    return true;
                }
                UChar* newBuf = NoCheckHeapNewArray(UChar, static_cast<size_t>(newCapacity));
                if (newBuf == nullptr)
                {
                    return false;
                }
                if (buf != stackBuf)
                {
                    NoCheckHeapDeleteArray(static_cast<size_t>(capacity), buf);
                }
                buf = newBuf;
                capacity = newCapacity;
                buf[0] = 0;
                return true;
            }
        };

        // Calls an ICU function of the form `int32_t f(UChar* result, int32_t capacity, UErrorCode*)`
        // and grows the buffer once on overflow. Returns false when ICU has no data (any ICU failure
        // except allocation failure, which is reported as a JS OOM) or the result is empty.
        template <typename TFunc>
        bool FillUChars(UCharBuffer& out, ScriptContext* scriptContext, TFunc func)
        {
            UErrorCode status = U_ZERO_ERROR;
            int32_t len = func(out.Data(), out.Capacity(), &status);
            if (status == U_BUFFER_OVERFLOW_ERROR || (U_SUCCESS(status) && len >= out.Capacity()))
            {
                if (len <= 0 || len >= INT32_MAX / 2)
                {
                    return false;
                }
                if (!out.Grow(len + 1))
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                status = U_ZERO_ERROR;
                len = func(out.Data(), out.Capacity(), &status);
            }
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            if (U_FAILURE(status) || len <= 0 || len >= out.Capacity())
            {
                return false;
            }
            out.SetLength(len);
            return true;
        }

        // Converts a BCP 47 tag (validated and canonicalized, ASCII) to an ICU locale id. Returns false
        // when ICU cannot represent the entire tag.
        bool TagToIcuLocaleId(const char* tag, size_t tagLen, char* outId, size_t outCap)
        {
            if (tagLen == 0 || tagLen >= static_cast<size_t>(INT32_MAX) || outCap < 2)
            {
                return false;
            }
            UErrorCode status = U_ZERO_ERROR;
            int32_t parsedLen = 0;
            int32_t idLen = uloc_forLanguageTag(tag, outId, static_cast<int32_t>(outCap), &parsedLen, &status);
            if (U_FAILURE(status) ||
                status == U_STRING_NOT_TERMINATED_WARNING ||
                idLen <= 0 ||
                static_cast<size_t>(idLen) >= outCap ||
                static_cast<size_t>(parsedLen) != tagLen)
            {
                return false;
            }
            outId[idLen] = '\0';
            return true;
        }
    } // namespace

    // State of an Intl.DisplayNames instance. Reached only through TryGetState/RequireState on the
    // receiver's HiddenObject internal property.
    //
    // GC tracing: this is a Recycler "finalized" allocation, scanned like any other Recycler object,
    // so the JavascriptString* members use the write-barrier Field() wrapper and the object is
    // allocated with RecyclerNewWithBarrierFinalized. ICU handles and scalars are not GC references
    // (FieldNoBarrier). Dispose releases each ICU handle exactly once.
    class IntlDisplayNamesState : public IntlModern::IntlModernState
    {
    private:
        Field(JavascriptString*) locale;
        Field(JavascriptString*) style;
        Field(JavascriptString*) type;
        Field(JavascriptString*) fallback;
        Field(JavascriptString*) languageDisplay;

        FieldNoBarrier(DisplayNamesType) typeKind;
        FieldNoBarrier(DisplayNamesStyle) styleKind;
        FieldNoBarrier(DisplayNamesFallback) fallbackKind;
        FieldNoBarrier(ULocaleDisplayNames*) displayNames;
        FieldNoBarrier(UDateTimePatternGenerator*) patternGenerator;
        char icuLocaleId[ULOC_FULLNAME_CAPACITY];

    public:
        IntlDisplayNamesState() :
            IntlModern::IntlModernState(IntlModern::IntlModernKind::DisplayNames),
            locale(nullptr),
            style(nullptr),
            type(nullptr),
            fallback(nullptr),
            languageDisplay(nullptr),
            typeKind(DisplayNamesType::Language),
            styleKind(DisplayNamesStyle::Long),
            fallbackKind(DisplayNamesFallback::Code),
            displayNames(nullptr),
            patternGenerator(nullptr)
        {
            icuLocaleId[0] = '\0';
        }

        JavascriptString* GetLocale() const { return locale; }
        JavascriptString* GetStyle() const { return style; }
        JavascriptString* GetType() const { return type; }
        JavascriptString* GetFallback() const { return fallback; }
        JavascriptString* GetLanguageDisplay() const { return languageDisplay; }
        DisplayNamesType GetTypeKind() const { return typeKind; }
        DisplayNamesStyle GetStyleKind() const { return styleKind; }
        DisplayNamesFallback GetFallbackKind() const { return fallbackKind; }
        ULocaleDisplayNames* GetDisplayNames() const { return displayNames; }
        UDateTimePatternGenerator* GetPatternGenerator() const { return patternGenerator; }
        const char* GetIcuLocaleId() const { return icuLocaleId; }

        void SetLocale(JavascriptString* value, const char* icuId)
        {
            locale = value;
            size_t idLen = strlen(icuId);
            Assert(idLen < sizeof(icuLocaleId));
            js_memcpy_s(icuLocaleId, sizeof(icuLocaleId), icuId, idLen + 1);
        }
        void SetStyle(JavascriptString* value, DisplayNamesStyle kind) { style = value; styleKind = kind; }
        void SetType(JavascriptString* value, DisplayNamesType kind) { type = value; typeKind = kind; }
        void SetFallback(JavascriptString* value, DisplayNamesFallback kind) { fallback = value; fallbackKind = kind; }
        void SetLanguageDisplay(JavascriptString* value) { languageDisplay = value; }

        // Take ownership of an ICU handle; the caller must not close it afterwards.
        void AdoptDisplayNames(ULocaleDisplayNames* handle)
        {
            Assert(displayNames == nullptr);
            displayNames = handle;
        }
        void AdoptPatternGenerator(UDateTimePatternGenerator* handle)
        {
            Assert(patternGenerator == nullptr);
            patternGenerator = handle;
        }

        virtual void Dispose(bool isShutdown) override
        {
            if (displayNames != nullptr)
            {
                uldn_close(displayNames);
                displayNames = nullptr;
            }
            if (patternGenerator != nullptr)
            {
                udatpg_close(patternGenerator);
                patternGenerator = nullptr;
            }
            InvalidateMagic();
        }
    };

    namespace
    {
        // Result of IsValidCodeForDisplayNames: canonical code bytes (NUL terminated, owned by `bytes`)
        // and the matching JS string used for the `fallback: "code"` result.
        struct CanonicalCode
        {
            AsciiBuffer bytes;
            size_t length;
            JavascriptString* jsString;

            CanonicalCode() : length(0), jsString(nullptr) {}
            const char* Data() const { return bytes.Data(); }
        };

        const char16* InvalidCodeMessage(DisplayNamesType type)
        {
            switch (type)
            {
            case DisplayNamesType::Language:
                return _u("Invalid language code for Intl.DisplayNames.prototype.of");
            case DisplayNamesType::Region:
                return _u("Invalid region code for Intl.DisplayNames.prototype.of");
            case DisplayNamesType::Script:
                return _u("Invalid script code for Intl.DisplayNames.prototype.of");
            case DisplayNamesType::Currency:
                return _u("Invalid currency code for Intl.DisplayNames.prototype.of");
            case DisplayNamesType::Calendar:
                return _u("Invalid calendar code for Intl.DisplayNames.prototype.of");
            default:
                return _u("Invalid dateTimeField code for Intl.DisplayNames.prototype.of");
            }
        }

        enum class CaseMode
        {
            Lower,
            Upper,
            Title
        };

        void StoreCased(const char* src, size_t len, CaseMode mode, CanonicalCode& out, ScriptContext* scriptContext)
        {
            if (!out.bytes.Reserve(len + 1))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            char* dst = out.bytes.Data();
            for (size_t i = 0; i < len; ++i)
            {
                char ch = src[i];
                if (mode == CaseMode::Upper || (mode == CaseMode::Title && i == 0))
                {
                    dst[i] = IntlModern::ToAsciiUpper(ch);
                }
                else
                {
                    dst[i] = IntlModern::ToAsciiLower(ch);
                }
            }
            dst[len] = '\0';
            out.length = len;
        }

        // IsValidCodeForDisplayNames (ECMA-402): throws RangeError for an invalid code and otherwise
        // returns the canonicalized code. `rawCode` is the copied ASCII of the coerced argument.
        void ValidateAndCanonicalizeCode(
            DisplayNamesType type,
            const char* rawCode,
            size_t rawLen,
            ScriptContext* scriptContext,
            CanonicalCode& out)
        {
            bool valid = false;

            switch (type)
            {
            case DisplayNamesType::Language:
            {
                // Must be a unicode_language_id (no -u-/-t-/-x- extensions, not "root") and structurally
                // valid; then canonicalized as a Unicode locale id.
                if (IntlModern::IsValidUnicodeLanguageId(rawCode, rawLen))
                {
                    JavascriptString* canonical = nullptr;
                    if (IntlModern::TryCanonicalizeUnicodeLocaleId(rawCode, rawLen, scriptContext, &canonical) &&
                        canonical != nullptr)
                    {
                        // The canonical tag is plain ASCII; copy it out of the Recycler string right away.
                        IntlModern::ScopedAsciiString ascii;
                        if (!ascii.Init(canonical))
                        {
                            if (ascii.IsOom())
                            {
                                JavascriptError::ThrowOutOfMemoryError(scriptContext);
                            }
                        }
                        else
                        {
                            if (!out.bytes.Reserve(ascii.Length() + 1))
                            {
                                JavascriptError::ThrowOutOfMemoryError(scriptContext);
                            }
                            js_memcpy_s(out.bytes.Data(), ascii.Length() + 1, ascii.Data(), ascii.Length() + 1);
                            out.length = ascii.Length();
                            out.jsString = canonical;
                            valid = true;
                        }
                    }
                }
                break;
            }
            case DisplayNamesType::Region:
                if (IntlModern::IsUnicodeRegionSubtag(rawCode, rawLen))
                {
                    StoreCased(rawCode, rawLen, CaseMode::Upper, out, scriptContext);
                    valid = true;
                }
                break;
            case DisplayNamesType::Script:
                if (IntlModern::IsUnicodeScriptSubtag(rawCode, rawLen))
                {
                    StoreCased(rawCode, rawLen, CaseMode::Title, out, scriptContext);
                    valid = true;
                }
                break;
            case DisplayNamesType::Currency:
                if (rawLen == 3 &&
                    IntlModern::IsAsciiAlpha(rawCode[0]) &&
                    IntlModern::IsAsciiAlpha(rawCode[1]) &&
                    IntlModern::IsAsciiAlpha(rawCode[2]))
                {
                    StoreCased(rawCode, rawLen, CaseMode::Upper, out, scriptContext);
                    valid = true;
                }
                break;
            case DisplayNamesType::Calendar:
                // type = alphanum{3,8} (sep alphanum{3,8})*
                if (IntlModern::IsUnicodeTypeNonterminal(rawCode, rawLen))
                {
                    StoreCased(rawCode, rawLen, CaseMode::Lower, out, scriptContext);
                    valid = true;
                }
                break;
            case DisplayNamesType::DateTimeField:
                if (FindDateTimeField(rawCode, rawLen, nullptr))
                {
                    // Field names are camel case and matched exactly: keep the spelling.
                    if (!out.bytes.Reserve(rawLen + 1))
                    {
                        JavascriptError::ThrowOutOfMemoryError(scriptContext);
                    }
                    js_memcpy_s(out.bytes.Data(), rawLen + 1, rawCode, rawLen);
                    out.bytes.Data()[rawLen] = '\0';
                    out.length = rawLen;
                    valid = true;
                }
                break;
            }

            if (!valid)
            {
                IntlModern::ThrowRangeErrorMessage(scriptContext, InvalidCodeMessage(type));
            }
            if (out.jsString == nullptr)
            {
                out.jsString = IntlModern::CreateAsciiString(out.Data(), out.length, scriptContext);
            }
        }

        // Fetches one ucurr_getName result. Returns false when ICU has no usable name. `isCodeEcho`
        // reports ICU's "no data" signature: the code itself with U_USING_DEFAULT_WARNING.
        bool QueryCurrencyName(
            const UChar* currency,
            const char* icuLocaleId,
            UCurrNameStyle nameStyle,
            ScriptContext* scriptContext,
            const UChar** outName,
            int32_t* outLen,
            bool* isCodeEcho)
        {
            UErrorCode status = U_ZERO_ERROR;
            UBool isChoice = FALSE;
            int32_t nameLen = 0;
            const UChar* name = ucurr_getName(currency, icuLocaleId, nameStyle, &isChoice, &nameLen, &status);
            if (status == U_MEMORY_ALLOCATION_ERROR)
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            if (U_FAILURE(status) || name == nullptr || nameLen <= 0)
            {
                return false;
            }
            *outName = name;
            *outLen = nameLen;
            *isCodeEcho = status == U_USING_DEFAULT_WARNING && nameLen == 3 &&
                name[0] == currency[0] && name[1] == currency[1] && name[2] == currency[2];
            return true;
        }

        bool LookupCurrencyName(
            IntlDisplayNamesState* state,
            const CanonicalCode& code,
            UCharBuffer& out,
            ScriptContext* scriptContext)
        {
            if (code.length != 3)
            {
                return false;
            }
            UChar currency[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < 3; ++i)
            {
                currency[i] = static_cast<UChar>(static_cast<unsigned char>(code.Data()[i]));
            }

            const UChar* name = nullptr;
            int32_t nameLen = 0;
            bool isCodeEcho = false;
            DisplayNamesStyle style = state->GetStyleKind();
            const char* locale = state->GetIcuLocaleId();

            if (style != DisplayNamesStyle::Long)
            {
                UCurrNameStyle symbolStyle = (style == DisplayNamesStyle::Narrow) ? UCURR_NARROW_SYMBOL_NAME : UCURR_SYMBOL_NAME;
                bool haveSymbol = QueryCurrencyName(currency, locale, symbolStyle, scriptContext, &name, &nameLen, &isCodeEcho);
                if (haveSymbol && !isCodeEcho)
                {
                    // fall through to copy
                }
                else
                {
                    // Either no symbol, or the symbol is just the code. The code is real data only if the
                    // currency has a long name.
                    const UChar* longName = nullptr;
                    int32_t longLen = 0;
                    bool longEcho = false;
                    if (!QueryCurrencyName(currency, locale, UCURR_LONG_NAME, scriptContext, &longName, &longLen, &longEcho) || longEcho)
                    {
                        return false;
                    }
                    if (!haveSymbol)
                    {
                        name = longName;
                        nameLen = longLen;
                    }
                }
            }
            else
            {
                if (!QueryCurrencyName(currency, locale, UCURR_LONG_NAME, scriptContext, &name, &nameLen, &isCodeEcho) || isCodeEcho)
                {
                    return false;
                }
            }

            if (nameLen >= out.Capacity() && !out.Grow(nameLen + 1))
            {
                JavascriptError::ThrowOutOfMemoryError(scriptContext);
            }
            js_memcpy_s(out.Data(), static_cast<size_t>(out.Capacity()) * sizeof(UChar), name, static_cast<size_t>(nameLen) * sizeof(UChar));
            out.Data()[nameLen] = 0;
            out.SetLength(nameLen);
            return true;
        }

        // Looks up the display name for an already validated, canonical code. Returns false if ICU has
        // no data for it (the caller then applies the `fallback` option).
        bool LookupDisplayName(
            IntlDisplayNamesState* state,
            const CanonicalCode& code,
            UCharBuffer& out,
            ScriptContext* scriptContext)
        {
            const char* codeBytes = code.Data();
            switch (state->GetTypeKind())
            {
            case DisplayNamesType::Language:
            {
                char icuId[ULOC_FULLNAME_CAPACITY];
                if (!TagToIcuLocaleId(codeBytes, code.length, icuId, sizeof(icuId)))
                {
                    return false;
                }
                ULocaleDisplayNames* ldn = state->GetDisplayNames();
                return FillUChars(out, scriptContext, [&](UChar* buf, int32_t cap, UErrorCode* status) -> int32_t
                {
                    return uldn_localeDisplayName(ldn, icuId, buf, cap, status);
                });
            }
            case DisplayNamesType::Region:
            {
                ULocaleDisplayNames* ldn = state->GetDisplayNames();
                return FillUChars(out, scriptContext, [&](UChar* buf, int32_t cap, UErrorCode* status) -> int32_t
                {
                    return uldn_regionDisplayName(ldn, codeBytes, buf, cap, status);
                });
            }
            case DisplayNamesType::Script:
            {
                ULocaleDisplayNames* ldn = state->GetDisplayNames();
                return FillUChars(out, scriptContext, [&](UChar* buf, int32_t cap, UErrorCode* status) -> int32_t
                {
                    return uldn_scriptDisplayName(ldn, codeBytes, buf, cap, status);
                });
            }
            case DisplayNamesType::Calendar:
            {
                // ICU keys calendar data by its legacy type name ("gregory" -> "gregorian").
                const char* legacyType = uloc_toLegacyType("calendar", codeBytes);
                if (legacyType == nullptr || legacyType[0] == '\0')
                {
                    legacyType = codeBytes;
                }
                ULocaleDisplayNames* ldn = state->GetDisplayNames();
                return FillUChars(out, scriptContext, [&](UChar* buf, int32_t cap, UErrorCode* status) -> int32_t
                {
                    return uldn_keyValueDisplayName(ldn, "calendar", legacyType, buf, cap, status);
                });
            }
            case DisplayNamesType::Currency:
                return LookupCurrencyName(state, code, out, scriptContext);
            case DisplayNamesType::DateTimeField:
            {
                UDateTimePatternGenerator* generator = state->GetPatternGenerator();
                UDateTimePatternField field = UDATPG_ERA_FIELD;
                if (generator == nullptr || !FindDateTimeField(codeBytes, code.length, &field))
                {
                    return false;
                }
                UDateTimePGDisplayWidth width = UDATPG_WIDE;
                switch (state->GetStyleKind())
                {
                case DisplayNamesStyle::Narrow:
                    width = UDATPG_NARROW;
                    break;
                case DisplayNamesStyle::Short:
                    width = UDATPG_ABBREVIATED;
                    break;
                default:
                    width = UDATPG_WIDE;
                    break;
                }
                return FillUChars(out, scriptContext, [&](UChar* buf, int32_t cap, UErrorCode* status) -> int32_t
                {
                    return udatpg_getFieldDisplayName(generator, field, width, buf, cap, status);
                });
            }
            }
            return false;
        }
    } // namespace

    void IntlDisplayNames::Initialize(JavascriptLibrary* library, DynamicObject* intlObject)
    {
        Assert(library != nullptr && intlObject != nullptr);
        RuntimeFunction* ctor = nullptr;
        DynamicObject* proto = IntlModern::RegisterIntlClass(
            library,
            intlObject,
            _u("DisplayNames"),
            &EntryInfo::NewInstance,
            2,
            _u("Intl.DisplayNames"),
            nullptr,
            &ctor);

        library->SetIntlDisplayNamesPrototype(proto);

        library->AddFunctionToLibraryObject(ctor, PropertyIds::supportedLocalesOf, &EntryInfo::SupportedLocalesOf, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::of, &EntryInfo::Of, 1);
        library->AddFunctionToLibraryObject(proto, PropertyIds::resolvedOptions, &EntryInfo::ResolvedOptions, 0);
    }

    Var IntlDisplayNames::EntryNewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        // 1. If NewTarget is undefined, throw a TypeError exception.
        if (!(callInfo.Flags & CallFlags_New))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DisplayNames"));
        }
        Var newTarget = args.HasNewTarget() ? args.GetNewTarget() : function;
        if (newTarget == nullptr || JavascriptOperators::IsUndefined(newTarget))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("Intl.DisplayNames"));
        }

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var options = args.Info.Count > 2 ? args[2] : undefinedVar;

        // 2. OrdinaryCreateFromConstructor (reads newTarget.prototype: user code).
        DynamicObject* instance = IntlModern::OrdinaryCreateFromConstructor(
            newTarget, scriptContext, &JavascriptLibrary::GetIntlDisplayNamesPrototype);

        // 3. CanonicalizeLocaleList(locales) (user code through the locales object).
        JavascriptArray* requestedLocales = IntlModern::CanonicalizeLocaleList(locales, scriptContext);

        // 4. If options is undefined, throw a TypeError exception.
        if (JavascriptOperators::IsUndefined(options))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }

        // 5. Set options to ? GetOptionsObject(options).
        RecyclableObject* optionsObj = IntlModern::GetOptionsObject(options, scriptContext);
        Assert(optionsObj != nullptr);

        // 6-7. localeMatcher.
        JavascriptString* matcher = IntlModern::GetStringOption(
            optionsObj,
            PropertyIds::localeMatcher,
            _u("localeMatcher"),
            localeMatcherValues,
            _countof(localeMatcherValues),
            _u("best fit"),
            scriptContext);

        // 8. ResolveLocale. Intl.DisplayNames has no relevant extension keys.
        IntlModern::ResolvedLocale resolved = IntlModern::ResolveLocale(
            IntlModern::IntlModernKind::DisplayNames,
            requestedLocales,
            matcher != nullptr ? matcher->GetSz() : nullptr,
            nullptr,
            0,
            nullptr,
            scriptContext);
        JavascriptString* resolvedLocale = resolved.locale;
        Assert(resolvedLocale != nullptr);

        // 9-10. style.
        JavascriptString* styleOption = IntlModern::GetStringOption(
            optionsObj, _u("style"), styleValues, _countof(styleValues), _u("long"), scriptContext);
        DisplayNamesStyle styleKind = static_cast<DisplayNamesStyle>(
            IndexOfValue(styleOption, styleValues, _countof(styleValues)));

        // 11-12. type is required.
        JavascriptString* typeOption = IntlModern::GetStringOption(
            optionsObj, _u("type"), typeValues, _countof(typeValues), nullptr, scriptContext);
        if (typeOption == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject);
        }
        DisplayNamesType typeKind = static_cast<DisplayNamesType>(
            IndexOfValue(typeOption, typeValues, _countof(typeValues)));

        // 13-14. fallback.
        JavascriptString* fallbackOption = IntlModern::GetStringOption(
            optionsObj, _u("fallback"), fallbackValues, _countof(fallbackValues), _u("code"), scriptContext);
        DisplayNamesFallback fallbackKind = static_cast<DisplayNamesFallback>(
            IndexOfValue(fallbackOption, fallbackValues, _countof(fallbackValues)));

        // 15-16. languageDisplay is always read; it is recorded only for type "language".
        JavascriptString* languageDisplayOption = IntlModern::GetStringOption(
            optionsObj, _u("languageDisplay"), languageDisplayValues, _countof(languageDisplayValues), _u("dialect"), scriptContext);
        DisplayNamesLanguageDisplay languageDisplayKind = static_cast<DisplayNamesLanguageDisplay>(
            IndexOfValue(languageDisplayOption, languageDisplayValues, _countof(languageDisplayValues)));

        // The last user-visible step is done. Build the native state from JS-independent data only.
        char icuLocaleId[ULOC_FULLNAME_CAPACITY] = { 0 };
        {
            IntlModern::ScopedAsciiString localeAscii;
            if (!localeAscii.Init(resolvedLocale))
            {
                if (localeAscii.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Invalid locale resolved for Intl.DisplayNames"));
            }
            if (!TagToIcuLocaleId(localeAscii.Data(), localeAscii.Length(), icuLocaleId, sizeof(icuLocaleId)))
            {
                js_memcpy_s(icuLocaleId, sizeof(icuLocaleId), "en", 3);
            }
        }

        IntlDisplayNamesState* state = RecyclerNewWithBarrierFinalized(scriptContext->GetRecycler(), IntlDisplayNamesState);
        state->SetLocale(resolvedLocale, icuLocaleId);
        state->SetStyle(JavascriptString::NewCopySz(styleValues[static_cast<size_t>(styleKind)], scriptContext), styleKind);
        state->SetType(JavascriptString::NewCopySz(typeValues[static_cast<size_t>(typeKind)], scriptContext), typeKind);
        state->SetFallback(JavascriptString::NewCopySz(fallbackValues[static_cast<size_t>(fallbackKind)], scriptContext), fallbackKind);
        if (typeKind == DisplayNamesType::Language)
        {
            state->SetLanguageDisplay(JavascriptString::NewCopySz(
                languageDisplayValues[static_cast<size_t>(languageDisplayKind)], scriptContext));
        }

        if (typeKind == DisplayNamesType::DateTimeField)
        {
            UErrorCode status = U_ZERO_ERROR;
            UDateTimePatternGenerator* generator = udatpg_open(icuLocaleId, &status);
            if (U_FAILURE(status) || generator == nullptr)
            {
                if (generator != nullptr)
                {
                    udatpg_close(generator);
                }
                if (status == U_MEMORY_ALLOCATION_ERROR)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Unable to initialize Intl.DisplayNames"));
            }
            // No throwing call between the successful open and the transfer to the state.
            state->AdoptPatternGenerator(generator);
        }
        else
        {
            // Only "language" can yield partial names, so only it relies on ICU substitution (with
            // fallback "code" ICU fills unknown subtags with the code itself). Every other type asks
            // ICU for NO_SUBSTITUTE so "no data" is a detectable failure that EntryOf maps to the
            // canonical code or undefined.
            UDisplayContext contexts[4];
            contexts[0] = (typeKind == DisplayNamesType::Language && languageDisplayKind == DisplayNamesLanguageDisplay::Dialect)
                ? UDISPCTX_DIALECT_NAMES : UDISPCTX_STANDARD_NAMES;
            contexts[1] = UDISPCTX_CAPITALIZATION_NONE;
            contexts[2] = (styleKind == DisplayNamesStyle::Long) ? UDISPCTX_LENGTH_FULL : UDISPCTX_LENGTH_SHORT;
            contexts[3] = (typeKind == DisplayNamesType::Language && fallbackKind == DisplayNamesFallback::Code)
                ? UDISPCTX_SUBSTITUTE : UDISPCTX_NO_SUBSTITUTE;

            UErrorCode status = U_ZERO_ERROR;
            ULocaleDisplayNames* ldn = uldn_openForContext(icuLocaleId, contexts, static_cast<int32_t>(_countof(contexts)), &status);
            if (U_FAILURE(status) || ldn == nullptr)
            {
                if (ldn != nullptr)
                {
                    uldn_close(ldn);
                }
                if (status == U_MEMORY_ALLOCATION_ERROR)
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, _u("Unable to initialize Intl.DisplayNames"));
            }
            // No throwing call between the successful open and the transfer to the state.
            state->AdoptDisplayNames(ldn);
        }

        IntlModern::SetState(instance, state);
        return instance;
    }

    Var IntlDisplayNames::EntrySupportedLocalesOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var locales = args.Info.Count > 1 ? args[1] : undefinedVar;
        Var options = args.Info.Count > 2 ? args[2] : undefinedVar;
        return IntlModern::SupportedLocales(IntlModern::IntlModernKind::DisplayNames, locales, options, scriptContext);
    }

    Var IntlDisplayNames::EntryOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var thisVar = args.Info.Count > 0 ? args[0] : undefinedVar;
        IntlModern::RequireState<IntlDisplayNamesState>(
            thisVar, IntlModern::IntlModernKind::DisplayNames, scriptContext, _u("Intl.DisplayNames.prototype.of"));

        // 3. Let code be ? ToString(code). May run user code, GC or reenter this object.
        Var codeVar = args.Info.Count > 1 ? args[1] : undefinedVar;
        JavascriptString* codeString = JavascriptConversion::ToString(codeVar, scriptContext);

        // Re-acquire the state from the receiver after all coercions.
        IntlDisplayNamesState* state = IntlModern::RequireState<IntlDisplayNamesState>(
            thisVar, IntlModern::IntlModernKind::DisplayNames, scriptContext, _u("Intl.DisplayNames.prototype.of"));

        // The rest is a leaf window: no user code runs and no Recycler character pointer is retained;
        // the argument bytes are copied into a ScopedAsciiString before use.
        CanonicalCode code;
        {
            IntlModern::ScopedAsciiString rawCode;
            if (!rawCode.Init(codeString))
            {
                if (rawCode.IsOom())
                {
                    JavascriptError::ThrowOutOfMemoryError(scriptContext);
                }
                IntlModern::ThrowRangeErrorMessage(scriptContext, InvalidCodeMessage(state->GetTypeKind()));
            }
            ValidateAndCanonicalizeCode(state->GetTypeKind(), rawCode.Data(), rawCode.Length(), scriptContext, code);
        }

        UCharBuffer result;
        if (LookupDisplayName(state, code, result, scriptContext))
        {
            return IntlModern::CreateUtf16String(result.Data(), result.Length(), scriptContext);
        }

        if (state->GetFallbackKind() == DisplayNamesFallback::Code)
        {
            return code.jsString;
        }
        return undefinedVar;
    }

    Var IntlDisplayNames::EntryResolvedOptions(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);
        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Var undefinedVar = scriptContext->GetLibrary()->GetUndefined();
        Var thisVar = args.Info.Count > 0 ? args[0] : undefinedVar;
        IntlDisplayNamesState* state = IntlModern::RequireState<IntlDisplayNamesState>(
            thisVar, IntlModern::IntlModernKind::DisplayNames, scriptContext, _u("Intl.DisplayNames.prototype.resolvedOptions"));

        DynamicObject* result = IntlModern::CreatePlainDataObject(scriptContext);
        IntlModern::SetDataProperty(result, _u("locale"), state->GetLocale(), scriptContext);
        IntlModern::SetDataProperty(result, _u("style"), state->GetStyle(), scriptContext);
        IntlModern::SetDataProperty(result, _u("type"), state->GetType(), scriptContext);
        IntlModern::SetDataProperty(result, _u("fallback"), state->GetFallback(), scriptContext);
        if (state->GetTypeKind() == DisplayNamesType::Language)
        {
            IntlModern::SetDataProperty(result, _u("languageDisplay"), state->GetLanguageDisplay(), scriptContext);
        }
        return result;
    }
} // namespace Js

#endif // INTL_ICU
